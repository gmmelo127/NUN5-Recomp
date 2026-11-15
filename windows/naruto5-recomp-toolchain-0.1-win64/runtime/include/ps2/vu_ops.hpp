#pragma once

#include "cpu.hpp"
#include "fpu.hpp"
#include "vu.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#define PS2_VU_INLINE inline __attribute__((always_inline))

namespace ps2::vu {

void kick(int unit, std::uint32_t address);
std::uint32_t top();
std::uint32_t itop();
void interpret(int unit, std::uint32_t pc);

using ProgramFn = void (*)(Vu0Regs &r, std::uint8_t *mem);

struct Program {
    std::uint64_t key;
    ProgramFn function;
};

void register_programs(const Program *programs, std::uint32_t count);

inline std::uint32_t lower_major(std::uint32_t lo) { return lo >> 25; }
// Flag reads bypass the one-instruction VI delay seen by branch operands.
inline bool branch_delay_exempt(std::uint32_t lo) {
    switch (lower_major(lo)) {
    case 0x10: case 0x12: case 0x13: case 0x14: case 0x16: case 0x17: case 0x18: case 0x1A: case 0x1B: case 0x1C: case 0x21: case 0x25: return true;
    default: return false;
    }
}
inline bool is_branch(std::uint32_t lo) {
    const std::uint32_t op = lower_major(lo);
    return op == 0x20 || op == 0x21 || op == 0x24 || op == 0x25 || (op >= 0x28 && op <= 0x2F && op != 0x2A && op != 0x2B);
}
inline bool is_jump_register(std::uint32_t lo) { return lower_major(lo) == 0x24 || lower_major(lo) == 0x25; }
inline bool is_unconditional(std::uint32_t lo) { return lower_major(lo) == 0x20 || lower_major(lo) == 0x21; }
inline bool links(std::uint32_t lo) { return lower_major(lo) == 0x21 || lower_major(lo) == 0x25; }
inline std::uint32_t branch_target(std::uint32_t lo, std::uint32_t pc, std::uint32_t mask) {
    const std::int32_t imm11 = static_cast<std::int32_t>(lo << 21) >> 21;
    return (pc + 8u + static_cast<std::uint32_t>(imm11) * 8u) & mask;
}

inline std::vector<std::uint32_t> reachable(const std::uint8_t *code, std::uint32_t mask, std::uint32_t entry) {
    std::vector<std::uint8_t> seen((mask + 1) / 8, 0);
    std::vector<std::uint32_t> work{entry & mask}, out;
    while (!work.empty()) {
        std::uint32_t pc = work.back();
        work.pop_back();
        int stop_after = -1;
        for (int guard = 0; guard < 4096; ++guard) {
            if (seen[pc >> 3]) break;
            seen[pc >> 3] = 1;
            out.push_back(pc);
            if (stop_after == 0) break;
            if (stop_after > 0) --stop_after;
            std::uint32_t lo, hi;
            std::memcpy(&lo, code + pc, 4);
            std::memcpy(&hi, code + pc + 4, 4);
            if (hi & 0x40000000u) stop_after = 0;
            if (!(hi & 0x80000000u) && is_branch(lo)) {
                if (!is_jump_register(lo)) work.push_back(branch_target(lo, pc, mask));
                if (links(lo)) work.push_back((pc + 16u) & mask);
                if (is_unconditional(lo) || is_jump_register(lo)) stop_after = 0;
            }
            pc = (pc + 8u) & mask;
        }
    }
    std::vector<std::uint32_t> sorted(out);
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

struct VfUse {
    std::uint8_t reg[4] = {}, mask[4] = {}, wreg[2] = {}, wmask[2] = {};
    unsigned n = 0, wn = 0;
    std::uint16_t vi_reads = 0;
    unsigned vi_load = 0;
    void read_vi(unsigned r) { vi_reads = static_cast<std::uint16_t>(vi_reads | ((1u << (r & 15u)) & ~1u)); }
    void read(unsigned r, unsigned m) {
        if (r && m) {
            reg[n] = static_cast<std::uint8_t>(r);
            mask[n++] = static_cast<std::uint8_t>(m);
        }
    }
    void write(unsigned r, unsigned m) {
        if (r && m) {
            wreg[wn] = static_cast<std::uint8_t>(r);
            wmask[wn++] = static_cast<std::uint8_t>(m);
        }
    }
};

inline VfUse vf_use(std::uint32_t lo, std::uint32_t hi) {
    VfUse u;
    const unsigned dest = (hi >> 21) & 15u, ft = (hi >> 16) & 31u, fs = (hi >> 11) & 31u, fd = (hi >> 6) & 31u, bc = 8u >> (hi & 3u), funct = hi & 63u;
    if (funct < 0x3C) {
        if (funct < 0x1C) {
            u.read(fs, dest);
            u.read(ft, bc);
        } else if (funct < 0x28) {
            u.read(fs, dest);
        } else if (funct == 0x2E) {
            u.read(fs, 0xE);
            u.read(ft, 0xE);
        } else {
            u.read(fs, dest);
            u.read(ft, dest);
        }
        u.write(fd, funct == 0x2E ? 0xE : dest);
    } else {
        const unsigned index = ((hi >> 6) & 31u) * 4u + (funct & 3u);
        if (index < 0x10 || (index >= 0x18 && index < 0x1C)) {
            u.read(fs, dest);
            u.read(ft, bc);
        } else if (index < 0x18 || index == 0x1D) {
            u.read(fs, dest);
            u.write(ft, dest);
        } else if (index == 0x1F) {
            u.read(fs, 0xE);
            u.read(ft, 1);
        } else if (index < 0x28) {
            u.read(fs, dest);
        } else if (index == 0x2E) {
            u.read(fs, 0xE);
            u.read(ft, 0xE);
        } else if (index < 0x2F) {
            u.read(fs, dest);
            u.read(ft, dest);
        }
    }
    if (hi & 0x80000000u) return u;
    const unsigned ldest = (lo >> 21) & 15u, lft = (lo >> 16) & 31u, lfs = (lo >> 11) & 31u, fsf = 8u >> ((lo >> 21) & 3u), ftf = 8u >> ((lo >> 23) & 3u);
    const unsigned it = (lo >> 16) & 15u, is = (lo >> 11) & 15u, major = lower_major(lo);
    if (major == 0x00 || major == 0x08 || major == 0x09 || major == 0x18 || major == 0x1A || major == 0x1B || major == 0x24 || major == 0x25 || (major >= 0x2C && major <= 0x2F)) u.read_vi(is);
    if (major == 0x01) u.read_vi(it);
    if (major == 0x04 || major == 0x05 || major == 0x28 || major == 0x29) u.read_vi(is);
    if (major == 0x05 || major == 0x28 || major == 0x29) u.read_vi(it);
    if (major == 0x04) u.vi_load = it;
    if (major == 0x40 && (lo & 0x3Cu) != 0x3Cu) {
        const unsigned funct = lo & 0x3Fu;
        if (funct == 0x30 || funct == 0x31 || funct == 0x32 || funct == 0x34 || funct == 0x35) u.read_vi(is);
        if (funct == 0x30 || funct == 0x31 || funct == 0x34 || funct == 0x35) u.read_vi(it);
    }
    if (major == 0x40 && (lo & 0x3Cu) == 0x3Cu) {
        const unsigned index = ((lo >> 6) & 31u) * 4u + (lo & 3u);
        if (index == 0x34 || index == 0x36 || index == 0x3D || index == 0x3E || index == 0x3F || index == 0x6C) u.read_vi(is);
        if (index == 0x35 || index == 0x37 || index == 0x3F) u.read_vi(it);
        if (index == 0x3E) u.vi_load = it;
    }
    if (major == 0x00) u.write(lft, ldest);
    else if (major == 0x01) u.read(lfs, ldest);
    else if (major == 0x40 && (lo & 0x3Cu) == 0x3Cu) {
        const unsigned index = ((lo >> 6) & 31u) * 4u + (lo & 3u);
        switch (index) {
        case 0x30: u.read(lfs, ldest); u.write(lft, ldest); break;
        case 0x31: u.read(lfs, ((ldest >> 1) | (ldest << 3)) & 15u); u.write(lft, ldest); break;
        case 0x34: case 0x36: case 0x3D: case 0x40: case 0x41: case 0x64: u.write(lft, ldest); break;
        case 0x35: case 0x37: u.read(lfs, ldest); break;
        case 0x38: case 0x3A: u.read(lfs, fsf); u.read(lft, ftf); break;
        case 0x39: u.read(lft, ftf); break;
        case 0x3C: case 0x42: case 0x43: u.read(lfs, fsf); break;
        default: if (index >= 0x70) u.read(lfs, 0xF); break;
        }
    }
    return u;
}

inline std::uint32_t issue(const VfUse &u, std::uint32_t cycle, const std::uint32_t (&ready)[32][4], const std::uint32_t (&vi_ready)[16]) {
    for (unsigned i = 0; i < u.n; ++i)
        for (unsigned lane = 0; lane < 4; ++lane)
            if (u.mask[i] & (8u >> lane)) cycle = std::max(cycle, ready[u.reg[i]][lane]);
    for (unsigned r = 1; r < 16; ++r)
        if (u.vi_reads & (1u << r)) cycle = std::max(cycle, vi_ready[r]);
    return cycle;
}

inline void retire(const VfUse &u, std::uint32_t cycle, std::uint32_t (&ready)[32][4], std::uint32_t (&vi_ready)[16]) {
    for (unsigned i = 0; i < u.wn; ++i)
        for (unsigned lane = 0; lane < 4; ++lane)
            if (u.wmask[i] & (8u >> lane)) ready[u.wreg[i]][lane] = cycle + 4u;
    if (u.vi_load) vi_ready[u.vi_load] = cycle + 4u;
}

inline std::uint32_t q_delay(const std::uint8_t *code, std::uint32_t mask, std::uint32_t pc, std::uint32_t latency) {
    std::uint32_t ready[32][4] = {}, vi_ready[16] = {};
    std::uint32_t cycle = 0, issued = 0;
    for (int k = -3; k <= static_cast<int>(latency); ++k) {
        const std::uint32_t at = (pc + static_cast<std::uint32_t>(k) * 8u) & mask;
        std::uint32_t lo, hi;
        std::memcpy(&lo, code + at, 4);
        std::memcpy(&hi, code + at + 4, 4);
        const VfUse u = vf_use(lo, hi);
        cycle = issue(u, cycle + 1u, ready, vi_ready);
        if (k == 0) issued = cycle;
        else if (k > 0 && cycle >= issued + latency) return static_cast<std::uint32_t>(k);
        retire(u, cycle, ready, vi_ready);
    }
    return latency;
}

inline std::uint32_t q_unit_latency(std::uint32_t lo, std::uint32_t hi) {
    if ((hi & 0x80000000u) || lower_major(lo) != 0x40 || (lo & 0x3Cu) != 0x3Cu) return 0;
    const unsigned index = ((lo >> 6) & 31u) * 4u + (lo & 3u);
    return index == 0x38 || index == 0x39 ? 7u : index == 0x3A ? 13u : 0u;
}

inline bool waits_q(std::uint32_t lo, std::uint32_t hi) {
    return !(hi & 0x80000000u) && lower_major(lo) == 0x40 && (lo & 0x3Fu) == 0x3Fu && ((lo >> 6) & 31u) == 0x0Eu;
}

inline bool reads_q(std::uint32_t hi) {
    const unsigned funct = hi & 63u, op = funct < 0x3C ? funct : ((hi >> 6) & 31u) * 4u + (funct & 3u);
    return op == 0x1C || op == 0x20 || op == 0x21 || op == 0x24 || op == 0x25;
}

enum QRead : std::uint8_t { kQDynamic, kQNew, kQOld };

inline QRead q_read_kind(const std::uint8_t *code, std::uint32_t mask, std::uint32_t pc) {
    const auto load = [&](std::uint32_t a, std::uint32_t &lo, std::uint32_t &hi) {
        std::memcpy(&lo, code + (a & mask), 4);
        std::memcpy(&hi, code + (a & mask) + 4, 4);
    };
    {
        std::uint32_t lo, hi;
        load(pc, lo, hi);
        if (waits_q(lo, hi) || q_unit_latency(lo, hi)) return kQNew;
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> jumps;
    for (std::uint32_t a = 0; a <= mask; a += 8) {
        std::uint32_t lo, hi;
        load(a, lo, hi);
        if (!(hi & 0x80000000u) && is_branch(lo) && !is_jump_register(lo)) jumps.emplace_back(branch_target(lo, a, mask), (a + 8u) & mask);
    }
    const auto preds = [&](std::uint32_t y) {
        std::vector<std::uint32_t> out;
        std::uint32_t lo, hi;
        load(y - 16u, lo, hi);
        const bool jumped = !(hi & 0x80000000u) && is_branch(lo) && (is_unconditional(lo) || is_jump_register(lo));
        if (!jumped && !(hi & 0x40000000u)) out.push_back((y - 8u) & mask);
        for (const auto &[target, slot] : jumps)
            if (target == (y & mask)) out.push_back(slot);
        return out;
    };
    const auto complete = [&](const std::vector<std::uint32_t> &back, std::uint32_t latency) {
        std::vector<std::uint32_t> order;
        const std::uint32_t div = back.back();
        for (std::uint32_t k = 3; k > 0; --k) order.push_back((div - 8u * k) & mask);
        order.insert(order.end(), back.rbegin(), back.rend());
        std::uint32_t ready[32][4] = {}, vi_ready[16] = {};
        std::uint32_t cycle = 0, issued = 0;
        for (std::size_t i = 0; i < order.size(); ++i) {
            std::uint32_t lo, hi;
            load(order[i], lo, hi);
            const VfUse u = vf_use(lo, hi);
            cycle = issue(u, cycle + 1u, ready, vi_ready);
            if (i == 3) issued = cycle;
            retire(u, cycle, ready, vi_ready);
        }
        return cycle >= issued + latency;
    };
    bool any_new = false, any_old = false;
    unsigned budget = 4096;
    std::vector<std::vector<std::uint32_t>> stack{{pc & mask}};
    while (!stack.empty()) {
        if (!--budget) return kQDynamic;
        const std::vector<std::uint32_t> path = std::move(stack.back());
        stack.pop_back();
        const std::uint32_t at = path.back();
        if (path.size() > 1) {
            std::uint32_t lo, hi;
            load(at, lo, hi);
            if (const std::uint32_t latency = q_unit_latency(lo, hi)) {
                (complete(path, latency) ? any_new : any_old) = true;
                continue;
            }
            if (waits_q(lo, hi) || path.size() > 15) {
                any_new = true;
                continue;
            }
        }
        const std::vector<std::uint32_t> from = preds(at);
        if (from.empty()) any_new = true;
        for (std::uint32_t p : from) {
            std::vector<std::uint32_t> next = path;
            next.push_back(p);
            stack.push_back(std::move(next));
        }
        if (any_new && any_old) return kQDynamic;
    }
    return any_old ? kQOld : kQNew;
}

inline std::uint64_t program_key(const std::uint8_t *code, const std::vector<std::uint32_t> &pcs, std::uint32_t entry) {
    std::uint64_t h = 0xCBF29CE484222325ull ^ entry;
    const auto mix = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h ^= (v >> (i * 8)) & 0xFFu;
            h *= 0x100000001B3ull;
        }
    };
    for (std::uint32_t pc : pcs) {
        std::uint32_t lo, hi;
        std::memcpy(&lo, code + pc, 4);
        std::memcpy(&hi, code + pc + 4, 4);
        mix(pc);
        mix(lo);
        mix(hi);
    }
    return h;
}

}

