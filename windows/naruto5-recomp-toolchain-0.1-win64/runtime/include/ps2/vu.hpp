#pragma once

#include "cpu.hpp"
#include "fpu.hpp"

#include <cstdint>

namespace ps2::vu {

inline bool lane(unsigned dest, int i) { return dest & (8u >> i); }

template <typename F>
inline void apply(Vu0Regs &v, unsigned dest, unsigned fd, F f) {
    Reg128 r = v.vf[fd];
    for (int i = 0; i < 4; ++i)
        if (lane(dest, i)) r.w[i] = f(i);
    if (fd) v.vf[fd] = r;
}

template <typename F>
inline void apply_acc(Vu0Regs &v, unsigned dest, F f) {
    Reg128 r = v.acc;
    for (int i = 0; i < 4; ++i)
        if (lane(dest, i)) r.w[i] = f(i);
    v.acc = r;
}

inline void add(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    apply(v, dest, fd, [&](int i) { return fp::add(v.vf[fs].w[i], v.vf[ft].w[i]); });
}
inline void addbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    apply(v, dest, fd, [&](int i) { return fp::add(v.vf[fs].w[i], t); });
}
inline void sub(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    apply(v, dest, fd, [&](int i) { return fp::sub(v.vf[fs].w[i], v.vf[ft].w[i]); });
}
inline void subbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    apply(v, dest, fd, [&](int i) { return fp::sub(v.vf[fs].w[i], t); });
}
inline void mul(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    apply(v, dest, fd, [&](int i) { return fp::mul(v.vf[fs].w[i], v.vf[ft].w[i]); });
}
inline void mulbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    apply(v, dest, fd, [&](int i) { return fp::mul(v.vf[fs].w[i], t); });
}
inline void madd(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    apply(v, dest, fd, [&](int i) { return fp::add(v.acc.w[i], fp::mul(v.vf[fs].w[i], v.vf[ft].w[i])); });
}
inline void maddbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    apply(v, dest, fd, [&](int i) { return fp::add(v.acc.w[i], fp::mul(v.vf[fs].w[i], t)); });
}
inline void msub(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    apply(v, dest, fd, [&](int i) { return fp::sub(v.acc.w[i], fp::mul(v.vf[fs].w[i], v.vf[ft].w[i])); });
}
inline void msubbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    apply(v, dest, fd, [&](int i) { return fp::sub(v.acc.w[i], fp::mul(v.vf[fs].w[i], t)); });
}
inline void max(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    apply(v, dest, fd, [&](int i) { return fp::max(v.vf[fs].w[i], v.vf[ft].w[i]); });
}
inline void maxbc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    apply(v, dest, fd, [&](int i) { return fp::max(v.vf[fs].w[i], t); });
}
inline void mini(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
    apply(v, dest, fd, [&](int i) { return fp::min(v.vf[fs].w[i], v.vf[ft].w[i]); });
}
inline void minibc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t t) {
    apply(v, dest, fd, [&](int i) { return fp::min(v.vf[fs].w[i], t); });
}

inline void adda(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) {
    apply_acc(v, dest, [&](int i) { return fp::add(v.vf[fs].w[i], v.vf[ft].w[i]); });
}
inline void addabc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t t) {
    apply_acc(v, dest, [&](int i) { return fp::add(v.vf[fs].w[i], t); });
}
inline void suba(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) {
    apply_acc(v, dest, [&](int i) { return fp::sub(v.vf[fs].w[i], v.vf[ft].w[i]); });
}
inline void subabc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t t) {
    apply_acc(v, dest, [&](int i) { return fp::sub(v.vf[fs].w[i], t); });
}
inline void mula(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) {
    apply_acc(v, dest, [&](int i) { return fp::mul(v.vf[fs].w[i], v.vf[ft].w[i]); });
}
inline void mulabc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t t) {
    apply_acc(v, dest, [&](int i) { return fp::mul(v.vf[fs].w[i], t); });
}
inline void madda(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) {
    apply_acc(v, dest, [&](int i) { return fp::add(v.acc.w[i], fp::mul(v.vf[fs].w[i], v.vf[ft].w[i])); });
}
inline void maddabc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t t) {
    apply_acc(v, dest, [&](int i) { return fp::add(v.acc.w[i], fp::mul(v.vf[fs].w[i], t)); });
}
inline void msuba(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) {
    apply_acc(v, dest, [&](int i) { return fp::sub(v.acc.w[i], fp::mul(v.vf[fs].w[i], v.vf[ft].w[i])); });
}
inline void msubabc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t t) {
    apply_acc(v, dest, [&](int i) { return fp::sub(v.acc.w[i], fp::mul(v.vf[fs].w[i], t)); });
}

