#pragma once

#include <functional>

bool open_window(bool fullscreen, bool borderless = false);
int run_window(const std::function<int()> &emulate);