namespace ps2::vuops {

PS2_VU_INLINE float fin(std::uint32_t x) {
    const std::uint32_t e = x & 0x7F800000u;
    if (!e) x &= 0x80000000u;
    else if (e == 0x7F800000u) x = (x & 0x80000000u) | 0x7F7FFFFFu;
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

PS2_VU_INLINE std::uint32_t fout(float f) {
    std::uint32_t x;
    std::memcpy(&x, &f, 4);
    const std::uint32_t e = x & 0x7F800000u;
    if (!e) return x & 0x80000000u;
    if (e == 0x7F800000u) return (x & 0x80000000u) | 0x7F7FFFFFu;
    return x;
}

PS2_VU_INLINE float norm(float f) { return fin(fout(f)); }
PS2_VU_INLINE std::uint32_t add(std::uint32_t a, std::uint32_t b) { return fout(fin(a) + fin(b)); }
PS2_VU_INLINE std::uint32_t sub(std::uint32_t a, std::uint32_t b) { return fout(fin(a) - fin(b)); }
PS2_VU_INLINE std::uint32_t mul(std::uint32_t a, std::uint32_t b) { return fout(fin(a) * fin(b)); }
PS2_VU_INLINE std::uint32_t madd(std::uint32_t acc, std::uint32_t a, std::uint32_t b) { return fout(fin(acc) + norm(fin(a) * fin(b))); }
PS2_VU_INLINE std::uint32_t msub(std::uint32_t acc, std::uint32_t a, std::uint32_t b) { return fout(fin(acc) - norm(fin(a) * fin(b))); }
PS2_VU_INLINE std::uint32_t max(std::uint32_t a, std::uint32_t b) { return fp::max(a, b); }
PS2_VU_INLINE std::uint32_t min(std::uint32_t a, std::uint32_t b) { return fp::min(a, b); }

typedef std::uint32_t v4u __attribute__((vector_size(16)));
typedef std::int32_t v4i __attribute__((vector_size(16)));
typedef float v4f __attribute__((vector_size(16)));

PS2_VU_INLINE v4u vload(const Reg128 &r) {
    v4u v;
    std::memcpy(&v, &r, 16);
    return v;
}
PS2_VU_INLINE void vstore(Reg128 &r, v4u v) { std::memcpy(&r, &v, 16); }
PS2_VU_INLINE v4u vsplat(std::uint32_t x) { return v4u{x, x, x, x}; }
PS2_VU_INLINE v4u vsel(v4u old, v4u now, v4u mask) { return (now & mask) | (old & ~mask); }
PS2_VU_INLINE v4u vfix(v4u x) {
#if defined(__SSE2__)
    const __m128i v = (__m128i)x;
    const __m128i abs = _mm_and_si128(v, _mm_set1_epi32(0x7FFFFFFF));
    const __m128i limit = _mm_set1_epi32(0x7F7FFFFF);
    const __m128i lt = _mm_cmplt_epi32(abs, limit);
    const __m128i clamped = _mm_or_si128(_mm_and_si128(lt, abs), _mm_andnot_si128(lt, limit));
    const __m128i normal = _mm_cmpgt_epi32(clamped, _mm_set1_epi32(0x007FFFFF));
    const __m128i sign = _mm_and_si128(v, _mm_set1_epi32(static_cast<int>(0x80000000u)));
    return (v4u)_mm_or_si128(sign, _mm_and_si128(clamped, normal));
#else
    std::uint32_t temp[4];
    std::memcpy(temp, &x, 16);
    std::int32_t clamped[4];
    for(int i=0; i<4; ++i) {
        clamped[i] = std::min(static_cast<std::int32_t>(temp[i] & 0x7FFFFFFFu), 0x7F7FFFFF);
    }
    v4i cv;
    std::memcpy(&cv, clamped, 16);
    const v4i normal = (v4f)cv >= (v4f)(v4i){0x00800000, 0x00800000, 0x00800000, 0x00800000};
    return (x & 0x80000000u) | (v4u)(cv & normal);
#endif
}
PS2_VU_INLINE v4f vin(v4u x) { return (v4f)vfix(x); }
PS2_VU_INLINE v4u vout(v4f f) { return vfix((v4u)f); }
PS2_VU_INLINE v4f vnorm(v4f f) { return (v4f)vfix((v4u)f); }
// The n variants take operands that already went through vfix.
PS2_VU_INLINE v4u vaddn(v4u a, v4u b) { return vout((v4f)a + (v4f)b); }
PS2_VU_INLINE v4u vsubn(v4u a, v4u b) { return vout((v4f)a - (v4f)b); }
PS2_VU_INLINE v4u vmuln(v4u a, v4u b) { return vout((v4f)a * (v4f)b); }
PS2_VU_INLINE v4u vmaddn(v4u acc, v4u a, v4u b) { return vout((v4f)acc + vnorm((v4f)a * (v4f)b)); }
PS2_VU_INLINE v4u vmsubn(v4u acc, v4u a, v4u b) { return vout((v4f)acc - vnorm((v4f)a * (v4f)b)); }
PS2_VU_INLINE v4u vadd(v4u a, v4u b) { return vaddn(vfix(a), vfix(b)); }
PS2_VU_INLINE v4u vsub(v4u a, v4u b) { return vsubn(vfix(a), vfix(b)); }
PS2_VU_INLINE v4u vmul(v4u a, v4u b) { return vmuln(vfix(a), vfix(b)); }
PS2_VU_INLINE v4u vmadd(v4u acc, v4u a, v4u b) { return vmaddn(vfix(acc), vfix(a), vfix(b)); }
PS2_VU_INLINE v4u vmsub(v4u acc, v4u a, v4u b) { return vmsubn(vfix(acc), vfix(a), vfix(b)); }
PS2_VU_INLINE v4i vkey(v4u x) {
    const v4u zero = (v4u)((x & 0x7F800000u) == 0u);
    const v4u f = (x & ~zero) | (x & 0x80000000u & zero);
    const v4i m = (v4i)(f & 0x7FFFFFFFu);
    const v4i negative = (v4i)((f & 0x80000000u) != 0u);
    return (m & ~negative) | (-m & negative);
}
PS2_VU_INLINE v4u vmax(v4u a, v4u b) {
    const v4u ge = (v4u)(vkey(a) >= vkey(b));
    return (a & ge) | (b & ~ge);
}
PS2_VU_INLINE v4u vmin(v4u a, v4u b) {
    const v4u le = (v4u)(vkey(a) <= vkey(b));
    return (a & le) | (b & ~le);
}
PS2_VU_INLINE v4u vabs(v4u a) { return a & 0x7FFFFFFFu; }
PS2_VU_INLINE v4u vftoi(v4u x, int fraction) {
    const v4f f = (v4f)vfix(x) * static_cast<float>(1u << fraction);
    const v4i big = f >= 2147483648.0f;
    const v4i small = f < -2147483648.0f;
    const v4i out = big | small;
    const v4i t = __builtin_convertvector((v4f)((v4i)f & ~out), v4i);
    return (v4u)((t & ~out) | (big & 0x7FFFFFFF) | (small & (v4i)vsplat(0x80000000u)));
}
PS2_VU_INLINE v4u vitof(v4u x, int fraction) {
    const v4i v = (v4i)x;
    v4f f = __builtin_convertvector(v, v4f);
    bool any_overflow = false;
    std::uint32_t temp[4];
    std::memcpy(temp, &x, 16);
    for(int i=0; i<4; ++i) {
        if ((std::int32_t)temp[i] + 0x01000000u > 0x02000000u) {
            any_overflow = true;
            break;
        }
    }
    if (any_overflow) {
        const v4i big = f >= 2147483648.0f;
        const v4i back = __builtin_convertvector((v4f)((v4i)f & ~big), v4i);
        const v4i over = big | ((v >= 0) & (back > v)) | ((v < 0) & (back < v));
        f = (v4f)((v4i)f + over);
    }
    return (v4u)(f * (1.0f / static_cast<float>(1u << fraction)));
}

PS2_VU_INLINE std::uint32_t mac_bits(v4u x, unsigned dest) {
    // The four invalid/sign/max-exponent MAC bits for the selected lanes.
    // This runs for every flag-setting VU instruction, so avoid the scalar
    // lane loop: data-dependent branches mispredict on real vertex data.
    const unsigned d = dest & 0xFu;
    if (!d) return 0;
#if defined(__SSE2__)
    // Reverse the lanes once so movemask already produces VU MAC bit order
    // (lane 0 is MAC bit 3), instead of reversing three 4-bit masks.
    const __m128i v = _mm_shuffle_epi32((__m128i)x, 0x1B);
    const __m128i inv = _mm_cmpeq_epi32(_mm_and_si128(v, _mm_set1_epi32(0x7F800000)), _mm_setzero_si128());
    const __m128i mx = _mm_cmpeq_epi32(_mm_and_si128(v, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7FFFFFFF));
    const unsigned invalid = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(inv)));
    const unsigned sign = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(v)));
    const unsigned maxexp = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(mx)));
    return (invalid | (sign << 4) | (maxexp << 12)) & (d | (d << 4) | (d << 12));
