#include "snr2.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace recomp {

namespace {

constexpr std::uint32_t kMagic = 0x32524E53u;

std::uint32_t u32(const std::vector<std::uint8_t> &b, std::uint32_t o) {
    std::uint32_t v = 0;
    if (o <= b.size() && b.size() - o >= 4) std::memcpy(&v, b.data() + o, 4);
    return v;
}

std::uint16_t u16(const std::vector<std::uint8_t> &b, std::uint32_t o) {
    std::uint16_t v = 0;
    if (o <= b.size() && b.size() - o >= 2) std::memcpy(&v, b.data() + o, 2);
    return v;
}

std::optional<Fix> fix_of(std::uint32_t type) {
    switch (type) {
    case 2: return Fix::Word;
    case 4: return Fix::Jump;
    case 5: return Fix::High;
    case 6: return Fix::Low;
    default: return std::nullopt;
    }
}

std::string module_name(const std::vector<std::uint8_t> &b, std::uint32_t at) {
    std::string name;
    for (std::uint32_t o = at; at && o < b.size() && b[o]; ++o) name += static_cast<char>(b[o]);
    if (const auto slash = name.find_last_of("\\/"); slash != std::string::npos) name = name.substr(slash + 1);
    if (const auto dot = name.find('.'); dot != std::string::npos) name = name.substr(0, dot);
    std::string clean;
    for (char ch : name) clean += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
    return clean.empty() ? "module" : clean;
}

}

std::optional<Snr2Module> read_snr2_module(std::vector<std::uint8_t> bytes) {
    if (bytes.size() < 0x40 || u32(bytes, 0) != kMagic) return std::nullopt;
    Snr2Module m;
    const std::uint32_t rel = u32(bytes, 0x04), nrel = u32(bytes, 0x08), sym = u32(bytes, 0x0C), nsym = u32(bytes, 0x10);
    const std::uint32_t stream = u32(bytes, 0x34);
    m.size = u32(bytes, 0x2C);
    if (m.size > bytes.size() || rel + nrel * 12u > m.size || sym + nsym * 12u > m.size || stream >= m.size) return std::nullopt;
    m.name = module_name(bytes, u32(bytes, 0x14));

    m.code_begin = 0x40;
    while (m.code_begin < m.size && !u32(bytes, m.code_begin)) m.code_begin += 4;
    m.code_end = m.size;
    for (std::uint32_t table : {rel, sym, stream, u32(bytes, 0x14)})
        if (table > m.code_begin) m.code_end = std::min(m.code_end, table);

    for (std::uint32_t entry : {u32(bytes, 0x18), u32(bytes, 0x1C)})
        if (entry) m.seeds.push_back(entry);

    std::vector<std::uint8_t> kinds(nsym);
    for (std::uint32_t k = 0; k < nsym; ++k) kinds[k] = static_cast<std::uint8_t>(u32(bytes, sym + k * 12u + 8u) >> 16);

    std::vector<std::pair<std::uint32_t, std::uint32_t>> steps;
    for (std::uint32_t p = stream, at = 0; stream && p < m.size;) {
        const std::uint8_t b = bytes[p++];
        if (b & 0xFCu) {
            at += b & 0xFCu;
        } else {
            const std::uint32_t word = u32(bytes, p);
            p += 4;
            if (!word) break;
            at = word;
        }
        steps.emplace_back(b & 3u, at);
    }
    for (std::size_t k = 0; k < steps.size(); ++k) {
        const auto [type, at] = steps[k];
        switch (type) {
        case 0:
            m.links[at] = {Fix::Low, false, 0};
            break;
        case 1: {
            const auto low = std::find_if(steps.begin() + static_cast<std::ptrdiff_t>(k) + 1, steps.end(), [](const auto &s) { return s.first == 0; });
            if (low == steps.end()) break;
            const std::uint32_t value = (static_cast<std::uint32_t>(u16(bytes, at)) << 16) +
                                        static_cast<std::uint32_t>(static_cast<std::int16_t>(u16(bytes, low->second)));
            m.links[at] = {Fix::High, false, value};
            break;
        }
        case 2:
            m.links[at] = {Fix::Jump, false, 0};
            m.seeds.push_back((u32(bytes, at) & 0x03FFFFFFu) << 2);
            break;
        default:
            m.links[at] = {Fix::Word, false, 0};
            break;
        }
    }

    for (std::uint32_t k = 0; k < nrel; ++k) {
        const std::uint32_t at = u32(bytes, rel + k * 12u), info = u32(bytes, rel + k * 12u + 4u);
        const std::uint32_t symbol = info >> 8;
        const auto fix = fix_of(info & 0xFFu);
        if (!fix) continue;
        m.links[at] = {*fix, true, 0};
        if (symbol < nsym && (kinds[symbol] == 0 || kinds[symbol] == 2 || kinds[symbol] == 3) && *fix == Fix::Jump)
            m.seeds.push_back(u32(bytes, sym + symbol * 12u + 4u) + u32(bytes, rel + k * 12u + 8u));
    }
    m.bytes = std::move(bytes);
    m.bytes.resize(m.size);
    return m;
}

Links snr2_imports(const Image &image) {
    Links links;
    for (const auto &segment : image.segments()) {
        for (std::uint32_t o = 0; o + 0x40 <= segment.bytes.size(); o += 4) {
            if (u32(segment.bytes, o) != kMagic) continue;
            const std::uint32_t rel = u32(segment.bytes, o + 0x04), nrel = u32(segment.bytes, o + 0x08);
            if (!nrel || nrel > 0x100000u || !image.word(rel) || !image.word(rel + nrel * 12u - 4u)) continue;
            for (std::uint32_t k = 0; k < nrel; ++k) {
                const std::uint32_t at = *image.word(rel + k * 12u), info = *image.word(rel + k * 12u + 4u);
                const auto fix = fix_of(info & 0xFFu);
                if (fix && *fix != Fix::Word && image.executable(at)) links[at] = {*fix, true, 0};
            }
            return links;
        }
    }
    return links;
}

Image snr2_image(const Snr2Module &module) {
    return Image::raw(0, module.bytes, {module.code_begin, module.code_end});
}

}
