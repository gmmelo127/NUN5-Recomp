#include "ipu.hpp"

#include "tables.hpp"
#include "../dmac.hpp"
#include "../hw.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#include <vector>

namespace ps2::ipu {

namespace {

using namespace tables;

struct NeedData {};
struct BadCode {};

constexpr std::uint32_t kEcd = 0x4000u, kScd = 0x8000u, kReset = 0x40000000u;
constexpr std::size_t kFifoBytes = 128;

constexpr std::uint8_t kDefaultIntra[64] = {
    8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
};

constexpr std::uint8_t kNonLinearScale[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  10, 12, 14, 16, 18, 20, 22,
                                              24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};

struct Entry {
    std::uint8_t length = 0;
    std::uint32_t value = 0;
};

struct Lookup {
    unsigned bits = 0;
    std::vector<Entry> entries;
};

struct DctEntry {
    std::uint8_t length = 0;
    std::uint8_t run = 0;
    std::int16_t level = 0;
};

struct DctLookup {
    unsigned bits = 0;
    std::vector<DctEntry> entries;
};

template <std::size_t N>
Lookup build(const Vlc (&table)[N]) {
    Lookup l;
    for (const Vlc &e : table) l.bits = std::max<unsigned>(l.bits, e.length);
    l.entries.resize(std::size_t{1} << l.bits);
    for (const Vlc &e : table) {
        const unsigned shift = l.bits - e.length;
        for (std::uint32_t i = 0; i < (1u << shift); ++i) l.entries[(static_cast<std::uint32_t>(e.code) << shift) | i] = {e.length, e.value};
    }
    return l;
}

template <std::size_t N>
DctLookup build(const Dct (&table)[N]) {
    DctLookup l;
    for (const Dct &e : table) l.bits = std::max<unsigned>(l.bits, e.length);
    l.entries.resize(std::size_t{1} << l.bits);
    for (const Dct &e : table) {
        const unsigned shift = l.bits - e.length;
        for (std::uint32_t i = 0; i < (1u << shift); ++i) l.entries[(static_cast<std::uint32_t>(e.code) << shift) | i] = {e.length, e.run, e.level};
    }
    return l;
}

struct Tables {
    Lookup mbai = build(kMbAddressIncrement), mb_i = build(kMbTypeI), mb_p = build(kMbTypeP), mb_b = build(kMbTypeB);
    Lookup motion = build(kMotionCode), dmv = build(kDmVector), dc_luma = build(kDcSizeLuma), dc_chroma = build(kDcSizeChroma);
    Lookup cbp = build(kCodedBlockPattern);
    DctLookup dct0 = build(kDctTable0), dct1 = build(kDctTable1);
    double cosines[8][8];