#else
    std::uint32_t mac = 0;
    for (int i = 0; i < 4; ++i) {
        if (!(dest & (8u >> i))) continue;
        const std::uint32_t bit = 3u - static_cast<std::uint32_t>(i);
        const std::uint32_t lane = x[i];
        if (!(lane & 0x7F800000u)) mac |= 1u << bit;
        if (lane & 0x80000000u) mac |= 1u << (bit + 4);
        if ((lane & 0x7FFFFFFFu) == 0x7FFFFFFFu) mac |= 1u << (bit + 12);
    }
    return mac;
#endif
}

PS2_VU_INLINE void flags(v4u x, unsigned dest, std::uint32_t &mac_out, std::uint32_t &status_io) {
    const std::uint32_t mac = mac_bits(x, dest);
    mac_out = mac;
    std::uint32_t status = status_io & 0xFF0u;
    status |= (mac & 0x000Fu) != 0;
    status |= ((mac & 0x00F0u) != 0) << 1;
    status |= ((mac & 0x0F00u) != 0) << 2;
    status |= ((mac & 0xF000u) != 0) << 3;
    status |= (status & 0xFu) << 6;
    status_io = (status & ~0x30u) | (status_io & 0x30u);
}

PS2_VU_INLINE void flags(Vu0Regs &r, v4u x, unsigned dest) { flags(x, dest, r.mac, r.status); }

