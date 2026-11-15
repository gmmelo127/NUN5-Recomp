#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <utility>

namespace ps2::fp {

inline constexpr std::uint32_t kSign = 0x80000000u;
inline constexpr std::uint32_t kAbs = 0x7FFFFFFFu;
inline constexpr std::uint32_t kOne = 0x3F800000u;

inline std::uint32_t exponent(std::uint32_t x) { return (x >> 23) & 0xFFu; }
inline std::uint32_t flush(std::uint32_t x) { return exponent(x) ? x : (x & kSign); }
inline std::uint32_t mantissa(std::uint32_t x) { return (x & 0x7FFFFFu) | 0x800000u; }

inline std::uint32_t pack(std::uint32_t sign, int e, std::uint32_t m) {
    if (e > 255) return sign | kAbs;
    if (e <= 0) return sign;
    return sign | (static_cast<std::uint32_t>(e) << 23) | (m & 0x7FFFFFu);
}

inline std::uint32_t add(std::uint32_t a, std::uint32_t b) {
    a = flush(a);
    b = flush(b);
    std::uint32_t aa = a & kAbs, ba = b & kAbs;
    if (!ba) return aa ? a : (a & b);
    if (!aa) return b;
    if (ba > aa) {
        std::swap(a, b);
        std::swap(aa, ba);
    }
    int e = static_cast<int>(aa >> 23);
    const int diff = e - static_cast<int>(ba >> 23);
    std::uint32_t ma = mantissa(aa) << 1;
    const std::uint32_t mb = diff > 25 ? 0u : (mantissa(ba) << 1) >> diff;
    if ((a ^ b) & kSign) {
        ma -= mb;
        if (!ma) return 0;
        const int shift = std::countl_zero(ma) - 7;
        ma <<= shift;
        e -= shift;
    } else {
        ma += mb;
        if (ma & 0x2000000u) {
            ma >>= 1;
            ++e;
        }
    }
    return pack(a & kSign, e, ma >> 1);
}

inline std::uint32_t sub(std::uint32_t a, std::uint32_t b) { return add(a, b ^ kSign); }

inline std::uint32_t mul(std::uint32_t a, std::uint32_t b) {
    const std::uint32_t sign = (a ^ b) & kSign;
    const int ea = static_cast<int>(exponent(a)), eb = static_cast<int>(exponent(b));
    if (!ea || !eb) return sign;
    std::uint64_t m = static_cast<std::uint64_t>(mantissa(a)) * mantissa(b);
    int e = ea + eb - 127;
    if (m >> 47) {
        m >>= 24;
        ++e;
    } else {
        m >>= 23;
    }
    return pack(sign, e, static_cast<std::uint32_t>(m));
}

inline double to_double(std::uint32_t x) {
    if (!exponent(x)) return (x & kSign) ? -0.0 : 0.0;
    const std::uint64_t bits = (static_cast<std::uint64_t>(x & kSign) << 32) |
                               (static_cast<std::uint64_t>(exponent(x) + 1023 - 127) << 52) |
                               (static_cast<std::uint64_t>(x & 0x7FFFFFu) << 29);
    return std::bit_cast<double>(bits);
}

inline std::uint32_t from_double(double d) {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(d);
    const std::uint32_t sign = static_cast<std::uint32_t>(bits >> 32) & kSign;
    const int e = static_cast<int>((bits >> 52) & 0x7FFu);
    if (!e) return sign;
    return pack(sign, e - 1023 + 127, static_cast<std::uint32_t>(bits >> 29));
}

struct Result {
    std::uint32_t value;
    bool invalid;
};

inline Result div(std::uint32_t a, std::uint32_t b) {
    a = flush(a);
    b = flush(b);
    const std::uint32_t sign = (a ^ b) & kSign;
    if (!(b & kAbs)) return {sign | kAbs, true};
    if (!(a & kAbs)) return {sign, false};
    return {from_double(to_double(a) / to_double(b)), false};
}

inline std::uint32_t sqrt(std::uint32_t a) {
    a = flush(a) & kAbs;
    if (!a) return 0;
    return from_double(std::sqrt(to_double(a)));
}

inline Result rsqrt(std::uint32_t a, std::uint32_t b) {
    a = flush(a);
    b = flush(b) & kAbs;
    if (!b) return {(a & kSign) | kAbs, true};
    if (!(a & kAbs)) return {a & kSign, false};
    return {from_double(to_double(a) / std::sqrt(to_double(b))), false};
}

inline std::int32_t key(std::uint32_t x) {
    x = flush(x);
    const auto magnitude = static_cast<std::int32_t>(x & kAbs);
    return (x & kSign) ? -magnitude : magnitude;
}

inline std::uint32_t max(std::uint32_t a, std::uint32_t b) { return key(a) >= key(b) ? a : b; }
inline std::uint32_t min(std::uint32_t a, std::uint32_t b) { return key(a) <= key(b) ? a : b; }

inline std::uint32_t to_fixed(std::uint32_t x, int fraction) {
    const int e = static_cast<int>(exponent(x)) + fraction;
    if (e <= 126 || !exponent(x)) return 0;
    const bool negative = x & kSign;
    if (e >= 158) return negative ? 0x80000000u : 0x7FFFFFFFu;
    const int shift = e - 150;
    const std::uint32_t m = mantissa(x);
    const std::uint32_t v = shift >= 0 ? m << shift : m >> -shift;
    return negative ? 0u - v : v;
}

inline std::uint32_t from_fixed(std::int32_t v, int fraction) {
    return from_double(static_cast<double>(v) / static_cast<double>(1u << fraction));
}

inline std::uint32_t cvt_w_s(std::uint32_t x) { return to_fixed(x, 0); }
inline std::uint32_t cvt_s_w(std::uint32_t v) { return from_fixed(static_cast<std::int32_t>(v), 0); }

}