    Tables() {
        for (int x = 0; x < 8; ++x)
            for (int u = 0; u < 8; ++u)
                cosines[x][u] = (u ? 0.5 : 0.5 / std::sqrt(2.0)) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0);
    }
};

const Tables &tables() {
    static const Tables t;
    return t;
}

enum Command { kBclr, kIdec, kBdec, kVdec, kFdec, kSetiq, kSetvq, kCsc, kPack, kSetth };

std::vector<std::uint8_t> g_in;
std::size_t g_bit = 0;
std::vector<std::uint8_t> g_out;
std::size_t g_out_pos = 0;

std::uint32_t g_ctrl = 0, g_result = 0, g_command = 0;
int g_last = -1;
bool g_busy = false, g_decoding = false, g_interrupt = true, g_starved = false;
int g_phase = 0;
std::uint32_t g_qsc = 0, g_count = 0;
bool g_first = true;
std::uint8_t g_intra_iq[64], g_nonintra_iq[64];
std::uint16_t g_vqclut[16];
std::uint16_t g_th0 = 0, g_th1 = 0;
std::int16_t g_dc[3];
std::uint64_t g_commands[16], g_stalls = 0, g_out_qwords = 0;
std::uint64_t g_conv16 = 0, g_conv32 = 0;


bool pull() {
    std::uint8_t q[16];
    if (!dmac::ipu_pull(q)) return false;
    g_in.insert(g_in.end(), q, q + 16);
    return true;
}

bool have(std::size_t bits) {
    while (g_in.size() * 8 < g_bit + bits)
        if (!pull()) return false;
    return true;
}

std::size_t available() { return g_in.size() * 8 > g_bit ? g_in.size() * 8 - g_bit : 0; }

std::uint32_t bits_at(std::size_t bit, unsigned n) {
    if (!n) return 0;
    const std::size_t byte = bit >> 3;
    std::uint64_t v;
    if (byte + 8 <= g_in.size()) {
        std::memcpy(&v, g_in.data() + byte, 8);
        v = __builtin_bswap64(v);
    } else {
        v = 0;
        for (std::size_t i = 0; i < 8; ++i) v = (v << 8) | (byte + i < g_in.size() ? g_in[byte + i] : 0u);
    }
    return static_cast<std::uint32_t>((v << (bit & 7u)) >> (64 - n));
}

std::uint32_t peek(unsigned n) {
    if (!have(n)) throw NeedData{};
    return bits_at(g_bit, n);
}

std::uint32_t get(unsigned n) {
    const std::uint32_t v = peek(n);
    g_bit += n;
    return v;
}

void skip(unsigned n) {
    if (!have(n)) throw NeedData{};
    g_bit += n;
}

std::uint32_t peek_soft(unsigned n, std::size_t &avail) {
    have(n);
    avail = available();
    return bits_at(g_bit, n);
}

std::uint32_t decode(const Lookup &l) {
    std::size_t avail;
    const Entry &e = l.entries[peek_soft(l.bits, avail)];
    if (!e.length) {
        if (avail < l.bits) throw NeedData{};
        throw BadCode{};
    }
    if (e.length > avail) throw NeedData{};
    g_bit += e.length;
    return e.value;
}

std::uint32_t peek_now() {
    const unsigned n = static_cast<unsigned>(std::min<std::size_t>(32, available()));
    return n ? bits_at(g_bit, n) << (32 - n) : 0u;
}

void commit() {
    const std::size_t drop = std::min(g_bit / 128 * 16, g_in.size() / 16 * 16);
    g_in.erase(g_in.begin(), g_in.begin() + static_cast<std::ptrdiff_t>(drop));
    g_bit -= drop * 8;
}

void top_up() {
    while (g_in.size() < kFifoBytes && pull()) {}
}

unsigned dc_precision() { return (g_ctrl >> 16) & 3u; }
bool alternate_scan() { return g_ctrl & 0x100000u; }
bool intra_vlc_one() { return g_ctrl & 0x200000u; }
bool nonlinear_q() { return g_ctrl & 0x400000u; }
bool mpeg1() { return g_ctrl & 0x800000u; }
unsigned picture_type() { return (g_ctrl >> 24) & 7u; }

int read_dc_diff(int channel) {
    const unsigned size = decode(channel ? tables().dc_chroma : tables().dc_luma) & 0xFFFFu;
    if (!size) return 0;
    const int v = static_cast<int>(get(size));
    return v < (1 << (size - 1)) ? v + 1 - (1 << size) : v;
}

bool read_coefficient(const DctLookup &l, int &run, int &level) {
    std::size_t avail;
    const DctEntry &e = l.entries[peek_soft(l.bits, avail)];
    if (!e.length) {
        if (avail < l.bits) throw NeedData{};
        throw BadCode{};
    }
    if (e.length > avail) throw NeedData{};
    g_bit += e.length;
    if (e.run == kEscape) {
        run = static_cast<int>(get(6));
        if (mpeg1()) {
            int v = static_cast<int>(get(8));
            if (v == 0) v = static_cast<int>(get(8));
            else if (v == 128) v = static_cast<int>(get(8)) - 256;
            else if (v > 128) v -= 256;
            level = v;
        } else {
            const int v = static_cast<int>(get(12));
            level = v & 0x800 ? v - 0x1000 : v;
        }
        return true;
    }
    run = e.run;
    level = get(1) ? -e.level : e.level;
    return true;
}

void idct(const int *in, std::int16_t *out) {
    const auto &c = tables().cosines;
    double rows[8][8];
    for (int v = 0; v < 8; ++v)
        for (int x = 0; x < 8; ++x) {
            double s = 0;
            for (int u = 0; u < 8; ++u) s += c[x][u] * in[v * 8 + u];
            rows[v][x] = s;
        }
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            double s = 0;
            for (int v = 0; v < 8; ++v) s += c[y][v] * rows[v][x];
            out[y * 8 + x] = static_cast<std::int16_t>(std::clamp(static_cast<int>(std::floor(s + 0.5)), -256, 255));
        }
}

