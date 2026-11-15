#pragma once

#include <cstdint>
#include <vector>

namespace ps2::vu::jit {

struct Layout {
    std::uint32_t flags = 0;
};

enum Import : std::uint32_t {
    kMac, kStatus, kClip, kForceMac, kEfu, kKick, kTop, kItop, kInterpret, kImports
};

extern const char *const kImportNames[kImports];

bool translate(const std::uint8_t *code, std::uint32_t entry, const Layout &layout, std::vector<std::uint8_t> &out);

}