inline void opmula(Vu0Regs &v, unsigned fs, unsigned ft) {
    const Reg128 s = v.vf[fs], t = v.vf[ft];
    v.acc.w[0] = fp::mul(s.w[1], t.w[2]);
    v.acc.w[1] = fp::mul(s.w[2], t.w[0]);
    v.acc.w[2] = fp::mul(s.w[0], t.w[1]);
}

inline void opmsub(Vu0Regs &v, unsigned fd, unsigned fs, unsigned ft) {
    const Reg128 s = v.vf[fs], t = v.vf[ft];
    Reg128 r = v.vf[fd];
    r.w[0] = fp::sub(v.acc.w[0], fp::mul(s.w[1], t.w[2]));
    r.w[1] = fp::sub(v.acc.w[1], fp::mul(s.w[2], t.w[0]));
    r.w[2] = fp::sub(v.acc.w[2], fp::mul(s.w[0], t.w[1]));
    if (fd) v.vf[fd] = r;
}

inline void abs(Vu0Regs &v, unsigned dest, unsigned ft, unsigned fs) {
    apply(v, dest, ft, [&](int i) { return v.vf[fs].w[i] & fp::kAbs; });
}
inline void move(Vu0Regs &v, unsigned dest, unsigned ft, unsigned fs) {
    apply(v, dest, ft, [&](int i) { return v.vf[fs].w[i]; });
}
inline void mr32(Vu0Regs &v, unsigned dest, unsigned ft, unsigned fs) {
    const Reg128 s = v.vf[fs];
    apply(v, dest, ft, [&](int i) { return s.w[(i + 1) & 3]; });
}
inline void ftoi(Vu0Regs &v, unsigned dest, unsigned ft, unsigned fs, int fraction) {
    apply(v, dest, ft, [&](int i) { return fp::to_fixed(v.vf[fs].w[i], fraction); });
}
inline void itof(Vu0Regs &v, unsigned dest, unsigned ft, unsigned fs, int fraction) {
    apply(v, dest, ft, [&](int i) { return fp::from_fixed(v.vf[fs].sw[i], fraction); });
}

inline void clip(Vu0Regs &v, unsigned fs, unsigned ft) {
    const std::int32_t w = fp::key(v.vf[ft].w[3] & fp::kAbs);
    std::uint32_t flags = 0;
    for (int i = 0; i < 3; ++i) {
        const std::int32_t x = fp::key(v.vf[fs].w[i]);
        if (x > w) flags |= 1u << (i * 2);
        if (x < -w) flags |= 2u << (i * 2);
    }
    v.clip = ((v.clip << 6) | flags) & 0xFFFFFFu;
}

inline void div(Vu0Regs &v, std::uint32_t s, std::uint32_t t) {
    const fp::Result r = fp::div(s, t);
    v.q = r.value;
    v.status = (v.status & ~0x30u) | (r.invalid ? 0x20u : 0u);
}
inline void sqrt(Vu0Regs &v, std::uint32_t t) { v.q = fp::sqrt(t); }
inline void rsqrt(Vu0Regs &v, std::uint32_t s, std::uint32_t t) {
    const fp::Result r = fp::rsqrt(s, t);
    v.q = r.value;
    v.status = (v.status & ~0x30u) | (r.invalid ? 0x20u : 0u);
}

inline void mfir(Vu0Regs &v, unsigned dest, unsigned ft, unsigned is) {
    const std::uint32_t value = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(v.vi[is])));
    apply(v, dest, ft, [&](int) { return value; });
}

inline void set_vi(Vu0Regs &v, unsigned index, std::uint32_t value) {
    if (index) v.vi[index] = static_cast<std::uint16_t>(value);
}

inline std::uint32_t rand_next(std::uint32_t r) {
    const std::uint32_t bit = ((r >> 4) ^ (r >> 22)) & 1u;
    return ((r << 1) | bit) & 0x7FFFFFu;
}

}