void decode_block(std::int16_t *out, int channel, bool intra, std::uint32_t qsc) {
    int coef[64] = {};
    int index = 0;
    const DctLookup &table = intra && intra_vlc_one() ? tables().dct1 : tables().dct0;
    const bool table_one = &table == &tables().dct1;
    if (intra) {
        g_dc[channel] = static_cast<std::int16_t>(g_dc[channel] + read_dc_diff(channel));
        coef[0] = g_dc[channel];
        index = 1;
    } else if (peek(1)) {
        coef[0] = get(2) & 1u ? -1 : 1;
        index = 1;
    }
    for (;;) {
        if (index) {
            if (table_one ? peek(4) == 6u : peek(2) == 2u) {
                skip(table_one ? 4 : 2);
                break;
            }
        }
        int run = 0, level = 0;
        read_coefficient(table, run, level);
        index += run;
        if (index >= 64) throw BadCode{};
        coef[index++] = level;
    }
    const std::uint8_t *scan = alternate_scan() ? kAlternate : kZigzag;
    const std::uint8_t *weights = intra ? g_intra_iq : g_nonintra_iq;
    const int scale = mpeg1() || !nonlinear_q() ? static_cast<int>(qsc) * 2 : kNonLinearScale[qsc & 31u];
    int f[64] = {};
    int sum = 0;
    for (int k = 0; k < 64; ++k) {
        const int v = coef[k];
        const int r = scan[k];
        int x;
        if (intra && k == 0) {
            x = v * (8 >> dc_precision());
        } else {
            if (!v) continue;
            const int sign = v > 0 ? 1 : -1;
            x = intra ? (v * weights[r] * scale * 2) / 32 : ((2 * v + sign) * weights[r] * scale) / 32;
            if (mpeg1() && x && !(x & 1)) x -= sign;
        }
        x = std::clamp(x, -2048, 2047);
        f[r] = x;
        sum += x;
    }
    if (!mpeg1() && !(sum & 1)) f[63] += f[63] & 1 ? -1 : 1;
    idct(f, out);
}

void decode_macroblock(std::int16_t *raw, bool intra, bool field, bool dc_reset, std::uint32_t qsc) {
    const unsigned cbp = intra ? 0x3Fu : decode(tables().cbp) & 0xFFu;
    if (dc_reset) g_dc[0] = g_dc[1] = g_dc[2] = static_cast<std::int16_t>(128 << dc_precision());
    std::int16_t blocks[6][64] = {};
    for (int b = 0; b < 6; ++b)
        if (cbp & (32u >> b)) decode_block(blocks[b], b < 4 ? 0 : b - 3, intra, qsc);
    for (int i = 0; i < 8; ++i) {
        const int top = field ? 2 * i : i, bottom = field ? 2 * i + 1 : i + 8;
        std::memcpy(raw + top * 16, blocks[0] + i * 8, 16);
        std::memcpy(raw + top * 16 + 8, blocks[1] + i * 8, 16);
        std::memcpy(raw + bottom * 16, blocks[2] + i * 8, 16);
        std::memcpy(raw + bottom * 16 + 8, blocks[3] + i * 8, 16);
    }
    std::memcpy(raw + 256, blocks[4], 128);
    std::memcpy(raw + 320, blocks[5], 128);
}

