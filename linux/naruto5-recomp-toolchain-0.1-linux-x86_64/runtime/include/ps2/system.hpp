#pragma once

#include "cpu.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ps2 {

inline constexpr std::uint64_t kEeClock = 294912000ull;
inline constexpr std::uint32_t kHandlerReturn = 0x00001000u;
inline constexpr std::uint32_t kIdle = 0x00001010u;

extern Cpu g_cpu;

struct Press {
    std::uint64_t frame;
    std::uint32_t frames;
    std::uint16_t buttons;
    std::uint8_t stick = 0;
};

struct Patch {
    std::uint8_t place;
    std::uint8_t bytes;
    std::uint32_t address;
    std::uint64_t value;
};

struct AxisPress {
    std::uint64_t frame;
    std::uint32_t frames;
    int axis;
    std::uint8_t value;
};

struct Options {
    bool trace_syscalls = false;
    bool trace_io = false;
    bool trace_threads = false;
    bool pal = true;
    // Presentation V-Sync (Fifo). Off uses Immediate/Mailbox when available.
    bool vsync = true;
    bool widescreen = false;
    bool progressive = false;
    // 0 = native vblank rate (50/60 from the video mode), -1 = unlocked
    // (run as fast as the host can), >0 = override the emulated rate in Hz.
    int fps = 0;
    // 0 = follow fps/native, >0 = cap the window presentation in Hz.
    int hz = 0;  // 0 = auto (display refresh, minimum 60)
    // -1 = follow the game's TEX1 bilinear bit, 0 = force nearest,
    // 1 = force bilinear.
    int filter = -1;
    // 0 = off, 1 = FXAA (applied in the presentation blit).
    int aa = 0;
    // Reserved quality knobs (saved/editable; the GS has no aniso/draw distance).
    int aniso = 0;
    int draw_distance = 0;
    // Run VU1 microcode on its own host thread (MTVU). Startup only.
    bool mtvu = true;
    // Present an interpolated frame between the game's 30 fps updates.
    bool interpolate = false;
    bool invert_camera = false;
    bool rate_forced = false;
    std::uint64_t max_frames = 0;
    std::string misses_path;
    std::string disc_path;
    bool trace_iop_modules = false;
    bool trace_iop_hle = false;
    std::string frames_dir;
    std::uint32_t frame_every = 60;
    // Replay a PCSX2 GS dump (`.gs`) through the GS instead of booting a game.
    std::string gs_dump_path;
    // Requested output resolution (0 = window default / desktop for fullscreen).
    int out_width = 0;
    int out_height = 0;
    // Optional window size independent of the internal Resolution cap.
    int window_width = 0;
    int window_height = 0;
    std::vector<Press> presses;
    std::vector<AxisPress> axis_presses;
    std::vector<Patch> patches;
    std::function<void()> on_vblank;
    std::function<void(const std::string &name, const std::uint8_t *code, std::size_t size)> on_vu_program;
    std::string memcard_path;
    std::function<void()> on_memcard_saved;
    std::string save_state_path;
    std::uint64_t save_state_frame = 0;
};

extern Options g_options;

struct ThreadContext {
    Reg128 r[32];
    Reg128 hi, lo;
    std::uint32_t sa, pc;
    std::uint32_t f[32];
    std::uint32_t facc, fcr31;
};

void save_context(const Cpu &c, ThreadContext &t);
void load_context(Cpu &c, const ThreadContext &t);

std::uint64_t now();
std::uint64_t frame_cycles();
std::uint64_t hblank_cycles();
void set_budget(Cpu &c, std::int64_t cycles);
void schedule(int event, std::uint64_t at, std::function<void()> action);
std::uint64_t scheduled_at(int event);
bool preempt(Cpu &c);

bool has_code(std::uint32_t pc);
void raise_intc(unsigned cause);
void request_reschedule();
void request_iop_sync();
void sync_iop();

