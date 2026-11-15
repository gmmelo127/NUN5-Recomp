#pragma once

#include "../gs.hpp"

#include <cstdint>
#include <vector>

namespace ps2::gs::gpu {

enum class Mode { off, mirror, primary };

struct DisplaySource {
    std::uint32_t fbp, fbw, psm, dbx, dby, width, height;
    bool halved;  // FFMD: the framebuffer holds a half-height field
    bool bob;     // interlaced frame: present a single field (stride 2)
    std::uint32_t field;
};

struct SurfaceTarget {
    enum Kind { X11, Wayland, Canvas, Windows } kind;
    void *display;
    std::uint64_t window;
    void *surface;
    const char *selector;
    std::uint32_t width, height;
};

Mode mode();
bool init(Mode m);
void draw(const State &s, const Vertex *v, int count);
void image_begin(const State &s);
void image_data(const std::uint8_t *data, std::size_t bytes);
void local_copy(const State &s);
void load_clut(std::uint64_t tex0, std::uint64_t texclut);
void flush();
void texflush();
void wait_idle();
void download(std::uint8_t *memory, std::uint32_t offset = 0, std::uint32_t bytes = kMemorySize);
bool download_async(std::uint8_t *memory, std::uint32_t offset, std::uint32_t bytes, std::uint32_t serial);
bool finish_download(std::uint32_t serial);
void read_state(std::vector<std::uint32_t> &vram, std::uint32_t &scale, std::vector<std::uint32_t> &clut);
void write_state(const std::vector<std::uint32_t> &vram, std::uint32_t scale, const std::vector<std::uint32_t> &clut);
bool display(const DisplaySource &src, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
bool display_async(const DisplaySource &src, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
bool attach_surface(const SurfaceTarget &target);
void resize_surface(std::uint32_t width, std::uint32_t height);
bool has_surface();
bool present(const DisplaySource &src, int mode = 0);
void set_vsync(bool on);
double busy_ms();
std::uint32_t scale();
// True when the Master Mode pause overlay was drawn in the last few frames.
bool pause_overlay_active();
// Effective internal raster size of the last presented frame (source * scale).
std::uint32_t internal_width();
std::uint32_t internal_height();
// Size actually shaded/presented (capped to the configured Resolution when
// the window is maximized larger).
std::uint32_t present_width();
std::uint32_t present_height();
// Changes the internal supersampling scale live, preserving the GS VRAM.
bool set_scale(std::uint32_t scale);
std::uint32_t preferred_scale(std::uint32_t width);
// Selects the bilinear sampling domain. `sub` keeps the supersampled grid
// (sharp, experimental); `native` is the standard hardware bilinear over the
// native texel grid and avoids the hard/blocky look when magnified.
void set_texture_grid(bool sub);
void refresh_hud_scale();
void stats();

}
