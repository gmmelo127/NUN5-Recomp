#pragma once

#include <cstdint>

void start_profile(std::uint64_t from_frame);
void write_profile(const char *path);
