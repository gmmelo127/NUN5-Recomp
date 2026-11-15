#include "sio2.hpp"

#include "hw.hpp"
#include "iop.hpp"
#include "ps2/system.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <vector>

namespace ps2::iop::sio2 {

namespace {

constexpr std::uint8_t kDigital = 0x41, kAnalog = 0x73, kPressure = 0x79, kConfig = 0xF3;

struct Pad {
    bool connected = false;
    std::uint16_t buttons = 0;
    std::uint8_t axes[4] = {0x80, 0x80, 0x80, 0x80};
    std::uint8_t mode = kDigital;
    bool config = false;
    bool locked = false;
    std::uint8_t poll_mask[3] = {0xFF, 0xFF, 0x03};
};

constexpr std::uint8_t kLibpadMotors[6] = {0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF};

struct Feel {
    std::uint8_t motors[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    std::uint8_t pressure[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    std::atomic<std::uint16_t> rumble{0};
};

struct Dma {
    bool armed = false;
    std::uint32_t madr = 0, block_words = 0, blocks = 0;
};

constexpr std::uint32_t kPageBytes = 528, kPages = 0x4000, kBlockPages = 16;
constexpr std::size_t kCardBytes = static_cast<std::size_t>(kPageBytes) * kPages;

struct Card {
    bool present = false;
    std::vector<std::uint8_t> data;
    std::size_t address = 0;
    std::uint8_t term = 0x55;
    bool dirty = false;
    std::uint32_t quiet = 0;
};

std::uint32_t g_send3[16];
std::uint32_t g_ctrl1[4], g_ctrl2[4];
std::uint32_t g_index = 0;
std::uint32_t g_stat = 0;
std::deque<std::uint8_t> g_in, g_out;
Pad g_pads[2];
// which ports the host has a controller in; kept out of save states so loading one does not unplug a pad
bool g_plugged[2] = {true, false};
Feel g_feel[2];
Card g_card;
Dma g_dma[2];

void no_device() { g_stat |= 0xC000u | (0x10000u << (g_index & 15u)); }

void controller(int port, std::size_t at, std::uint32_t out_size) {
    const std::vector<std::uint8_t> in(g_in.begin(), g_in.end());
    if (port > 1 || !g_pads[port].connected || in.empty() || in[0] != 0x01) {
        no_device();
        return;
    }
    Pad &p = g_pads[port];
    Feel &f = g_feel[port];
    const std::uint8_t cmd = in.size() > 1 ? in[1] : 0;
    auto put = [&](std::size_t i, std::uint8_t v) {
        if (i < out_size) g_out[at + i] = v;
    };
    put(0, 0xFF);
    put(1, p.config ? kConfig : p.mode);
    put(2, 0x5A);
    const std::uint16_t raw = static_cast<std::uint16_t>(~p.buttons);
    auto data = [&]() {
        put(3, static_cast<std::uint8_t>(raw));
        put(4, static_cast<std::uint8_t>(raw >> 8));
        if (p.mode != kDigital) {
            put(5, p.axes[0]);
            put(6, p.axes[1]);
            put(7, p.axes[2]);
            put(8, p.axes[3]);
        }
        if (p.mode == kPressure) {
            static const int kBits[12] = {5, 7, 4, 6, 12, 13, 14, 15, 10, 11, 8, 9};
            for (int i = 0; i < 12; ++i) put(9 + static_cast<std::size_t>(i), (p.buttons >> kBits[i]) & 1u ? f.pressure[kBits[i]] : 0x00);
        }
    };
    switch (cmd) {
    case 0x40:
        put(3, 0); put(4, 0); put(5, 2); put(6, 0); put(7, 0); put(8, 0x5A);
        break;
    case 0x41:
        if (p.mode == kDigital) for (int i = 3; i < 9; ++i) put(static_cast<std::size_t>(i), 0);
        else { put(3, p.poll_mask[0]); put(4, p.poll_mask[1]); put(5, p.poll_mask[2]); put(6, 0); put(7, 0); put(8, 0x5A); }
        break;
    case 0x42: {
        data();
        std::uint8_t small = 0, large = 0;
        for (std::size_t i = 0; i < 6 && 3 + i < in.size(); ++i) {
            if (f.motors[i] == 0x00) small = in[3 + i] & 1u;
            if (f.motors[i] == 0x01) large = in[3 + i];
        }
        f.rumble.store(static_cast<std::uint16_t>(small | (large << 8)), std::memory_order_relaxed);
        break;
    }
    case 0x43:
        if (!p.config) data();
        else for (int i = 3; i < 9; ++i) put(static_cast<std::size_t>(i), 0);
        if (in.size() > 3) p.config = in[3] == 1;
        break;
    case 0x44:
        if (in.size() > 4) {
            p.mode = in[3] ? kAnalog : kDigital;
            p.locked = in[4] == 3;
        }
        for (int i = 3; i < 9; ++i) put(static_cast<std::size_t>(i), 0);
        break;
    case 0x45:
        put(3, 0x03); put(4, 0x02); put(5, p.mode == kDigital ? 0 : 1); put(6, 0x02); put(7, 0x01); put(8, 0x00);
        break;
    case 0x46:
        put(3, 0);
        if (in.size() > 3 && in[3] == 0) { put(4, 0x00); put(5, 0x01); put(6, 0x02); put(7, 0x00); put(8, 0x0A); }
        else { put(4, 0x00); put(5, 0x01); put(6, 0x01); put(7, 0x01); put(8, 0x14); }
        break;
    case 0x47:
        put(3, 0); put(4, 0x00); put(5, 0x02); put(6, 0x00); put(7, 0x01); put(8, 0x00);
        break;
    case 0x4C:
        put(3, 0);
        if (in.size() > 3 && in[3] == 0) { put(4, 0); put(5, 0); put(6, 0x04); put(7, 0); put(8, 0); }
        else { put(4, 0); put(5, 0); put(6, 0x07); put(7, 0); put(8, 0); }
        break;
    case 0x4D:
        for (int i = 3; i < 9; ++i) put(static_cast<std::size_t>(i), 0xFF);
        for (std::size_t i = 0; i < 6 && 3 + i < in.size(); ++i) f.motors[i] = in[3 + i];
        f.rumble.store(0, std::memory_order_relaxed);
        break;
    case 0x4F:
        if (in.size() > 5) {
            p.poll_mask[0] = in[3];
            p.poll_mask[1] = in[4];
            p.poll_mask[2] = in[5];
        }
        p.mode = kPressure;
        put(3, 0); put(4, 0); put(5, 0); put(6, 0); put(7, 0); put(8, 0x5A);
        break;
    default:
        data();
        break;
    }
}

std::uint8_t xor_of(const std::uint8_t *p, std::size_t n) {
    std::uint8_t v = 0;
    for (std::size_t i = 0; i < n; ++i) v ^= p[i];
    return v;
}

constexpr std::uint32_t kPageData = 512, kPagesPerCluster = 2;
constexpr std::uint32_t kClusters = 8192, kAllocOffset = 41, kAllocEnd = 8135;
constexpr std::uint32_t kBackupBlock2 = 1022;
constexpr std::size_t kCardDataBytes = static_cast<std::size_t>(kPageData) * kPages;

std::uint8_t parity8(std::uint8_t a) {
    a ^= static_cast<std::uint8_t>(a >> 1);
    a ^= static_cast<std::uint8_t>(a >> 2);
    a ^= static_cast<std::uint8_t>(a >> 4);
    return a & 1u;
}

struct EccTables {
    std::uint8_t parity[256];
    std::uint8_t column[256];
};

const EccTables &ecc_tables() {
    static const EccTables tables = [] {
        EccTables t{};
        const std::uint8_t masks[7] = {0x55, 0x33, 0x0F, 0x00, 0xAA, 0xCC, 0xF0};
        for (int b = 0; b < 256; ++b) {
            t.parity[b] = parity8(static_cast<std::uint8_t>(b));
            std::uint8_t mask = 0;
            for (int i = 0; i < 7; ++i) mask |= static_cast<std::uint8_t>(t.parity[b & masks[i]] << i);
            t.column[b] = mask;
        }
        return t;
    }();
    return tables;
}

void ecc128(const std::uint8_t *s, std::uint8_t *out) {
    const EccTables &t = ecc_tables();
    std::uint8_t column = 0x77, line0 = 0x7F, line1 = 0x7F;
    for (int i = 0; i < 128; ++i) {
        const std::uint8_t b = s[i];
        column ^= t.column[b];
        if (t.parity[b]) {
            line0 ^= static_cast<std::uint8_t>((~i) & 0x7F);
            line1 ^= static_cast<std::uint8_t>(i);
        }
    }
    out[0] = column;
    out[1] = static_cast<std::uint8_t>(line0 & 0x7F);
    out[2] = line1;
}

void ecc_page(std::uint8_t *page) {
    for (int i = 0; i < 4; ++i) ecc128(page + i * 128, page + kPageData + i * 3);
    std::memset(page + kPageData + 12, 0, 4);
}

void put16(std::uint8_t *p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}

void put32(std::uint8_t *p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

void write_cluster(std::vector<std::uint8_t> &card, std::uint32_t cluster, const std::uint8_t *src) {
    for (std::uint32_t page = 0; page < kPagesPerCluster; ++page) {
        std::memcpy(&card[(static_cast<std::size_t>(cluster) * kPagesPerCluster + page) * kPageBytes],
                    src + page * kPageData, kPageData);
    }
}

void write_dirent(std::uint8_t *p, std::uint16_t mode, std::uint32_t length, std::uint32_t fat, std::uint32_t parent, const char *name) {
    put16(p + 0, mode);
    put16(p + 2, 0);
    put32(p + 4, length);
    const std::uint8_t tod[8] = {0, 0, 0, 0, 1, 1, 0xE9, 0x07};
    std::memcpy(p + 8, tod, 8);
    put32(p + 16, fat);
    put32(p + 20, parent);
    std::memcpy(p + 24, tod, 8);
    put32(p + 32, 0);
    std::strncpy(reinterpret_cast<char *>(p + 64), name, 447);
}

void format_card(std::vector<std::uint8_t> &card) {
    card.assign(kCardBytes, 0);

    std::uint8_t sb[kPageData] = {};
    static const char magic[] = "Sony PS2 Memory Card Format ";
    std::memcpy(sb, magic, 28);
    std::memcpy(sb + 28, "1.2.0.0", 7);
    put16(sb + 0x28, static_cast<std::uint16_t>(kPageData));
    put16(sb + 0x2A, static_cast<std::uint16_t>(kPagesPerCluster));
    put16(sb + 0x2C, static_cast<std::uint16_t>(kBlockPages));
    put16(sb + 0x2E, 0xFF00);
    put32(sb + 0x30, kClusters);
    put32(sb + 0x34, kAllocOffset);
    put32(sb + 0x38, kAllocEnd);
    put32(sb + 0x3C, 0);
    put32(sb + 0x40, kPages / kBlockPages - 1);
    put32(sb + 0x44, kBackupBlock2);
    for (int i = 0; i < 32; ++i) put32(sb + 0x50 + i * 4, i == 0 ? 8u : 0u);
    for (int i = 0; i < 32; ++i) put32(sb + 0xD0 + i * 4, 0xFFFFFFFFu);
    sb[0x150] = 2;
    sb[0x151] = 0x2B;
    std::memcpy(&card[0], sb, kPageData);

    std::uint8_t cluster[1024];
    std::memset(cluster, 0xFF, sizeof(cluster));
    for (int i = 0; i < 32; ++i) put32(cluster + i * 4, static_cast<std::uint32_t>(9 + i));
    write_cluster(card, 8, cluster);

    for (std::uint32_t c = 9; c < 41; ++c) {
        for (int j = 0; j < 256; ++j) {
            const std::uint32_t n = (c - 9) * 256 + static_cast<std::uint32_t>(j);
            put32(cluster + j * 4, (n == 0 || n >= kAllocEnd) ? 0xFFFFFFFFu : 0x7FFFFFFFu);
        }
        write_cluster(card, c, cluster);
    }

    std::memset(cluster, 0, sizeof(cluster));
    write_dirent(cluster, 0x8427, 2, 0, 0, ".");
    write_dirent(cluster + kPageData, 0xA426, 0, 0, 0, "..");
    write_cluster(card, kAllocOffset, cluster);

    for (std::uint32_t page = 0; page < kPages; ++page) ecc_page(&card[static_cast<std::size_t>(page) * kPageBytes]);
    std::memset(&card[static_cast<std::size_t>(kBackupBlock2) * kBlockPages * kPageBytes], 0xFF,
                static_cast<std::size_t>(kBlockPages) * kPageBytes);
}

void load_card() {
    if (g_card.present || g_options.memcard_path.empty()) return;
    g_card.data.assign(kCardBytes, 0xFF);

    std::vector<std::uint8_t> source;
    std::ifstream in(g_options.memcard_path, std::ios::binary);
    if (in) {
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        in.seekg(0, std::ios::beg);
        if (size == static_cast<std::streamoff>(kCardBytes)) {
            source.resize(kCardBytes);
            in.read(reinterpret_cast<char *>(source.data()), static_cast<std::streamsize>(kCardBytes));
        } else if (size == static_cast<std::streamoff>(kCardDataBytes)) {
            source.resize(kCardDataBytes);
            in.read(reinterpret_cast<char *>(source.data()), static_cast<std::streamsize>(kCardDataBytes));
            if (std::memcmp(source.data(), "Sony PS2 Memory Card Format ", 28) == 0) {
                const std::string backup = g_options.memcard_path + ".8mb.bak";
                std::ifstream have(backup, std::ios::binary);
                if (!have) {
                    std::ofstream out(backup, std::ios::binary | std::ios::trunc);
                    if (out) out.write(reinterpret_cast<const char *>(source.data()), static_cast<std::streamsize>(kCardDataBytes));
                }
                std::vector<std::uint8_t> converted(kCardBytes, 0);
                for (std::uint32_t page = 0; page < kPages; ++page) {
                    std::memcpy(&converted[static_cast<std::size_t>(page) * kPageBytes],
                                &source[static_cast<std::size_t>(page) * kPageData], kPageData);
                    ecc_page(&converted[static_cast<std::size_t>(page) * kPageBytes]);
                }
                source.swap(converted);
                std::fprintf(stderr, "[mc] converted 8 MB card to 528-byte pages (%s.8mb.bak)\n", g_options.memcard_path.c_str());
            }
        }
    }

    const bool valid = source.size() == kCardBytes && std::memcmp(source.data(), "Sony PS2 Memory Card Format ", 28) == 0;
    if (valid) {
        g_card.data.swap(source);
        std::fprintf(stderr, "[mc] card in slot 1: %s\n", g_options.memcard_path.c_str());
    } else {
        format_card(g_card.data);
        g_card.dirty = true;
        g_card.quiet = 0;
        std::fprintf(stderr, "[mc] card in slot 1: %s (formatted)\n", g_options.memcard_path.c_str());
    }
    g_card.present = true;
}

void memory_card(std::size_t at, std::uint32_t out_size) {
    Card &c = g_card;
    const std::vector<std::uint8_t> in(g_in.begin(), g_in.end());
    if (!c.present || in.size() < 2 || in[0] != 0x81) {
        no_device();
        return;
    }
    const std::size_t n = out_size;
    auto put = [&](std::size_t i, std::uint8_t v) {
        if (i < n) g_out[at + i] = v;
    };
    auto ack = [&](std::size_t from) {
        for (std::size_t i = from; i + 2 < n; ++i) put(i, 0x00);
        if (n >= 2) put(n - 2, 0x2B);
        if (n >= 1) put(n - 1, c.term);
    };
    const std::uint8_t cmd = in[1];
    static const bool trace = std::getenv("PS2_TRACE_MC") != nullptr;
    if (trace) {
        std::fprintf(stderr, "[mc f%llu] cmd %02X in %zu out %zu page %zu :", static_cast<unsigned long long>(ps2::frame_count()), cmd, in.size(), n, c.address / kPageBytes);
        for (std::size_t i = 0; i < in.size() && i < 24; ++i) std::fprintf(stderr, " %02X", in[i]);
        std::fprintf(stderr, "\n");
    }
    switch (cmd) {
    case 0x21: case 0x22: case 0x23: {
        const std::uint32_t page = in.size() > 5 ? static_cast<std::uint32_t>(in[2] | (in[3] << 8) | (in[4] << 16) | (in[5] << 24)) : 0u;
        c.address = static_cast<std::size_t>(std::min(page, kPages - 1)) * kPageBytes;
        ack(2);
        break;
    }
    case 0x26: {
        const std::uint8_t spec[8] = {0x00, 0x02, kBlockPages, 0x00, 0x00, 0x40, 0x00, 0x00};
        put(2, 0x2B);
        for (std::size_t i = 0; i < 8; ++i) put(3 + i, spec[i]);
        put(11, xor_of(spec, 8));
        put(12, c.term);
        break;
    }
    case 0x27:
        if (in.size() > 2) c.term = in[2];
        ack(2);
        break;
    case 0x28:
        put(2, 0x2B);
        put(3, c.term);
        put(4, 0x55);
        break;
    case 0x42: {
        const std::size_t len = in.size() > 2 ? in[2] : 0;
        for (std::size_t i = 0; i < len && 3 + i < in.size() && c.address < kCardBytes; ++i) c.data[c.address++] = in[3 + i];
        c.dirty = true;
        c.quiet = 0;
        ack(2);
        break;
    }
    case 0x43: {
        const std::size_t len = in.size() > 2 ? in[2] : 0;
        std::uint8_t sum = 0;
        put(2, 0x00);
        put(3, 0x2B);
        for (std::size_t i = 0; i < len; ++i) {
            const std::uint8_t v = c.address < kCardBytes ? c.data[c.address++] : 0xFF;
            sum ^= v;
            put(4 + i, v);
        }
        put(4 + len, sum);
        put(5 + len, c.term);
        break;
    }
    case 0x82: {
        const std::size_t block = c.address / (static_cast<std::size_t>(kPageBytes) * kBlockPages) * kPageBytes * kBlockPages;
        std::fill_n(c.data.begin() + static_cast<std::ptrdiff_t>(block), std::min<std::size_t>(kPageBytes * kBlockPages, kCardBytes - block), 0xFF);
        c.dirty = true;
        c.quiet = 0;
        ack(2);
        break;
    }
    default:
        ack(2);
        break;
    }
    if (trace) {
        std::fprintf(stderr, "[mc]  out %zu :", n);
        for (std::size_t i = 0; i < n; ++i) std::fprintf(stderr, " %02X", g_out[at + i]);
        std::fprintf(stderr, " (term %02X)\n", c.term);
    }
}

void process() {
    const std::uint32_t reg = g_send3[g_index & 15u];
    const std::uint32_t in_size = (reg >> 8) & 0x1FFu, out_size = (reg >> 18) & 0x1FFu;
    if (g_in.size() < in_size) return;
    const int port = static_cast<int>(reg & 3u);
    const std::size_t at = g_out.size();
    for (std::uint32_t i = 0; i < out_size; ++i) g_out.push_back(0xFF);
    if (g_index == 0) g_stat = 0x1100u;
    const std::uint32_t device = g_ctrl2[port];
    if (port == 2) memory_card(at, out_size);
    else if (device == 0x5FFFFu || device == 0x00030064u || port >= 2) no_device();
    else controller(port, at, out_size);
    g_in.clear();
    ++g_index;
}

void run_transfer() {
    Dma &in = g_dma[0], &out = g_dma[1];
    if (in.armed) {
        for (std::uint32_t b = 0; b < in.blocks; ++b) {
            for (std::uint32_t i = 0; i < in.block_words * 4; ++i) g_in.push_back(read8(in.madr + b * in.block_words * 4 + i));
            process();
        }
        in.armed = false;
        hw::dma_done(11);
    }
    if (out.armed) {
        for (std::uint32_t b = 0; b < out.blocks && b < 16; ++b) {
            const std::uint32_t size = (g_send3[b] >> 18) & 0x1FFu;
            for (std::uint32_t i = 0; i < size && !g_out.empty(); ++i) {
                write8(out.madr + b * out.block_words * 4 + i, g_out.front());
                g_out.pop_front();
            }
        }
        out.armed = false;
        hw::dma_done(12);
    }
    hw::raise_interrupt(17);
}

}

void reset() {
    std::memset(g_send3, 0, sizeof g_send3);
    std::memset(g_ctrl1, 0, sizeof g_ctrl1);
    std::memset(g_ctrl2, 0, sizeof g_ctrl2);
    g_index = 0;
    g_stat = 0;
    g_in.clear();
    g_out.clear();
    g_pads[0] = Pad{};
    g_pads[1] = Pad{};
    g_pads[0].connected = g_plugged[0];
    g_pads[1].connected = g_plugged[1];
    for (Feel &f : g_feel) {
        std::fill(std::begin(f.motors), std::end(f.motors), 0xFF);
        f.rumble.store(0);
    }
    g_dma[0] = g_dma[1] = Dma{};
    load_card();
}

void flush_cards() {
    Card &c = g_card;
    if (!c.dirty || ++c.quiet < 50) return;
    c.dirty = false;
    std::ofstream out(g_options.memcard_path, std::ios::binary | std::ios::trunc);
    if (out) out.write(reinterpret_cast<const char *>(c.data.data()), static_cast<std::streamsize>(c.data.size()));
    if (!out) {
        std::fprintf(stderr, "[mc] cannot write %s\n", g_options.memcard_path.c_str());
        return;
    }
    out.close();
    if (g_options.on_memcard_saved) g_options.on_memcard_saved();
}

std::uint32_t read(std::uint32_t a) {
    if (a >= 0x1F808200u && a < 0x1F808240u) return g_send3[(a - 0x1F808200u) / 4];
    if (a >= 0x1F808240u && a < 0x1F808260u) {
        const std::uint32_t port = (a - 0x1F808240u) / 8;
        return (a & 4u) ? g_ctrl2[port] : g_ctrl1[port];
    }
    switch (a) {
    case 0x1F808264u: {
        if (g_out.empty()) return 0xFF;
        const std::uint8_t v = g_out.front();
        g_out.pop_front();
        return v;
    }
    case 0x1F808268u: return 0;
    case 0x1F80826Cu: return g_stat;
    case 0x1F808270u: return 0xF;
    case 0x1F808274u: return 0;
    default: return 0;
    }
}

void write(std::uint32_t a, std::uint32_t v) {
    if (a >= 0x1F808200u && a < 0x1F808240u) { g_send3[(a - 0x1F808200u) / 4] = v; return; }
    if (a >= 0x1F808240u && a < 0x1F808260u) {
        const std::uint32_t port = (a - 0x1F808240u) / 8;
        if (a & 4u) g_ctrl2[port] = v;
        else g_ctrl1[port] = v;
        return;
    }
    switch (a) {
    case 0x1F808260u:
        g_in.push_back(static_cast<std::uint8_t>(v));
        process();
        break;
    case 0x1F808268u:
        if ((v & 0xCu) == 0xCu) {
            g_index = 0;
            g_in.clear();
            g_out.clear();
        }
        if (v & 1u) run_transfer();
        break;
    default:
        break;
    }
}

void arm_dma(int channel, std::uint32_t madr, std::uint32_t block_words, std::uint32_t blocks) {
    Dma &d = g_dma[channel == 11 ? 0 : 1];
    d.armed = true;
    d.madr = madr;
    d.block_words = block_words;
    d.blocks = blocks;
}

void set_buttons(int pad, std::uint16_t pressed_mask) {
    if (pad >= 0 && pad < 2) g_pads[pad].buttons = pressed_mask;
}

void set_axis(int pad, int axis, std::uint8_t value) {
    if (pad >= 0 && pad < 2 && axis >= 0 && axis < 4) g_pads[pad].axes[axis] = value;
}

void set_pressure(int pad, int button, std::uint8_t value) {
    if (pad >= 0 && pad < 2 && button >= 0 && button < 16) g_feel[pad].pressure[button] = value;
}

void set_connected(int pad, bool connected) {
    if (pad >= 0 && pad < 2) g_pads[pad].connected = g_plugged[pad] = connected;
}

std::uint16_t rumble(int pad) { return pad >= 0 && pad < 2 ? g_feel[pad].rumble.load(std::memory_order_relaxed) : 0; }

}

namespace ps2::iop::sio2 {

void state(ps2::state::Stream &s) {
    s.tag("sio2");
    s.pod(g_send3);
    s.pod(g_ctrl1);
    s.pod(g_ctrl2);
    s.pod(g_index);
    s.pod(g_stat);
    s.deque(g_in);
    s.deque(g_out);
    s.pod(g_pads);
    if (s.loading()) {
        g_pads[0].connected = g_plugged[0];
        g_pads[1].connected = g_plugged[1];
        for (Feel &f : g_feel) {
            std::copy(std::begin(kLibpadMotors), std::end(kLibpadMotors), f.motors);
            f.rumble.store(0);
        }
    }
    s.pod(g_dma);
    s.size(g_card.address);
    s.pod(g_card.term);
}

}