void convert(const std::uint8_t *raw, bool rgb16) {
    std::uint8_t pixels[1024];
    std::size_t size = 0;
    if (rgb16) ++g_conv16; else ++g_conv32;
#if defined(__SSE2__)
    // Same fixed point math as the scalar path below, eight pixels at a time.
    const __m128i zero = _mm_setzero_si128();
    const __m128i k149_204 = _mm_set1_epi32(static_cast<int>(0x00CC0095u));
    const __m128i k149_m50 = _mm_set1_epi32(static_cast<int>(0xFFCE0095u));
    const __m128i k149_258 = _mm_set1_epi32(static_cast<int>(0x01020095u));
    const __m128i km104_0 = _mm_set1_epi32(static_cast<int>(0x0000FF98u));
    const __m128i v128 = _mm_set1_epi16(128);
    const __m128i v16 = _mm_set1_epi16(16);
    const __m128i v64 = _mm_set1_epi32(64);
    const __m128i v255 = _mm_set1_epi16(255);
    const __m128i v128w = _mm_set1_epi16(0x80);
    const __m128i v64w = _mm_set1_epi16(0x40);
    const __m128i th0 = _mm_set1_epi16(static_cast<short>(g_th0));
    const __m128i th1 = _mm_set1_epi16(static_cast<short>(g_th1));
    for (int y = 0; y < 16; ++y) {
        for (int x0 = 0; x0 < 16; x0 += 8) {
            const __m128i yb = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(raw + y * 16 + x0));
            __m128i yp = _mm_subs_epu16(_mm_unpacklo_epi8(yb, zero), v16);
            const int cy = (y >> 1) * 8 + (x0 >> 1);
            const __m128i cbq = _mm_cvtsi32_si128(*reinterpret_cast<const int *>(raw + 256 + cy));
            const __m128i crq = _mm_cvtsi32_si128(*reinterpret_cast<const int *>(raw + 320 + cy));
            const __m128i cbw = _mm_unpacklo_epi8(cbq, zero), crw = _mm_unpacklo_epi8(crq, zero);
            const __m128i cb = _mm_sub_epi16(_mm_unpacklo_epi16(cbw, cbw), v128);
            const __m128i cr = _mm_sub_epi16(_mm_unpacklo_epi16(crw, crw), v128);
            const __m128i yl = _mm_unpacklo_epi16(yp, cr), yh = _mm_unpackhi_epi16(yp, cr);
            const __m128i cl = _mm_unpacklo_epi16(yp, cb), ch = _mm_unpackhi_epi16(yp, cb);
            const __m128i rl = _mm_unpacklo_epi16(cr, zero), rh = _mm_unpackhi_epi16(cr, zero);
            const __m128i r0 = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yl, k149_204), v64), 7);
            const __m128i r1 = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yh, k149_204), v64), 7);
            const __m128i g0 = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(cl, k149_m50), _mm_madd_epi16(rl, km104_0)), v64), 7);
            const __m128i g1 = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(ch, k149_m50), _mm_madd_epi16(rh, km104_0)), v64), 7);
            const __m128i b0 = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(cl, k149_258), v64), 7);
            const __m128i b1 = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(ch, k149_258), v64), 7);
            __m128i r16 = _mm_min_epi16(_mm_max_epi16(_mm_packs_epi32(r0, r1), zero), v255);
            __m128i g16 = _mm_min_epi16(_mm_max_epi16(_mm_packs_epi32(g0, g1), zero), v255);
            __m128i b16 = _mm_min_epi16(_mm_max_epi16(_mm_packs_epi32(b0, b1), zero), v255);
            __m128i m1 = _mm_and_si128(_mm_and_si128(_mm_cmpgt_epi16(th1, r16), _mm_cmpgt_epi16(th1, g16)), _mm_cmpgt_epi16(th1, b16));
            __m128i m0 = _mm_and_si128(_mm_and_si128(_mm_cmpgt_epi16(th0, r16), _mm_cmpgt_epi16(th0, g16)), _mm_cmpgt_epi16(th0, b16));
            __m128i a16 = _mm_or_si128(_mm_andnot_si128(m1, v128w), _mm_and_si128(m1, v64w));
            a16 = _mm_andnot_si128(m0, a16);
            if (rgb16) {
                __m128i p = _mm_or_si128(_mm_srli_epi16(r16, 3), _mm_slli_epi16(_mm_srli_epi16(g16, 3), 5));
                p = _mm_or_si128(p, _mm_slli_epi16(_mm_srli_epi16(b16, 3), 10));
                p = _mm_or_si128(p, _mm_slli_epi16(_mm_and_si128(a16, v128w), 8));
                _mm_storeu_si128(reinterpret_cast<__m128i *>(pixels + size), p);
                size += 16;
            } else {
                const __m128i rb = _mm_packus_epi16(r16, r16), gb = _mm_packus_epi16(g16, g16);
                const __m128i bb = _mm_packus_epi16(b16, b16), ab = _mm_packus_epi16(a16, a16);
                const __m128i rg = _mm_unpacklo_epi8(rb, gb), ba = _mm_unpacklo_epi8(bb, ab);
                _mm_storeu_si128(reinterpret_cast<__m128i *>(pixels + size), _mm_unpacklo_epi16(rg, ba));
                _mm_storeu_si128(reinterpret_cast<__m128i *>(pixels + size + 16), _mm_unpackhi_epi16(rg, ba));
                size += 32;
            }
        }
    }
