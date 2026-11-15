#pragma once

#include "ps2/cpu.hpp"
#include "ps2/system.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace ps2::state {
class Stream;
}

namespace ps2::gs {

inline constexpr std::uint32_t kMemorySize = 4u << 20;

enum Psm : std::uint32_t {
    PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0A,
    PSMT8 = 0x13, PSMT4 = 0x14, PSMT8H = 0x1B, PSMT4HL = 0x24, PSMT4HH = 0x2C,
    PSMZ32 = 0x30, PSMZ24 = 0x31, PSMZ16 = 0x32, PSMZ16S = 0x3A,
};

struct Vertex {
    std::uint32_t x, y, z;
    std::uint8_t r, g, b, a;
    float q, s, t;
    std::uint32_t u, v;
    std::uint8_t f;
};

// PCSX2-style "Round Sprite" correction. The GS snaps sprite texel coordinates
// to the 1/16 pixel grid; without this, FST sprites (shadows, 2D effects) are
// sampled with slightly wrong UVs and can appear stretched/darkened.
inline void round_sprite_offset(Vertex &a, Vertex &b, std::uint32_t ofx, std::uint32_t ofy, bool linear) {
    const auto axis = [&](std::uint32_t &p0, std::uint32_t &p1, std::uint32_t ofs, std::uint32_t &t0, std::uint32_t &t1) {
        const int x0 = static_cast<int>(p0) - static_cast<int>(ofs);
        const int x1 = static_cast<int>(p1) - static_cast<int>(ofs);
        const int l = x1 - x0;
        if (!l) return;
        if (linear) {
            const int u0 = static_cast<int>(t0), u1 = static_cast<int>(t1);
            const int lu = u1 - u0;
            if (lu > 0 && lu <= l + 32) t1 = static_cast<std::uint32_t>(u1 - 8);
        } else {
            const auto interp = [](float al, std::uint32_t c0, std::uint32_t c1) {
                const float t = (1.0f - al) * static_cast<float>(c0) + al * static_cast<float>(c1);
                return static_cast<std::uint32_t>(static_cast<int>(t)) & ~15u;
            };
            const float a0 = static_cast<float>(((x0 + 15) & ~15) - x0) / static_cast<float>(l);
            const float a1 = static_cast<float>(((x1 - 1) & ~15) - x0) / static_cast<float>(l);
            const std::uint32_t v0 = interp(a0, t0, t1), v1 = interp(a1, t0, t1);
            if (v0 <= v1) { t0 = v0; t1 = v1 + 16u; }
            else { t0 = v0 + 15u; t1 = v1; }
        }
    };
    axis(a.x, b.x, ofx, a.u, b.u);
    axis(a.y, b.y, ofy, a.v, b.v);
}

struct Context {
    std::uint64_t tex0, clamp, tex1, tex2, xyoffset, miptbp1, miptbp2, scissor, alpha, test, fba, frame, zbuf;
};

struct State {
    std::uint64_t prim, prmode, prmodecont, rgbaq, st, uv, fog, texclut, scanmsk, texa, fogcol, dimx, dthe, colclamp, pabe;
    std::uint64_t bitbltbuf, trxpos, trxreg, trxdir;
    Context ctx[2];
    float q;
    Vertex queue[3];
    int count;
    std::uint32_t clut[512];
};

extern State g_state;
extern std::uint32_t g_setup_generation;
std::uint8_t *memory();

void init();
void state(ps2::state::Stream &s);
void reset();
void write(std::uint32_t reg, std::uint64_t value);
void signal(std::uint32_t reg, std::uint64_t value);
void note_transfer(std::uint32_t reg, std::uint64_t value);
void write_packed(std::uint32_t reg, const Reg128 &q);
void write_image(const std::uint8_t *data, std::uint32_t qwords);
void read_image(std::uint8_t *data, std::uint32_t qwords);
void render_read_image(std::uint8_t *data, std::uint32_t qwords);
void render_reset();

std::uint64_t read_privileged(std::uint32_t address);
void write_privileged(std::uint32_t address, std::uint64_t value);
void vblank(bool start);
bool interlaced_odd_field();

std::uint32_t address32(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y);
std::uint32_t read_pixel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y);
void write_pixel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y, std::uint32_t value);

struct DisplayRegs {
    std::uint64_t pmode, smode2, dispfb[2], display[2];
};
DisplayRegs display_regs();

bool display(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
bool display(const DisplayRegs &regs, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
// Frame for hosts that draw it themselves, with interlaced fields blended like present(); may lag a frame.
bool copy_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
bool copy_frame(const DisplayRegs &regs, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
bool attach_window(int kind, void *display, std::uint64_t window, void *surface, const char *selector, std::uint32_t width, std::uint32_t height);
void resize_window(std::uint32_t width, std::uint32_t height);
bool present(int mode = 0);
bool present(const DisplayRegs &regs, int mode = 0);
void start_thread();
void flush_and_wait();
void post_present(int mode = 0);
Rendered render_pending(const std::function<void(const DisplayRegs &, int)> &on_present, double budget_ms);
double gpu_busy_ms();
std::uint64_t primitives_drawn();
void stats();

}
