#pragma once

#include <cstdint>

namespace ps2 {

using u8x16 = std::uint8_t __attribute__((vector_size(16)));
using s8x16 = std::int8_t __attribute__((vector_size(16)));
using u16x8 = std::uint16_t __attribute__((vector_size(16)));
using s16x8 = std::int16_t __attribute__((vector_size(16)));
using u32x4 = std::uint32_t __attribute__((vector_size(16)));
using s32x4 = std::int32_t __attribute__((vector_size(16)));
using u64x2 = std::uint64_t __attribute__((vector_size(16)));
using s64x2 = std::int64_t __attribute__((vector_size(16)));
using f32x4 = float __attribute__((vector_size(16)));

union alignas(16) Reg128 {
    std::uint64_t d[2];
    std::int64_t sd[2];
    std::uint32_t w[4];
    std::int32_t sw[4];
    std::uint16_t h[8];
    std::int16_t sh[8];
    std::uint8_t b[16];
    std::int8_t sb[16];
    float f[4];
    u8x16 ub16;
    s8x16 sb16;
    u16x8 uh8;
    s16x8 sh8;
    u32x4 uw4;
    s32x4 sw4;
    u64x2 ud2;
    s64x2 sd2;
    f32x4 f4;
};

struct Vu0Regs {
    Reg128 vf[32];
    Reg128 acc;
    std::uint16_t vi[16];
    std::uint32_t q{};
    std::uint32_t p{};
    std::uint32_t i{};
    std::uint32_t r{};
    std::uint32_t mac{};
    std::uint32_t status{};
    std::uint32_t clip{};
    std::uint32_t cmsar0{};
    std::uint32_t tpc{};
};

struct Cpu {
    Reg128 r[32];
    Reg128 hi;
    Reg128 lo;
    std::uint32_t sa{};
    std::uint32_t pc{};
    std::int32_t budget{};
    std::uint32_t base{};

    std::uint32_t f[32];
    std::uint32_t facc{};
    std::uint32_t fcr31{};

    Vu0Regs vu0;
    std::uint32_t cop0[32];
};

inline constexpr std::uint32_t kUnwind = 1u;
inline constexpr std::uint32_t kFpuCondition = 1u << 23;

}