#else
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const int lum = 149 * std::max(0, raw[y * 16 + x] - 16);
            const int cb = raw[256 + (y / 2) * 8 + x / 2] - 128, cr = raw[320 + (y / 2) * 8 + x / 2] - 128;
            const int r = std::clamp((lum + 204 * cr + 64) >> 7, 0, 255);
            const int g = std::clamp((lum - 50 * cb - 104 * cr + 64) >> 7, 0, 255);
            const int b = std::clamp((lum + 258 * cb + 64) >> 7, 0, 255);
            int a = 0x80;
            if (r < g_th0 && g < g_th0 && b < g_th0) a = 0;
            else if (r < g_th1 && g < g_th1 && b < g_th1) a = 0x40;
            if (rgb16) {
                const std::uint16_t p = static_cast<std::uint16_t>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | (a & 0x80 ? 0x8000 : 0));
                std::memcpy(pixels + size, &p, 2);
                size += 2;
            } else {
                const std::uint8_t p[4] = {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(a)};
                std::memcpy(pixels + size, p, 4);
                size += 4;
            }
        }
#endif
    {
        static const bool check = std::getenv("PS2_IPU_CHECK") != nullptr;
        if (check) {
            std::uint8_t ref[1024];
            std::size_t rsize = 0;
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 16; ++x) {
                    const int lum = 149 * std::max(0, raw[y * 16 + x] - 16);
                    const int cb = raw[256 + (y / 2) * 8 + x / 2] - 128, cr = raw[320 + (y / 2) * 8 + x / 2] - 128;
                    const int r = std::clamp((lum + 204 * cr + 64) >> 7, 0, 255);
                    const int g = std::clamp((lum - 50 * cb - 104 * cr + 64) >> 7, 0, 255);
                    const int b = std::clamp((lum + 258 * cb + 64) >> 7, 0, 255);
                    int a = 0x80;
                    if (r < g_th0 && g < g_th0 && b < g_th0) a = 0;
                    else if (r < g_th1 && g < g_th1 && b < g_th1) a = 0x40;
                    if (rgb16) {
                        const std::uint16_t p = static_cast<std::uint16_t>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | (a & 0x80 ? 0x8000 : 0));
                        std::memcpy(ref + rsize, &p, 2);
                        rsize += 2;
                    } else {
                        const std::uint8_t p[4] = {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(a)};
                        std::memcpy(ref + rsize, p, 4);
                        rsize += 4;
                    }
                }
            if (rsize != size || std::memcmp(ref, pixels, size) != 0) {
                static int shown = 0;
                if (shown++ < 5) {
                    std::size_t at = 0;
                    while (at < size && at < rsize && ref[at] == pixels[at]) ++at;
                    std::fprintf(stderr, "[ipu] convert mismatch at byte %zu (simd %u ref %u) rgb16 %d\n", at,
                                 at < size ? pixels[at] : 0u, at < rsize ? ref[at] : 0u, rgb16 ? 1 : 0);
                }
            }
        }
    }
    g_out.insert(g_out.end(), pixels, pixels + size);
}