PS2_VU_INLINE void flags(Vu0Regs &r, const Reg128 &v, unsigned dest) { flags(r, vload(v), dest); }

PS2_VU_INLINE std::uint32_t clip_value(const Vu0Regs &r, const Reg128 &s, std::uint32_t w) {
    const std::int32_t lim = fp::key(w & fp::kAbs);
    std::uint32_t f = 0;
    for (int i = 0; i < 3; ++i) {
        const std::int32_t x = fp::key(s.w[i]);
        if (x > lim) f |= 1u << (i * 2);
        if (x < -lim) f |= 2u << (i * 2);
    }
    return ((r.clip << 6) | f) & 0xFFFFFFu;
}

PS2_VU_INLINE void clip(Vu0Regs &r, const Reg128 &s, std::uint32_t w) {
    const std::int32_t lim = fp::key(w & fp::kAbs);
    std::uint32_t f = 0;
    for (int i = 0; i < 3; ++i) {
        const std::int32_t x = fp::key(s.w[i]);
        if (x > lim) f |= 1u << (i * 2);
        if (x < -lim) f |= 2u << (i * 2);
    }
    r.clip = ((r.clip << 6) | f) & 0xFFFFFFu;
}

struct FlagPipe {
    std::uint32_t t = 16;
    std::uint32_t mac_ts[8] = {}, mac_v[8] = {}, status_v[8] = {}, clip_ts[8] = {}, clip_v[8] = {};
    unsigned mh = 0, ch = 0;
    std::uint32_t q_value = 0, q_ready = 0;
    bool q_pending = false;