bool load_elf(const std::string &path, std::uint32_t &entry);
void boot(std::uint32_t entry, const std::vector<std::string> &args);
int run();
void halt(const std::string &reason);
bool save_state(const std::string &path);
// Thread-safe requests from the host UI: the actual state I/O runs on the
// emulation thread at the next loop boundary (the machine must not be
// serialised while it is being stepped).
void request_save_state(const std::string &path);
void request_load_state(const std::string &path);
// Recompute the widescreen X FOV from PS2_FOV_ASPECT/PS2_PRESENT_AR.
void apply_fov();
bool load_state(const std::string &path);
// Replay a PCSX2 GS dump (uncompressed `.gs`) into the GS, dumping one PPM per
// vblank into g_options.frames_dir. Returns false on malformed input.
bool replay_gs_dump(const std::string &path);
bool load_state(const std::uint8_t *data, std::size_t size);
bool halted();
const std::string &halt_reason();
std::uint64_t frame_count();
const char *retime_label();

bool display_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);

struct WindowTarget {
    enum Kind { X11, Wayland, Canvas, Windows } kind = X11;
    void *display = nullptr;
    std::uint64_t window = 0;
    void *surface = nullptr;
    const char *selector = nullptr;
    std::uint32_t width = 0, height = 0;
};

// Lets the WebGPU renderer present straight to a window or canvas instead of handing frames back.
bool attach_window(const WindowTarget &target);
void resize_window(std::uint32_t width, std::uint32_t height);
bool present_frame(int mode = 0);
void init_graphics();
void start_render_thread();
void start_vu_thread();
void run_vu_thread();
double vu_thread_idle_ms();
std::uint64_t vu1_interpreted_calls();
void post_present(int mode = 0);
void present_log_mark(char kind);
bool game_frame_new();
enum class Rendered { idle, presented, busy };
Rendered render_pending(bool surface, const std::function<void(const std::uint32_t *rgba, std::uint32_t width, std::uint32_t height)> &copy,
                        const std::function<void()> &before_present, double budget_ms);
void wait_render_work(int timeout_ms);
void wake_render_thread();
bool copy_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
bool parse_press(const std::string &spec, Press &press);
bool parse_axis(const std::string &spec, AxisPress &press);
bool load_patches(const std::string &path);
double gpu_busy_ms();
std::uint32_t internal_scale();
std::uint32_t internal_width();
std::uint32_t internal_height();
// Size actually shaded/presented (capped to the configured Resolution).
std::uint32_t present_width();
std::uint32_t present_height();
// Applies the internal raster scale that matches a configured output width.
void set_render_scale_for_width(unsigned width);
void refresh_hud_scale();
void set_vsync(bool on);
// -1 = Auto, 0 = Nearest, 1 = Bilinear. Bilinear also selects the native
// texel sampling grid so magnified textures stay smooth.
void set_texture_filter(int filter);
// True while the game is on the top-level Mode Select screen (where Square
// opens the recomp settings menu).
bool mode_select_active();
// True on any screen where the recomp menu may open with Square: Mode Select
// and the Master Mode hub/menu.
bool recomp_menu_available();
// True specifically in the Master Mode hub/menu (used for the bottom-left
// "Opcoes [Square]" hint).
bool master_mode_menu_active();
// True on 2D menu screens (Mode Select / Master Mode pause) that should be
// presented at their native 4:3 aspect instead of being stretched to 21:9.
bool menu_screen_native_aspect();
void dump_recomp_menu();
void browser_yield();
std::uint64_t frames_run();
std::uint64_t primitives_drawn();
// pad 0 is the first controller port, pad 1 the second, which starts unplugged
void set_pad_buttons(std::uint16_t pressed, int pad = 0);
void set_pad_axis(int axis, std::uint8_t value, int pad = 0);
void set_pad_pressure(int button, std::uint8_t value, int pad = 0);
void set_pad_connected(int pad, bool connected);
std::uint16_t pad_rumble(int pad = 0);
void set_audio_sink(std::function<void(const std::int16_t *samples, std::size_t frames)> sink);

}