void emit_raw16(const std::int16_t *raw) {
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(raw);
    g_out.insert(g_out.end(), bytes, bytes + 768);
}

bool start_code_follows() {
    std::size_t avail;
    const std::uint32_t next = peek_soft(8, avail);
    return avail >= 8 && next == 0;
}

template <typename F>
bool atomic(F &&body) {
    const std::size_t bit = g_bit;
    std::int16_t dc[3];
    std::memcpy(dc, g_dc, sizeof dc);
    const std::size_t out = g_out.size();
    try {
        body();
        return true;
    } catch (const NeedData &) {
        ++g_stalls;
        g_bit = bit;
        std::memcpy(g_dc, dc, sizeof dc);
        g_out.resize(out);
        return false;
    }
}

bool run_idec() {
    const bool dtd = (g_command >> 24) & 1u, rgb16 = (g_command >> 27) & 1u;
    for (;;) {
        bool ok = true;
        switch (g_phase) {
        case 0:
            ok = atomic([&] { skip(g_command & 0x3Fu); });
            if (ok) g_phase = 1;
            break;
        case 1:
            ok = atomic([&] {
                const std::uint32_t type = decode(tables().mb_i) & 0xFFFFu;
                const bool field = dtd && get(1);
                std::uint32_t qsc = g_qsc;
                if (type & 0x10u) qsc = get(5);
                std::int16_t raw[384];
                decode_macroblock(raw, true, field, g_first, qsc);
                std::uint8_t raw8[384];
                for (int i = 0; i < 384; ++i) raw8[i] = static_cast<std::uint8_t>(std::clamp<int>(raw[i], 0, 255));
                convert(raw8, rgb16);
                g_qsc = qsc;
            });
            if (ok) {
                g_first = false;
                g_phase = 2;
            }
            break;
        case 2:
            ok = atomic([&] {
                std::size_t avail;
                const std::uint32_t next = peek_soft(8, avail);
                if (avail < 8) throw NeedData{};
                if (next == 0) {
                    g_bit = (g_bit + 7) & ~std::size_t{7};
                    g_phase = 3;
                    return;
                }
                std::uint32_t v;
                do v = decode(tables().mbai) & 0xFFFFu;
                while (v == 0x22u || v == 0x23u);
                g_phase = 1;
            });
            break;
        case 3:
            ok = atomic([&] {
                for (;;) {
                    std::size_t avail;
                    const std::uint32_t code = peek_soft(24, avail);
                    if (avail < 24 || code == 1) {
                        g_phase = 4;
                        return;
                    }
                    if (code != 0) throw BadCode{};
                    g_bit += 8;
                }
            });
            break;
        default:
            g_ctrl |= kScd;
            return true;
        }
        if (!ok) return false;
        commit();
    }
}