    PS2_VU_INLINE std::uint32_t begin_q(Vu0Regs &r) {
        flush_q(r);
        return r.q;
    }
    PS2_VU_INLINE void end_q(Vu0Regs &r, std::uint32_t before, std::uint32_t latency) {
        q_value = r.q;
        r.q = before;
        q_ready = t + latency;
        q_pending = true;
    }
    PS2_VU_INLINE void sync_q(Vu0Regs &r) {
        if (q_pending && t >= q_ready) flush_q(r);
    }
    PS2_VU_INLINE void flush_q(Vu0Regs &r) {
        if (!q_pending) return;
        r.q = q_value;
        q_pending = false;
    }

    void start(const Vu0Regs &r) {
        for (int i = 0; i < 8; ++i) {
            mac_ts[i] = clip_ts[i] = 0;
            mac_v[i] = r.mac;
            status_v[i] = r.status;
            clip_v[i] = r.clip;
        }
    }
    PS2_VU_INLINE void set_mac(std::uint32_t mac, std::uint32_t status) {
        const unsigned i = mh++ & 7u;
        mac_ts[i] = t;
        mac_v[i] = mac;
        status_v[i] = status;
    }
    PS2_VU_INLINE void set_clip(std::uint32_t clip, std::uint32_t at) {
        const unsigned i = ch++ & 7u;
        clip_ts[i] = at;
        clip_v[i] = clip;
    }
    PS2_VU_INLINE void force_mac(std::uint32_t mac, std::uint32_t status) {
        set_mac(mac, status);
        mac_ts[(mh - 1) & 7u] = t - 4u;
    }
    PS2_VU_INLINE unsigned mac_slot() const {
        for (unsigned k = 1; k <= 8; ++k) {
            const unsigned i = (mh - k) & 7u;
            if (mac_ts[i] + 4u <= t) return i;
        }
        return (mh - 8) & 7u;
    }
    PS2_VU_INLINE std::uint32_t mac() const { return mac_v[mac_slot()]; }
    PS2_VU_INLINE std::uint32_t status() const { return status_v[mac_slot()]; }
    PS2_VU_INLINE std::uint32_t clip() const {
        for (unsigned k = 1; k <= 8; ++k) {
            const unsigned i = (ch - k) & 7u;
            if (clip_ts[i] + 3u <= t) return clip_v[i];
        }
        return clip_v[(ch - 8) & 7u];
    }
};

