#pragma once
#include <cstdint>
#include <vector>

namespace ps2::menu {

// Draws the current menu into an RGBA8 buffer of w*h (straight alpha). The UI
// is laid out on a 1920x1080 canvas and fitted (letterboxed) into the target,
// so it never stretches when the presentation aspect changes.
void render_overlay(std::vector<std::uint32_t> &out, int w, int h);

// Draws only the bottom-left "OPCOES [Square]" hint (transparent elsewhere).
// Shown in the Master Mode hub while the recomp menu is closed.
void render_hint(std::vector<std::uint32_t> &out, int w, int h, bool mode_select);

} // namespace ps2::menu