bool run_command() {
    const std::uint32_t cmd = g_command;
    switch (cmd >> 28) {
    case kBclr:
        g_in.clear();
        g_bit = cmd & 0x7Fu;
        return true;
    case kIdec:
        return run_idec();
    case kBdec: {
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            std::int16_t raw[384];
            decode_macroblock(raw, (cmd >> 27) & 1u, (cmd >> 25) & 1u, (cmd >> 26) & 1u, (cmd >> 16) & 0x1Fu);
            emit_raw16(raw);
        });
        if (!ok) return false;
        commit();
        if (start_code_follows()) {
            g_ctrl |= kScd;
            g_interrupt = false;
        }
        return true;
    }
    case kVdec: {
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            const Lookup *table = nullptr;
            switch ((cmd >> 26) & 3u) {
            case 0: table = &tables().mbai; break;
            case 1:
                switch (picture_type()) {
                case 1: table = &tables().mb_i; break;
                case 2: table = &tables().mb_p; break;
                case 3: table = &tables().mb_b; break;
                default: throw BadCode{};
                }
                break;
            case 2: table = &tables().motion; break;
            default: table = &tables().dmv; break;
            }
            g_result = decode(*table);
        });
        if (ok) commit();
        return ok;
    }
    case kFdec: {
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            g_result = peek(32);
        });
        if (ok) commit();
        return ok;
    }
    case kSetiq: {
        std::uint8_t *matrix = (cmd >> 27) & 1u ? g_nonintra_iq : g_intra_iq;
        std::uint8_t values[64];
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            for (std::uint8_t &v : values) v = static_cast<std::uint8_t>(get(8));
        });
        if (!ok) return false;
        for (int k = 0; k < 64; ++k) matrix[kZigzag[k]] = values[k];
        commit();
        return true;
    }
    case kSetvq: {
        std::uint16_t values[16];
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            for (std::uint16_t &v : values) v = static_cast<std::uint16_t>(get(16));
        });
        if (!ok) return false;
        std::memcpy(g_vqclut, values, sizeof values);
        commit();
        return true;
    }
    case kCsc:
        while (g_count) {
            std::uint8_t raw[384];
            const bool ok = atomic([&] {
                for (std::uint8_t &v : raw) v = static_cast<std::uint8_t>(get(8));
                convert(raw, (cmd >> 27) & 1u);
            });
            if (!ok) return false;
            commit();
            --g_count;
        }
        return true;
    case kSetth:
        g_th0 = static_cast<std::uint16_t>(cmd & 0x1FFu);
        g_th1 = static_cast<std::uint16_t>((cmd >> 16) & 0x1FFu);
        return true;
    default:
        std::fprintf(stderr, "[ipu] command 0x%08X is not implemented\n", cmd);
        return true;
    }
}

void drain() {
    while (g_out_pos + 16 <= g_out.size() && dmac::ipu_push(g_out.data() + g_out_pos)) {
        g_out_pos += 16;
        ++g_out_qwords;
    }
    if (g_out_pos == g_out.size()) {
        g_out.clear();
        g_out_pos = 0;
    }
}

void step() {
    if (g_busy && g_decoding) {
        try {
            g_starved = !run_command();
            if (!g_starved) g_decoding = false;
        } catch (const BadCode &) {
            std::fprintf(stderr, "[ipu] bad code in command 0x%08X\n", g_command);
            g_ctrl |= kEcd;
            g_decoding = false;
            g_busy = false;
        }
    }
    drain();
    if (g_busy && !g_decoding && g_out.empty()) {
        g_busy = false;
        if (g_interrupt) hw::intc_raise(8);
    }
    top_up();
}

}

void reset() {
    g_in.clear();
    g_out.clear();
    g_bit = g_out_pos = 0;
    g_ctrl = g_result = g_command = 0;
    g_last = -1;
    g_busy = g_decoding = false;
    std::memcpy(g_intra_iq, kDefaultIntra, 64);
    std::fill(std::begin(g_nonintra_iq), std::end(g_nonintra_iq), 16);
    std::memset(g_vqclut, 0, sizeof g_vqclut);
    g_th0 = g_th1 = 0;
    std::memset(g_dc, 0, sizeof g_dc);
}

std::uint32_t read32(std::uint32_t a) {
    switch (a & 0xFFFFFFFCu) {
    case 0x10002000u:
        return g_last == kVdec || g_last == kFdec ? g_result : peek_now();
    case 0x10002004u:
        return g_busy ? 0x80000000u : 0u;
    case 0x10002010u: {
        const std::uint32_t ifc = g_starved ? 0u : static_cast<std::uint32_t>(std::min<std::size_t>(8, g_in.size() / 16));
        const std::uint32_t ofc = static_cast<std::uint32_t>(std::min<std::size_t>(8, (g_out.size() - g_out_pos) / 16));
        return (g_ctrl & ~0x800000FFu) | (g_busy ? 0x80000000u : 0u) | ifc | (ofc << 4);
    }
    case 0x10002020u: {
        if (g_starved) return 0;
        const std::uint32_t bp = static_cast<std::uint32_t>(g_bit & 127u);
        std::uint32_t ifc = static_cast<std::uint32_t>(g_in.size() / 16 - std::min(g_in.size() / 16, g_bit / 128));
        std::uint32_t fp = 0;
        if (bp && ifc) {
            fp = 1;
            --ifc;
        }
        return (fp << 16) | (std::min<std::uint32_t>(ifc, 8) << 8) | bp;
    }
    case 0x10002030u:
        return g_busy ? 0u : peek_now();
    case 0x10002034u:
        return g_busy || available() < 32 ? 0x80000000u : 0u;
    default:
        return 0;
    }
}