PS2_VU_INLINE std::uint8_t *qword(std::uint8_t *mem, std::uint32_t index) { return mem + (index & 1023u) * 16u; }
PS2_VU_INLINE void set_vi(Vu0Regs &r, unsigned i, std::uint32_t v) {
    if (i) r.vi[i] = static_cast<std::uint16_t>(v);
}
PS2_VU_INLINE void set_vi(std::uint16_t &dst, std::uint32_t v) { dst = static_cast<std::uint16_t>(v); }
PS2_VU_INLINE void drop_vi(std::uint32_t) {}

PS2_VU_INLINE std::uint32_t f2u(float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}

PS2_VU_INLINE float u2f(std::uint32_t v) {
    if (!(v & 0x7F800000u)) v &= 0x80000000u;
    if ((v & 0x7F800000u) == 0x7F800000u) v = (v & 0x80000000u) | 0x7F7FFFFFu;
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

inline void efu(Vu0Regs &r, unsigned index, unsigned fs, unsigned fsf) {
    switch (index) {
    case 0x70: case 0x71: case 0x72: case 0x73: {
        const float x = u2f(r.vf[fs].w[0]), y = u2f(r.vf[fs].w[1]), z = u2f(r.vf[fs].w[2]);
        const float sum = x * x + y * y + z * z;
        float p = sum;
        if (index == 0x71) p = sum != 0.0f ? 1.0f / sum : 3.4e38f;
        if (index == 0x72) p = std::sqrt(sum);
        if (index == 0x73) p = sum != 0.0f ? 1.0f / std::sqrt(sum) : 3.4e38f;
        r.p = f2u(p);
        break;
    }
    case 0x74: r.p = f2u(std::atan2(u2f(r.vf[fs].w[1]), u2f(r.vf[fs].w[0]))); break;
    case 0x75: r.p = f2u(std::atan2(u2f(r.vf[fs].w[2]), u2f(r.vf[fs].w[0]))); break;
    case 0x76: r.p = f2u(u2f(r.vf[fs].w[0]) + u2f(r.vf[fs].w[1]) + u2f(r.vf[fs].w[2]) + u2f(r.vf[fs].w[3])); break;
    case 0x78: r.p = f2u(std::sqrt(std::fabs(u2f(r.vf[fs].w[fsf])))); break;
    case 0x79: { const float v = std::sqrt(std::fabs(u2f(r.vf[fs].w[fsf]))); r.p = f2u(v != 0.0f ? 1.0f / v : 3.4e38f); break; }
    case 0x7A: { const float v = u2f(r.vf[fs].w[fsf]); r.p = f2u(v != 0.0f ? 1.0f / v : 3.4e38f); break; }
    case 0x7C: r.p = f2u(std::sin(u2f(r.vf[fs].w[fsf]))); break;
    case 0x7D: r.p = f2u(std::atan(u2f(r.vf[fs].w[fsf]))); break;
    case 0x7E: r.p = f2u(std::exp(-u2f(r.vf[fs].w[fsf]))); break;
    default: break;
    }
}

}
