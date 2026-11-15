// Replay of a PCSX2 GS dump (`.gs`, uncompressed v2+ format) directly into the
// GS: the GIF FIFO transfers are fed through the normal GIF path, the privileged
// register snapshots are applied to the display circuit, and one PPM per vblank
// is written so the result can be compared against the screenshot that PCSX2
// embeds in the same dump.
//
// Layout of the file (see PCSX2 pcsx2/GS/GSDump.cpp):
//   u32 fake_crc (0xFFFFFFFF)
//   u32 header_size            // sizeof(GSDumpHeader) + serial + screenshot
//   GSDumpHeader { state_version, state_size, serial_offset, serial_size, crc,
//                  screenshot_width, screenshot_height, screenshot_offset,
//                  screenshot_size }
//   serial, screenshot (w*h*4 bytes, RGBA)
//   state data (state_size bytes)   // PCSX2 savestate, not needed for replay
//   GSPrivRegSet (0x2000 bytes)
//   packets: u8 type
//     type 0: u8 path, u32 bytes, payload (GIF FIFO data, multiple of 16)
//     type 1: u8 field                       (vblank)
//     type 2: u32 bytes                      (VRAM->EE read, ignored)
//     type 3: 0x2000 bytes                   (GSPrivRegSet snapshot)

#include "ps2/system.hpp"

#include "gs/gif.hpp"
#include "gs/gs.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ps2 {
namespace {

std::uint32_t rd32(const std::uint8_t *p) {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

std::uint64_t rd64(const std::uint8_t *p) {
    std::uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

void write_ppm(const std::string &dir, std::uint64_t index, const std::vector<std::uint32_t> &rgba, std::uint32_t w, std::uint32_t h) {
    char name[512];
    std::snprintf(name, sizeof name, "%s/frame_%05llu.ppm", dir.c_str(), static_cast<unsigned long long>(index));
    std::FILE *f = std::fopen(name, "wb");
    if (!f) {
        std::fprintf(stderr, "[gsdump] cannot write %s\n", name);
        return;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (std::uint32_t p : rgba) {
        const std::uint8_t px[3] = {static_cast<std::uint8_t>(p), static_cast<std::uint8_t>(p >> 8), static_cast<std::uint8_t>(p >> 16)};
        std::fwrite(px, 1, 3, f);
    }
    std::fclose(f);
}

// Apply a GSPrivRegSet snapshot: 8-byte register followed by 8 bytes of padding,
// matching gs::write_privileged()'s 16-byte stride.
void apply_priv_regs(const std::uint8_t *regs) {
    for (std::uint32_t off = 0; off < 0x100u; off += 0x10u) gs::write_privileged(off, rd64(regs + off));
}

void dump_field(std::uint64_t index) {
    gs::flush_and_wait();
    std::vector<std::uint32_t> rgba;
    std::uint32_t w = 0, h = 0;
    if (!gs::display(rgba, w, h)) {
        std::fprintf(stderr, "[gsdump] field %llu: display circuit inactive\n", static_cast<unsigned long long>(index));
        return;
    }
    if (g_options.frames_dir.empty()) return;
    write_ppm(g_options.frames_dir, index, rgba, w, h);
    std::fprintf(stderr, "[gsdump] field %llu -> %ux%u\n", static_cast<unsigned long long>(index), w, h);
}

} // namespace

bool replay_gs_dump(const std::string &path) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "[gsdump] cannot open %s\n", path.c_str());
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long length = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (length <= 0) {
        std::fclose(f);
        return false;
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
    const std::size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    data.resize(got);
    if (data.size() < 0x40) {
        std::fprintf(stderr, "[gsdump] %s is too small for a GS dump\n", path.c_str());
        return false;
    }

    if (rd32(data.data()) != 0xFFFFFFFFu) {
        std::fprintf(stderr, "[gsdump] %s is not a PCSX2 GS dump (bad magic)\n", path.c_str());
        return false;
    }
    const std::uint8_t *hdr = data.data() + 8;
    const std::uint32_t state_version = rd32(hdr + 0);
    const std::uint32_t state_size = rd32(hdr + 4);
    const std::uint32_t serial_size = rd32(hdr + 12);
    const std::uint32_t shot_w = rd32(hdr + 20), shot_h = rd32(hdr + 24);
    const std::uint32_t header_size = rd32(data.data() + 4);

    const std::size_t regs_off = 8 + header_size + state_size;
    const std::size_t packets = regs_off + 0x2000;
    if (packets >= data.size()) {
        std::fprintf(stderr, "[gsdump] truncated dump (state %u, header %u)\n", state_size, header_size);
        return false;
    }
    const std::string serial(reinterpret_cast<const char *>(data.data() + 8 + 36), std::min<std::size_t>(serial_size, 32));
    std::fprintf(stderr, "[gsdump] %s: state v%u (%u bytes), screenshot %ux%u, packets at 0x%zX\n",
                 path.c_str(), state_version, state_size, shot_w, shot_h, packets);

    // Fresh GS: zeroed VRAM/state. Textures uploaded before the dump start are
    // missing, but this game re-uploads its material set every frame.
    gs::init();
    gs::render_reset();
    gif::reset();

    std::uint64_t fields = 0, transfers = 0, bytes = 0;
    std::size_t off = packets;
    while (off < data.size()) {
        const std::uint8_t type = data[off];
        if (type == 0) {
            if (off + 6 > data.size()) break;
            const std::uint8_t gif_path = data[off + 1];
            const std::uint32_t size = rd32(data.data() + off + 2);
            off += 6;
            if (off + size > data.size()) {
                std::fprintf(stderr, "[gsdump] truncated transfer at 0x%zX\n", off);
                break;
            }
            if (gif_path < 4u && size && (size & 15u) == 0) {
                gif::transfer(gif_path, data.data() + off, size / 16);
                ++transfers;
                bytes += size;
            }
            off += size;
        } else if (type == 1) {
            if (off + 2 > data.size()) break;
            off += 2;
            dump_field(fields);
            ++fields;
        } else if (type == 2) {
            if (off + 5 > data.size()) break;
            const std::uint32_t size = rd32(data.data() + off + 1);
            off += 5;
            // VRAM->EE reads: the FIFO data is not part of the dump.
            (void)size;
        } else if (type == 3) {
            if (off + 1 + 0x2000 > data.size()) break;
            apply_priv_regs(data.data() + off + 1);
            off += 1 + 0x2000;
        } else {
            std::fprintf(stderr, "[gsdump] unknown packet type %u at 0x%zX\n", type, off);
            break;
        }
    }

    std::fprintf(stderr, "[gsdump] replayed %llu transfers (%llu KB), %llu fields\n",
                 static_cast<unsigned long long>(transfers), static_cast<unsigned long long>(bytes >> 10),
                 static_cast<unsigned long long>(fields));
    gs::flush_and_wait();
    return true;
}

} // namespace ps2