void write32(std::uint32_t a, std::uint32_t v) {
    switch (a) {
    case 0x10002000u: {
    }
        g_ctrl &= ~(kEcd | kScd);
        g_starved = false;
        ++g_commands[v >> 28];
        g_command = v;
        g_last = static_cast<int>(v >> 28);
        g_busy = g_decoding = g_interrupt = true;
        g_phase = 0;
        g_qsc = (v >> 16) & 0x1Fu;
        g_first = true;
        g_count = v & 0x7FFu;
        step();
        {
            static const char *trace = std::getenv("PS2_TRACE_IPU");
            static std::uint64_t traced = 0, from = trace ? std::strtoull(trace, nullptr, 10) : 0;
            if (trace && ++traced >= from && traced < from + 200000)
                std::fprintf(stderr, "[ipu] cmd 0x%08X -> result %08X ctrl %08X bit %zu next %08X%08X\n", v, g_result, g_ctrl, g_bit,
                             bits_at(g_bit, 32), bits_at(g_bit + 32, 32));
        }
        break;
    case 0x10002010u:
        if (v & kReset) {
            g_starved = false;
            g_in.clear();
            g_out.clear();
            g_bit = g_out_pos = 0;
            g_busy = g_decoding = false;
            g_th0 = g_th1 = 0;
        }
        g_ctrl = (g_ctrl & ~0x3FFF0000u) | (v & 0x3FFF0000u);
        break;
    default:
        if (a >= 0x10007010u && a < 0x10007020u) {
            const std::uint8_t *bytes = reinterpret_cast<const std::uint8_t *>(&v);
            g_in.insert(g_in.end(), bytes, bytes + 4);
            step();
        }
        break;
    }
}

void write_fifo(const std::uint8_t *qword) {
    g_in.insert(g_in.end(), qword, qword + 16);
    step();
}

void read_fifo(std::uint8_t *qword) {
    if (g_out_pos + 16 <= g_out.size()) {
        std::memcpy(qword, g_out.data() + g_out_pos, 16);
        g_out_pos += 16;
    } else {
        std::memset(qword, 0, 16);
    }
    step();
}

void dma_started(int) { step(); }

void stats() {
    std::fprintf(stderr, "[ipu] csc out rgb16 %llu rgb32 %llu\n", (unsigned long long)g_conv16, (unsigned long long)g_conv32);
    std::fprintf(stderr, "[ipu] commands:");
    for (int i = 0; i < 16; ++i)
        if (g_commands[i]) std::fprintf(stderr, " %X:%llu", i, static_cast<unsigned long long>(g_commands[i]));
    std::fprintf(stderr, " stalls %llu out %llu qwords, busy %d decoding %d cmd 0x%08X in %zu bytes bit %zu out %zu\n",
                 static_cast<unsigned long long>(g_stalls), static_cast<unsigned long long>(g_out_qwords), g_busy, g_decoding, g_command,
                 g_in.size(), g_bit, g_out.size() - g_out_pos);
}

}

namespace ps2::ipu {

void state(ps2::state::Stream &s) {
    s.tag("ipu");
    s.vec(g_in);
    s.size(g_bit);
    s.vec(g_out);
    s.size(g_out_pos);
    s.pod(g_ctrl);
    s.pod(g_result);
    s.pod(g_command);
    s.pod(g_last);
    s.pod(g_busy);
    s.pod(g_decoding);
    s.pod(g_interrupt);
    s.pod(g_starved);
    s.pod(g_phase);
    s.pod(g_qsc);
    s.pod(g_count);
    s.pod(g_first);
    s.pod(g_intra_iq);
    s.pod(g_nonintra_iq);
    s.pod(g_vqclut);
    s.pod(g_th0);
    s.pod(g_th1);
    s.pod(g_dc);
}

}
