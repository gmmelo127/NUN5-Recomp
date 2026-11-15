#pragma once

#include "ps2/vu_ops.hpp"

#include <cstdint>
#include <vector>

namespace ps2::vu::jit {

int mode();
ProgramFn compile(const std::uint8_t *code, std::uint32_t entry);

extern bool g_dry;
extern std::int64_t g_escape;
extern std::vector<std::uint32_t> *g_kicks;

}
