#include "dmac.hpp"
#include "gs/gif.hpp"
#include "vif.hpp"
#include "vu_micro.hpp"
#include "gs/gs.hpp"
#include "gs/gpu/gpu.hpp"
#include "menu/recomp_menu.hpp"
#include "gs/queue.hpp"
#include "hw.hpp"
#include "iop/iop.hpp"
#include "iop/sif.hpp"
#include "iop/sio2.hpp"
#include "iop/spu2.hpp"
#include "ipu/ipu.hpp"
#include "ps2/disc.hpp"
#include "kernel.hpp"
#include "ps2/generated.hpp"
#include "ps2/memory.hpp"
#include "ps2/state.hpp"
#include "ps2/system.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace gen { extern float g_time_scale; }

// vblank()/retime() run on every frame; keep the environment switch lookups
// out of them (std::getenv scans the environment and takes a lock on Windows).
namespace {
const char *const g_env_bench = std::getenv("PS2_BENCH");
const char *const g_env_bench_tl = std::getenv("PS2_BENCH_TL");
const char *const g_env_frame_range = std::getenv("PS2_FRAME_RANGE");
const bool g_env_retime_anim = std::getenv("PS2_RETIME_ANIM") != nullptr;
const bool g_env_retime_test = std::getenv("PS2_RETIME_TEST") != nullptr;
const bool g_env_retime_dt = std::getenv("PS2_RETIME_DT") != nullptr;
const char *const g_env_force_wake = std::getenv("PS2_FORCE_WAKE");
const bool g_env_60fps_ntsc = std::getenv("PS2_60FPS_NTSC") != nullptr;
const bool g_env_no_retime = std::getenv("PS2_NO_RETIME") != nullptr;
}


namespace ps2 {

extern "C" unsigned long long ps2_scene_last_frame();
// Default: plain 30 fps NTSC (no retiming). Opt in to the experiments with
// PS2_60FPS (PAL speed) or PS2_60FPS_NTSC (NTSC speed).
bool g_retime_enabled = std::getenv("PS2_60FPS") != nullptr
                     || std::getenv("PS2_60FPS_PAL") != nullptr
                     || std::getenv("PS2_60FPS_NTSC") != nullptr;

Cpu g_cpu;
Options g_options;

std::uint64_t frame_cycles() { return kEeClock / (g_options.pal ? 50u : 60u); }

namespace {

constexpr std::int64_t kMaxSlice = 20000;

struct Event {
    int id;
    std::uint64_t at;
    std::function<void()> action;
};

std::uint64_t g_base = 0;
std::int64_t g_given = 0;
std::vector<Event> g_events;
bool g_halted = false;
std::string g_halt_reason;
std::uint64_t g_frames = 0;
bool g_new_frame = false;
std::uint64_t g_last_prims = 0;
std::uint64_t g_iop_synced = 0;
bool g_state_request = false;
bool g_resumed = false;
// 0 = none, 1 = save, 2 = load. Set by the host UI and consumed by run().
std::atomic<int> g_state_action{0};
std::mutex g_state_action_mutex;
std::string g_state_action_path;

bool g_loop_stats = std::getenv("PS2_LOOP_STATS") != nullptr;
// PS2_PERF keeps the same timers as PS2_LOOP_STATS but prints a single
// summary at exit instead of one line per frame. jogar.bat sets it so every
// last_run.log tells us whether the EE, the IOP or the GPU is the bottleneck.
const bool g_perf_stats = std::getenv("PS2_PERF") != nullptr;
const bool g_timing = g_loop_stats || g_perf_stats;
double g_sum_ev = 0, g_sum_iop = 0, g_sum_sif = 0, g_sum_svc = 0, g_sum_disp = 0, g_sum_idle = 0;
std::uint64_t g_sum_frames = 0;
std::chrono::steady_clock::time_point g_perf_start{};
double g_perf_idle0 = 0;
std::uint64_t g_perf_wait0 = 0;
std::uint64_t g_perf_disc0 = 0, g_perf_discb0 = 0;
std::uint64_t g_perf_vu0 = 0;
std::clock_t g_perf_cpu0 = 0;

// Process CPU time (all threads), in milliseconds. std::clock() is wall time
// on Mingw, so use GetProcessTimes there.
static double process_cpu_ms() {
#ifdef _WIN32
    FILETIME create{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user)) return 0.0;
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return static_cast<double>(k.QuadPart + u.QuadPart) / 10000.0;
#else
    return static_cast<double>(std::clock()) / CLOCKS_PER_SEC * 1000.0;
#endif
}
double g_iop_ms = 0, g_sif_ms = 0, g_svc_ms = 0, g_disp_ms = 0, g_ev_ms = 0, g_idle_ms = 0;
std::uint64_t g_disp_calls = 0;
std::uint64_t g_iop_calls = 0;

std::uint64_t next_event_time() {
    std::uint64_t t = UINT64_MAX;
    for (const Event &e : g_events) t = std::min(t, e.at);
    return t;
}

void run_due_events() {
    sync_iop();
    for (;;) {
        const std::uint64_t t = now();
        auto it = std::min_element(g_events.begin(), g_events.end(), [](const Event &a, const Event &b) { return a.at < b.at; });
        if (it == g_events.end() || it->at > t) return;
        auto action = std::move(it->action);
        g_events.erase(it);
        action();
    }
}

void refill(Cpu &c) {
    const std::uint64_t t = now(), next = next_event_time();
    std::int64_t slice = kMaxSlice;
    if (next != UINT64_MAX) slice = std::clamp<std::int64_t>(static_cast<std::int64_t>(next - std::min(next, t)), 1, kMaxSlice);
    set_budget(c, slice);
}


void dump_frame() {
    std::vector<std::uint32_t> rgba;
    std::uint32_t w = 0, h = 0;
    if (!gs::display(rgba, w, h)) return;
    char name[512];
    std::snprintf(name, sizeof name, "%s/frame_%05llu.ppm", g_options.frames_dir.c_str(), static_cast<unsigned long long>(g_frames));
    std::FILE *f = std::fopen(name, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (std::uint32_t p : rgba) {
        const std::uint8_t px[3] = {static_cast<std::uint8_t>(p), static_cast<std::uint8_t>(p >> 8), static_cast<std::uint8_t>(p >> 16)};
        std::fwrite(px, 1, 3, f);
    }
    std::fclose(f);
}

void apply_presses() {
    if (!g_options.presses.empty()) {
        std::uint16_t mask = 0;
        std::uint8_t stick = 0;
        for (const Press &p : g_options.presses)
            if (g_frames >= p.frame && g_frames < p.frame + p.frames) {
                mask |= p.buttons;
                stick |= p.stick;
            }
        iop::sio2::set_buttons(0, mask);
        iop::sio2::set_axis(0, 2, (stick & 4u) ? 0x00 : (stick & 8u) ? 0xFF : 0x80);
        iop::sio2::set_axis(0, 3, (stick & 1u) ? 0x00 : (stick & 2u) ? 0xFF : 0x80);
    }
    if (!g_options.axis_presses.empty()) {
        // Outside a hold the stick rests; within one it takes the given tilt.
        std::uint8_t axes[4] = {0x80, 0x80, 0x80, 0x80};
        for (const AxisPress &p : g_options.axis_presses)
            if (g_frames >= p.frame && g_frames < p.frame + p.frames) axes[p.axis] = p.value;
        for (int a = 0; a < 4; ++a) iop::sio2::set_axis(0, a, axes[a]);
    }
}

void time_frames() {
    static const char *spec = std::getenv("PS2_TIME");
    if (!spec) return;
    static const std::uint64_t from = std::strtoull(spec, nullptr, 10);
    static const std::uint64_t count = std::strchr(spec, ',') ? std::strtoull(std::strchr(spec, ',') + 1, nullptr, 10) : 500;
    static std::chrono::steady_clock::time_point start;
    if (g_frames == from) {
        gs::flush_and_wait();
        start = std::chrono::steady_clock::now();
    } else if (g_frames == from + count) {
        gs::flush_and_wait();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::fprintf(stderr, "[time] frames %llu-%llu: %.2f ms per frame\n", static_cast<unsigned long long>(from), static_cast<unsigned long long>(from + count), ms / static_cast<double>(count));
    }
}

void apply_fov_impl() {
// Optional aspect-driven FOV. The widescreen cheat is a static value for
// one aspect; PS2_FOV_ASPECT (or PS2_PRESENT_AR) recomputes the X scale for
// the current aspect and writes it after the file patches, so it wins.
const char *fov_env = std::getenv("PS2_FOV_ASPECT");
if (!fov_env) fov_env = std::getenv("PS2_PRESENT_AR");
if (menu_screen_native_aspect()) fov_env = "4:3";
if (fov_env && std::strcmp(fov_env, "fit") != 0) {
    float a = 0.0f, b = 0.0f, aspect = 0.0f;
    if (std::sscanf(fov_env, "%f:%f", &a, &b) == 2 && a > 0.0f && b > 0.0f) aspect = a / b;
    else {
        const float v = std::atof(fov_env);
        if (v > 0.5f && v < 8.0f) aspect = v;
    }
    float xscale = 0.0f;
    if (const char *fs = std::getenv("PS2_FOV_SCALE")) xscale = static_cast<float>(std::atof(fs));
    else if (aspect > 0.5f) xscale = (4.0f / 3.0f) / aspect;
    if (xscale > 0.3f && xscale < 1.5f) {
        const std::uint32_t bits = [&] {
            std::uint32_t v;
            std::memcpy(&v, &xscale, 4);
            return v;
        }();
        st32(0x00B3EF14u, bits);
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr, "[fov] aspect %s -> xscale %.6f (0x%08X)\n", fov_env, xscale, bits);
        }
    }
}
}

void apply_patches(bool boot) {
    static const bool log = std::getenv("PS2_PATCH_LOG") != nullptr;
    static std::uint64_t applies[2] = {0, 0};
    if (!g_options.patches.empty()) {
        const std::uint64_t n = ++applies[boot ? 1 : 0];
        if (log && (n <= 3 || n % 600 == 0)) {
            std::fprintf(stderr, "[patch] apply boot=%d #%llu (%zu patch%s)\n", boot ? 1 : 0,
                         static_cast<unsigned long long>(n), g_options.patches.size(),
                         g_options.patches.size() == 1 ? "" : "es");
        }
        for (const Patch &p : g_options.patches) {
            if (boot ? p.place == 1 : p.place == 0) continue;
            switch (p.bytes) {
            case 1: st8(p.address, static_cast<std::uint8_t>(p.value)); break;
            case 2: st16(p.address, static_cast<std::uint16_t>(p.value)); break;
            case 4: st32(p.address, static_cast<std::uint32_t>(p.value)); break;
            default: st64(p.address, p.value); break;
            }
            if (log && (n <= 3 || n % 600 == 0)) {
                std::fprintf(stderr, "[patch]   place %u addr %08X bytes %u value %08X -> memory %08X\n",
                             p.place, p.address, p.bytes, static_cast<std::uint32_t>(p.value),
                             ld32(p.address & 0x0FFFFFFFu));
            }
        }
    }
    apply_fov_impl();
}

void dump_ram_series() {
    static bool init = false;
    static std::string dir, hash_path;
    static std::vector<unsigned long long> frames;
    if (!init) {
        init = true;
        if (const char *d = std::getenv("PS2_RAM_SERIES")) dir = d;
        if (const char *h = std::getenv("PS2_RAM_HASH")) hash_path = h;
        if (const char *f = std::getenv("PS2_RAM_FRAMES")) {
            const std::string list = f;
            std::size_t at = 0;
            while (at <= list.size()) {
                const std::size_t comma = list.find(',', at);
                const std::string item = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                if (!item.empty()) frames.push_back(std::strtoull(item.c_str(), nullptr, 10));
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
        }
    }
    if (dir.empty() && hash_path.empty()) return;
    if (std::find(frames.begin(), frames.end(), g_frames) == frames.end()) return;
    if (!hash_path.empty()) {
        std::uint64_t h = 1469598103934665603ull;
        for (std::uint32_t i = 0; i < kRamSize; ++i) {
            h ^= g_ram[i];
            h *= 1099511628211ull;
        }
        if (std::FILE *f = std::fopen(hash_path.c_str(), "a")) {
            std::fprintf(f, "%llu %016llx\n", static_cast<unsigned long long>(g_frames), static_cast<unsigned long long>(h));
            std::fclose(f);
        }
    }
    if (dir.empty()) return;
    char name[512];
    std::snprintf(name, sizeof name, "%s/ram_%05llu.bin", dir.c_str(), static_cast<unsigned long long>(g_frames));
    if (std::FILE *f = std::fopen(name, "wb")) {
        std::fwrite(g_ram, 1, kRamSize, f);
        std::fclose(f);
    }
}

std::mutex g_present_log_mutex;
std::FILE *g_present_log = nullptr;
bool g_present_log_init = false;
std::chrono::steady_clock::time_point g_present_log_start;
void present_log(char kind, int mode) {
    if (!g_present_log_init) {
        g_present_log_init = true;
        if (const char *path = std::getenv("PS2_PRESENT_LOG")) {
            g_present_log = std::fopen(path, "w");
            if (g_present_log) {
                g_present_log_start = std::chrono::steady_clock::now();
                std::fprintf(g_present_log, "kind,us,mode,frame\n");
            }
        }
    }
    if (!g_present_log) return;
    std::lock_guard<std::mutex> lock(g_present_log_mutex);
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - g_present_log_start).count();
    std::fprintf(g_present_log, "%c,%lld,%d,%llu\n", kind, static_cast<long long>(us), mode,
                 static_cast<unsigned long long>(g_frames));
    std::fflush(g_present_log);
}

void vblank() {
    present_log('V', 0);
    ++g_frames;
    if (g_timing) {
        if (g_loop_stats)
            std::fprintf(stderr, "[loop] frame %llu ev %.2f (iop %.2f sif %.2f calls %llu) svc %.2f disp %.2f calls %llu idle %.2f\n",
                         (unsigned long long)g_frames, g_ev_ms, g_iop_ms, g_sif_ms, (unsigned long long)g_iop_calls, g_svc_ms, g_disp_ms, (unsigned long long)g_disp_calls, g_idle_ms);
        if (!g_sum_frames) { g_perf_start = std::chrono::steady_clock::now(); g_perf_idle0 = vu_thread_idle_ms(); g_perf_wait0 = gs::queue::producer_wait_ns(); g_perf_cpu0 = static_cast<std::clock_t>(process_cpu_ms());
            g_perf_disc0 = disc::read_ns(); g_perf_discb0 = disc::read_bytes(); g_perf_vu0 = vu::aot_misses(); }
        g_sum_ev += g_ev_ms; g_sum_iop += g_iop_ms; g_sum_sif += g_sif_ms;
        g_sum_svc += g_svc_ms; g_sum_disp += g_disp_ms; g_sum_idle += g_idle_ms;
        ++g_sum_frames;
        g_iop_ms = g_sif_ms = g_svc_ms = g_disp_ms = g_ev_ms = g_idle_ms = 0; g_disp_calls = 0;
        g_iop_calls = 0;
    }
    if (const char *b = g_env_bench) {
        static unsigned long long from = 0, to = 0;
        static std::chrono::steady_clock::time_point start;
        static double idle0 = 0.0;
        static bool init = false, started = false;
        if (!init) {
            unsigned long long a = 0, z = 0;
            if (std::sscanf(b, "%llu,%llu", &a, &z) == 2) { from = a; to = z; }
            init = true;
        }
        if (!started && g_frames >= from) { start = std::chrono::steady_clock::now(); idle0 = vu_thread_idle_ms(); started = true; }
        if (g_frames == to) {
            const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            const double idle = vu_thread_idle_ms() - idle0;
            const double n = static_cast<double>(to - from);
            std::fprintf(stderr, "[bench] frames %llu..%llu wall %.1f ms (%.2f ms/frame) VU idle %.1f ms busy %.2f ms/frame\n",
                         from, to, wall, wall / n, idle, (wall - idle) / n);
        }
    }
    if (const char *tl = g_env_bench_tl) {
        static unsigned long long start = 0, step = 100;
        static std::chrono::steady_clock::time_point last;
        static bool init = false;
        if (!init) {
            unsigned long long a = 0, s = 100;
            if (std::sscanf(tl, "%llu,%llu", &a, &s) == 2) { start = a; step = s ? s : 100; }
            last = std::chrono::steady_clock::now();
            init = true;
        }
        if (g_frames >= start && (g_frames - start) % step == 0 && g_frames > start) {
            const auto now = std::chrono::steady_clock::now();
            const double wall = std::chrono::duration<double, std::milli>(now - last).count();
            last = now;
            std::fprintf(stderr, "[tl] frame %llu wall %.1f ms (%.2f ms/frame)\n",
                         static_cast<unsigned long long>(g_frames), wall, wall / static_cast<double>(step));
        }
    }
    dump_ram_series();
    time_frames();
    apply_presses();
    apply_patches(false);
    const std::uint64_t prims = gs::primitives_drawn();
    g_new_frame = prims != g_last_prims;
    g_last_prims = prims;
    if (g_options.on_vblank) g_options.on_vblank();
    iop::sio2::flush_cards();
    if (g_options.trace_iop_modules && g_frames % 25 == 0) std::fprintf(stderr, "[frame] %llu prims %llu\n", static_cast<unsigned long long>(g_frames), static_cast<unsigned long long>(gs::primitives_drawn()));
    if (const char *range = g_env_frame_range) {
        unsigned long long a = 0, z = 0;
        if (std::sscanf(range, "%llu,%llu", &a, &z) == 2 && g_frames >= a && g_frames <= z) dump_frame();
    }
    if (!g_options.frames_dir.empty() && g_options.frame_every && g_frames % g_options.frame_every == 0) dump_frame();
    hw::vblank_start();
    iop::vblank(true);
    schedule(hw::kEventVblankEnd + 100, now() + hblank_cycles() * 22u, [] { iop::vblank(false); });
    kernel::on_vblank_start();
    if (g_env_retime_anim) {
        static const std::uint32_t anim_lui[] = {0x76CEC0u, 0x76CED4u, 0x76CF14u, 0x76CF30u, 0x76CF4Cu};
        for (std::uint32_t a : anim_lui) store<std::uint32_t>(g_ram + a, 0x3C023F00u); // lui $2, 0x3F00 (0.5)
    }
    if (g_env_retime_test) {
        // Experimental movement retiming: halve the Master Mode walk/run speeds.
        store<float>(g_ram + 0x89A0A0u + 32u, 2.35f);
        store<float>(g_ram + 0x89A0A0u + 36u, 15.0f);
        if (g_env_retime_dt) {
            store<float>(g_ram + 0x89A0A0u + 56u, 0.02f);
            store<float>(g_ram + 0x89A0A0u + 60u, 0.01f);
        }
    }
    if (const char *fw = g_env_force_wake) {
        const int id = std::atoi(fw);
        if (id > 0) kernel::force_wake(id);
    }
    const bool ntsc60 = g_env_60fps_ntsc;
    const bool native_pal = g_options.pal;
    gen::g_time_scale = (!g_retime_enabled || native_pal) ? 1.0f : (ntsc60 ? 0.5f : 25.0f / 60.0f);
    const bool retime = g_retime_enabled && !native_pal;
    if (retime) {
        // Only run the double-rate logic while the scene is actually being
        // updated. During loads and scene transitions the engine is not
        // re-entrant and extra updates corrupt its object graph.
        const unsigned long long last = ps2_scene_last_frame();
        const bool active = last != 0 && g_frames - last <= 2;
        if (active) {
            kernel::force_wake(7);
            // There are seven 0x64-byte character parameter slots starting at
            // 0x89A0A0; patch all of them so NPCs and battle characters are
            // retimed too, not only the first one.
            const float scale = gen::g_time_scale;
            for (unsigned k = 0; k < 7; ++k) {
                const std::uint32_t base = 0x89A0A0u + k * 0x64u;
                store<float>(g_ram + base + 32u, 4.7f * scale);
                store<float>(g_ram + base + 36u, 30.0f * scale);
                store<float>(g_ram + base + 56u, 0.04f * scale);
                store<float>(g_ram + base + 60u, 0.02f * scale);
            }
        }
    }

    if (g_options.max_frames && g_frames >= g_options.max_frames) halt("frame limit reached");
    if (g_options.save_state_frame && g_frames == g_options.save_state_frame) g_state_request = true;
    schedule(hw::kEventVblank, now() + frame_cycles(), vblank);
}

void idle(Cpu &c) {
    std::uint64_t next = next_event_time();
    if (next == UINT64_MAX && iop::idle()) {
        halt("deadlock: every thread waits and no event is pending");
        return;
    }
    next = std::min(next, now() + kEeClock / 2000);
    g_base = std::max(now(), next);
    g_given = 0;
    c.budget = 0;
    run_due_events();
}

}

void save_context(const Cpu &c, ThreadContext &t) {
    std::memcpy(t.r, c.r, sizeof t.r);
    t.hi = c.hi;
    t.lo = c.lo;
    t.sa = c.sa;
    t.pc = c.pc & ~kUnwind;
    std::memcpy(t.f, c.f, sizeof t.f);
    t.facc = c.facc;
    t.fcr31 = c.fcr31;
}

void load_context(Cpu &c, const ThreadContext &t) {
    std::memcpy(c.r, t.r, sizeof t.r);
    c.hi = t.hi;
    c.lo = t.lo;
    c.sa = t.sa;
    c.pc = t.pc;
    std::memcpy(c.f, t.f, sizeof t.f);
    c.facc = t.facc;
    c.fcr31 = t.fcr31;
}

std::uint64_t now() { return g_base + static_cast<std::uint64_t>(g_given - g_cpu.budget); }

std::uint64_t hblank_cycles() { return frame_cycles() * 2u / (g_options.pal ? 625u : 525u); }

void set_budget(Cpu &c, std::int64_t cycles) {
    g_base = now();
    g_given = cycles;
    c.budget = static_cast<std::int32_t>(cycles);
}

void schedule(int event, std::uint64_t at, std::function<void()> action) {
    g_events.erase(std::remove_if(g_events.begin(), g_events.end(), [&](const Event &e) { return e.id == event; }), g_events.end());
    g_events.push_back({event, at, std::move(action)});
}

std::uint64_t scheduled_at(int event) {
    for (const Event &e : g_events)
        if (e.id == event) return e.at;
    return UINT64_MAX;
}

bool preempt(Cpu &c) {
    run_due_events();
    refill(c);
    return g_halted || g_state_request || kernel::must_unwind();
}

void halt(const std::string &reason) {
    if (!g_halted) std::fprintf(stderr, "[ee] halted: %s\n", reason.c_str());
    g_halted = true;
    g_halt_reason = reason;
}

const char *retime_label() {
    if (g_options.pal) return "PAL-native";
    if (!g_retime_enabled) return "30";
    if (g_env_60fps_ntsc) return "NTSC60";
    return "PAL60";
}

bool halted() { return g_halted; }
const std::string &halt_reason() { return g_halt_reason; }

void apply_fov() { apply_fov_impl(); }

namespace {

constexpr char kStateMagic[8] = {'P', 'S', '2', 'S', 'T', 'A', 'T', 'E'};
constexpr std::uint32_t kStateVersion = 3;

std::function<void()> event_action(int id, std::uint64_t at) {
    if (id == hw::kEventVblank) return vblank;
    if (id == hw::kEventVblankEnd + 100) return [] { iop::vblank(false); };
    if (id >= hw::kEventCount) return kernel::alarm_action(id - hw::kEventCount);
    return hw::event_action(id, at);
}

std::atomic<std::uint32_t> g_vu_fence{0};

void sync_vu_thread() {
    if (!gs::queue::vu_active()) return;
    const std::uint32_t seen = g_vu_fence.load();
    gs::queue::vu().post(gs::queue::kVuFence, 0, 0, nullptr, 0);
    while (g_vu_fence.load() == seen) gs::queue::wait(g_vu_fence, seen);
}

void machine_state(state::Stream &s) {
    char magic[8];
    std::memcpy(magic, kStateMagic, sizeof magic);
    s.bytes(magic, sizeof magic);
    std::uint32_t version = kStateVersion;
    s.pod(version);
    if (std::memcmp(magic, kStateMagic, sizeof magic) != 0) throw std::runtime_error("not a state file");
    if (version != kStateVersion && !(s.loading() && version == 2)) throw std::runtime_error("state file from another version");
    s.set_version(version);
    s.tag("ee");
    s.pod(g_cpu);
    s.bytes(g_ram, kRamSize);
    s.bytes(g_spr, kScratchpadSize);
    s.pod(g_base);
    s.pod(g_given);
    s.pod(g_frames);
    s.pod(g_iop_synced);
    s.pod(g_options.pal);
    s.vec(g_events, [&s](Event &e) {
        s.pod(e.id);
        s.pod(e.at);
        if (s.loading()) e.action = event_action(e.id, e.at);
    });
    kernel::state(s);
    hw::state(s);
    dmac::state(s);
    sync_vu_thread();
    vif::state(s);
    vu::state(s);
    ipu::state(s);
    gs::state(s);
    gif::state(s);
    iop::state(s);
    sif::state(s);
    s.tag("end");
}

}

bool save_state(const std::string &path) {
    try {
        state::Writer w;
        machine_state(w);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(w.out.data()), static_cast<std::streamsize>(w.out.size()));
        if (!out) throw std::runtime_error("cannot write " + path);
        std::fprintf(stderr, "[state] saved frame %llu to %s (%zu KB)\n", static_cast<unsigned long long>(g_frames), path.c_str(), w.out.size() >> 10);
        return true;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[state] cannot save: %s\n", e.what());
        return false;
    }
}

bool load_state(const std::uint8_t *data, std::size_t size) {
    try {
        state::Reader r(data, size);
        machine_state(r);
        g_resumed = true;
        std::fprintf(stderr, "[state] loaded frame %llu\n", static_cast<unsigned long long>(g_frames));
        // Opt-in: the HUD lives in the saved framebuffer and is not redrawn by
        // the game every frame. A quick START/START toggles the Master Mode
        // menu, which makes the game redraw the HUD at the current aspect and
        // internal scale (needed after changing PS2_PRESENT_AR or resolution).
        if (const char *refresh = std::getenv("PS2_HUD_REFRESH")) {
            unsigned long long a = 30, b = 90;
            if (std::sscanf(refresh, "%llu,%llu", &a, &b) != 2) { a = 30; b = 90; }
            const std::uint64_t f = g_frames + a;
            g_options.presses.push_back(Press{f, 4, static_cast<std::uint16_t>(1u << 3), 0});
            g_options.presses.push_back(Press{g_frames + b, 4, static_cast<std::uint16_t>(1u << 3), 0});
            std::fprintf(stderr, "[hud] refresh: START at %llu and %llu\n",
                         static_cast<unsigned long long>(f), static_cast<unsigned long long>(g_frames + b));
        }
        return true;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[state] cannot load: %s\n", e.what());
        return false;
    }
}

bool load_state(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "[state] cannot open %s\n", path.c_str());
        return false;
    }
    const std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(in), {}};
    return load_state(data.data(), data.size());
}

void request_save_state(const std::string &path) {
    {
        std::lock_guard<std::mutex> lock(g_state_action_mutex);
        g_state_action_path = path;
    }
    g_state_action.store(1);
}

void request_load_state(const std::string &path) {
    {
        std::lock_guard<std::mutex> lock(g_state_action_mutex);
        g_state_action_path = path;
    }
    g_state_action.store(2);
}

std::uint64_t frame_count() { return g_frames; }

bool load_elf(const std::string &path, std::uint32_t &entry) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    const std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(file), {}};
    if (data.size() < 52 || std::memcmp(data.data(), "\x7f" "ELF", 4) != 0) return false;
    const auto u32 = [&](std::size_t o) { std::uint32_t v; std::memcpy(&v, data.data() + o, 4); return v; };
    const auto u16 = [&](std::size_t o) { std::uint16_t v; std::memcpy(&v, data.data() + o, 2); return v; };
    entry = u32(24);
    const std::uint32_t phoff = u32(28);
    for (std::uint32_t i = 0; i < u16(44); ++i) {
        const std::size_t ph = phoff + i * u16(42);
        if (u32(ph) != 1) continue;
        const std::uint32_t offset = u32(ph + 4), vaddr = physical(u32(ph + 8)), filesz = u32(ph + 16), memsz = u32(ph + 20);
        if (vaddr + memsz > kRamSize || offset + filesz > data.size()) return false;
        std::memcpy(g_ram + vaddr, data.data() + offset, filesz);
        watch_span(vaddr, filesz, "boot:load_exec");
        std::memset(g_ram + vaddr + filesz, 0, memsz - filesz);
    }
    return true;
}

void sync_iop() {
    const std::uint64_t t = now();
    if (t > g_iop_synced) {
        if (g_timing) {
            const auto a = std::chrono::steady_clock::now();
            iop::run(t - g_iop_synced);
            g_iop_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
            ++g_iop_calls;
        } else {
            iop::run(t - g_iop_synced);
        }
        g_iop_synced = t;
    }
    if (g_timing) {
        const auto a = std::chrono::steady_clock::now();
        sif::pump();
        g_sif_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
    } else {
        sif::pump();
    }
}

void request_iop_sync() {
    if (g_cpu.budget > 64) set_budget(g_cpu, 64);
}

void boot(std::uint32_t entry, const std::vector<std::string> &args) {
    init_watch();
    hw::reset();
    kernel::reset();
    if (!g_options.disc_path.empty() && !disc::open(g_options.disc_path))
        std::fprintf(stderr, "[ee] cannot open disc image %s\n", g_options.disc_path.c_str());
    iop::set_trace(g_options.trace_iop_modules, g_options.trace_iop_hle);
    iop::reset();
    std::vector<std::string> guest_args = args;
    if (!guest_args.empty() && guest_args[0].find(':') == std::string::npos) {
        std::string name = guest_args[0].substr(guest_args[0].find_last_of("/\\") + 1);
        for (char &ch : name) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        guest_args[0] = "cdrom0:\\" + name + ";1";
    }
    std::uint32_t argv = 0x00000800u, payload = argv + 4u * static_cast<std::uint32_t>(guest_args.size() + 1);
    for (std::size_t i = 0; i < guest_args.size(); ++i) {
        store<std::uint32_t>(g_ram + argv + 4 * i, payload);
        std::memcpy(g_ram + payload, guest_args[i].c_str(), guest_args[i].size() + 1);
        payload += static_cast<std::uint32_t>(guest_args[i].size() + 1);
    }
    store<std::uint32_t>(g_ram + argv + 4 * guest_args.size(), 0);
    kernel::start_main_thread(entry, 0, static_cast<std::uint32_t>(guest_args.size()), argv);
    apply_patches(true);
    schedule(hw::kEventVblank, frame_cycles(), vblank);
}

int run() {
    Cpu &c = g_cpu;
    {
        const char *ntsc = std::getenv("PS2_60FPS_NTSC");
        gen::g_time_scale = g_retime_enabled ? (ntsc ? 0.5f : 25.0f / 60.0f) : 1.0f;
        std::fprintf(stderr, "[retime] mode %s (scale %.4f) PS2_60FPS=%s PS2_60FPS_NTSC=%s\n",
                     retime_label(), gen::g_time_scale,
                     std::getenv("PS2_60FPS") ? "1" : "(unset)", ntsc ? ntsc : "(unset)");
    }
    if (!g_resumed) set_budget(c, kMaxSlice);
    while (!g_halted) {
        if (const int action = g_state_action.exchange(0)) {
            std::string path;
            {
                std::lock_guard<std::mutex> lock(g_state_action_mutex);
                path = g_state_action_path;
            }
            if (action == 1) save_state(path);
            else if (action == 2) load_state(path);
        }
        if (g_state_request) {
            g_state_request = false;
            save_state(g_options.save_state_path);
        }
        c.pc &= ~kUnwind;
        const auto la = g_timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        run_due_events();
        const auto lb = g_timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        kernel::service(c);
        const auto lc = g_timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (g_timing) {
            g_ev_ms += std::chrono::duration<double, std::milli>(lb - la).count();
            g_svc_ms += std::chrono::duration<double, std::milli>(lc - lb).count();
        }
        if (g_halted) break;
        if (c.pc == kIdle) {
            const auto ia = g_timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            idle(c);
            if (g_timing) g_idle_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ia).count();
            continue;
        }
        if (c.budget <= 0) refill(c);
        dispatch(c, 0, 0);
        if (g_timing) ++g_disp_calls;
        if (g_timing) g_disp_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lc).count();
    }
    kernel::dump();
    iop::dump();
    sif::dump();
    if (const char *peek = std::getenv("PS2_PEEK")) {
        std::uint32_t at = 0, len = 0;
        if (std::sscanf(peek, "%x,%x", &at, &len) == 2)
            for (std::uint32_t i = 0; i < len; i += 16) {
                std::fprintf(stderr, "[peek] %08X:", at + i);
                for (std::uint32_t j = 0; j < 16 && i + j < len; j += 4) std::fprintf(stderr, " %08X", ld32(at + i + j));
                std::fprintf(stderr, "\n");
            }
    }
    if (const char *path = std::getenv("PS2_DUMP")) {
        if (std::FILE *f = std::fopen(path, "wb")) {
            std::fwrite(g_ram, 1, kRamSize, f);
            std::fwrite(iop::ram_ptr(0), 1, iop::kRamSize, f);
            std::fwrite(iop::spu2::ram(), 2, 1u << 20, f);
            std::fclose(f);
        }
    }
    gs::stats();
    ipu::stats();
    if (const char *path = std::getenv("PS2_DUMP_AUDIO")) iop::spu2::dump_audio(path);
    if (std::getenv("PS2_TRACE_SPU2")) iop::spu2::dump_voices();
    dmac::stats();
    gif::stats();
    vif::stats();
    std::fprintf(stderr, "[ee] stopped after %llu frames, %llu cycles: %s\n",
                 static_cast<unsigned long long>(g_frames), static_cast<unsigned long long>(now()), g_halt_reason.c_str());
    if (g_timing && g_sum_frames) {
        const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_perf_start).count();
        const double n = static_cast<double>(g_sum_frames);
        const double vu_idle = vu_thread_idle_ms() - g_perf_idle0;
        const double gs_wait = (gs::queue::producer_wait_ns() - g_perf_wait0) / 1.0e6 / n;
        const double cpu = (process_cpu_ms() - static_cast<double>(g_perf_cpu0)) / n;
        const double disc_ms = static_cast<double>(disc::read_ns() - g_perf_disc0) / 1.0e6 / n;
        const double disc_mb = static_cast<double>(disc::read_bytes() - g_perf_discb0) / 1048576.0 / n;
        std::fprintf(stderr,
                     "[perf] %llu frames, %.2f ms/frame | cpu %.2f | ee %.2f | gs-wait %.2f | disc %.2fms/%.1fMB | iop %.2f | sif %.2f | svc %.2f | idle %.2f | vu busy %.1f%% aot %llu/%llu\n",
                     (unsigned long long)g_sum_frames, wall / n, cpu, g_sum_disp / n, gs_wait, disc_ms, disc_mb, g_sum_iop / n, g_sum_sif / n, g_sum_svc / n,
                     g_sum_idle / n, wall > 0 ? (wall - vu_idle) / wall * 100.0 : 0.0,
                     (unsigned long long)vu::aot_hits(), (unsigned long long)vu::aot_misses());
    }
    return 0;
}

bool display_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) { return gs::display(rgba, width, height); }
bool attach_window(const WindowTarget &t) { return gs::attach_window(static_cast<int>(t.kind), t.display, t.window, t.surface, t.selector, t.width, t.height); }
void resize_window(std::uint32_t width, std::uint32_t height) { gs::resize_window(width, height); }
bool present_frame(int mode) { return gs::present(mode); }
void init_graphics() { gs::init(); }
void start_render_thread() { gs::start_thread(); }
void start_vu_thread() { gs::queue::vu().start(32u << 20); }

std::atomic<double> g_vu_idle_ms{0.0};

double vu_thread_idle_ms() { return g_vu_idle_ms.load(); }
std::uint64_t vu1_interpreted_calls() { return vu::interpreted_calls(); }

void run_vu_thread() {
    gs::queue::role() = gs::queue::Role::Vu;
    gs::queue::Ring &in = gs::queue::vu();
    for (;;) {
        gs::queue::Header h;
        const std::uint8_t *payload = nullptr;
        while (in.peek(h, payload)) {
            switch (h.type) {
            case gs::queue::kVif1: vif::transfer(1, payload, h.arg0); break;
            case gs::queue::kVif1Write: vif::write(1, h.arg0, h.arg1); break;
            case gs::queue::kVuReset:
                vif::reset_vu_side();
                vu::reset();
                break;
            case gs::queue::kVuFence:
                g_vu_fence.fetch_add(1);
                gs::queue::notify(g_vu_fence);
                break;
            default: gs::queue::render().post(static_cast<gs::queue::Type>(h.type), h.arg0, h.arg1, payload, h.bytes); break;
            }
            in.pop(h);
        }
        const auto start = std::chrono::steady_clock::now();
        in.wait_for_work(50);
        g_vu_idle_ms.store(g_vu_idle_ms.load() + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
}
void present_log_mark(char kind) { present_log(kind, 0); }

void post_present(int mode) {
    present_log('E', mode);
    gs::post_present(mode);
}
bool game_frame_new() { return g_new_frame; }

Rendered render_pending(bool surface, const std::function<void(const std::uint32_t *, std::uint32_t, std::uint32_t)> &copy,
                        const std::function<void()> &before_present, double budget_ms) {
    static std::vector<std::uint32_t> frame;
    return gs::render_pending([&](const gs::DisplayRegs &regs, int mode) {
        if (before_present) before_present();
        if (surface) {
            present_log('P', mode);
            gs::present(regs, mode);
            present_log('Q', mode);
            return;
        }
        std::uint32_t width = 0, height = 0;
        if (copy && gs::copy_frame(regs, frame, width, height)) copy(frame.data(), width, height);
    }, budget_ms);
}

void wait_render_work(int timeout_ms) { gs::queue::render().wait_for_work(timeout_ms); }
void wake_render_thread() { gs::queue::render().wake(); }
bool copy_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) { return gs::copy_frame(rgba, width, height); }

bool parse_press(const std::string &spec, Press &press) {
    static const char *const kNames[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                           "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
    const auto colon = spec.find(':');
    if (colon == std::string::npos) return false;
    press.frame = std::strtoull(spec.c_str(), nullptr, 10);
    std::string rest = spec.substr(colon + 1);
    press.frames = 6;
    const auto second = rest.find(':');
    if (second != std::string::npos) {
        press.frames = static_cast<std::uint32_t>(std::strtoul(rest.c_str() + second + 1, nullptr, 10));
        rest = rest.substr(0, second);
    }
    press.buttons = 0;
    press.stick = 0;
    static const char *const kStick[4] = {"lup", "ldown", "lleft", "lright"};
    std::size_t start = 0;
    while (start <= rest.size()) {
        const auto plus = rest.find('+', start);
        const std::string name = rest.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        int bit = -1;
        for (int i = 0; i < 16; ++i)
            if (name == kNames[i]) bit = i;
        int axis = -1;
        for (int i = 0; i < 4; ++i)
            if (name == kStick[i]) axis = i;
        if (bit < 0 && axis < 0) return false;
        if (bit >= 0) press.buttons |= static_cast<std::uint16_t>(1u << bit);
        else press.stick |= static_cast<std::uint8_t>(1u << axis);
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return true;
}

bool load_patches(const std::string &path) {
    std::ifstream in(path);
    if (!in) return false;
    const bool log = std::getenv("PS2_PATCH_LOG") != nullptr;
    const auto add = [&](unsigned place, unsigned bytes, unsigned long address, unsigned long long value) {
        g_options.patches.push_back({static_cast<std::uint8_t>(place), static_cast<std::uint8_t>(bytes),
                                     static_cast<std::uint32_t>(address), value});
        if (log)
            std::fprintf(stderr, "[patch] loaded place %u addr %08lX bytes %u value %08llX\n",
                         place, address & 0x0FFFFFFFu, bytes, value & 0xFFFFFFFFull);
    };
    std::string line;
    while (std::getline(in, line)) {
        std::size_t a = 0, z = line.size();
        while (a < z && std::isspace(static_cast<unsigned char>(line[a]))) ++a;
        while (z > a && std::isspace(static_cast<unsigned char>(line[z - 1]))) --z;
        std::string text = line.substr(a, z - a);
        if (text.empty() || text[0] == '#' || text[0] == ';' || text[0] == '[')
            continue;
        // Drop trailing comments (both pnach/cheat styles use // and #).
        const std::size_t slash = text.find("//");
        if (slash != std::string::npos) text.resize(slash);
        const std::size_t hash = text.find('#');
        if (hash != std::string::npos) text.resize(hash);
        z = text.size();
        while (z && std::isspace(static_cast<unsigned char>(text[z - 1]))) --z;
        text.resize(z);
        if (text.empty()) continue;
        // PCSX2 pnach: patch=<place>,EE,<address>,<type>,<value>
        if (text.rfind("patch=", 0) == 0) {
            char place[16] = {}, cpu[8] = {}, addr[24] = {}, type[16] = {}, value[32] = {};
            if (std::sscanf(text.c_str() + 6, "%15[^,],%7[^,],%23[^,],%15[^,],%31[^,]",
                            place, cpu, addr, type, value) != 5)
                continue;
            if (std::strcmp(cpu, "EE") != 0) continue;
            unsigned long address = std::strtoul(addr, nullptr, 16);
            unsigned long long data = std::strtoull(value, nullptr, 16);
            unsigned bytes = 0;
            if (std::strcmp(type, "byte") == 0) bytes = 1;
            else if (std::strcmp(type, "short") == 0) bytes = 2;
            else if (std::strcmp(type, "word") == 0) bytes = 4;
            else if (std::strcmp(type, "double") == 0) bytes = 8;
            else if (std::strcmp(type, "extended") == 0) {
                // The leading nibble of the address carries the width; the rest
                // is the physical EE address (0x2aaaaaaa -> 32-bit write).
                const unsigned width = static_cast<unsigned>(address >> 28);
                if (width > 3) continue;
                bytes = 1u << width;
                address &= 0x0FFFFFFFu;
            } else continue;
            const unsigned place_n = static_cast<unsigned>(std::strtoul(place, nullptr, 10));
            if (place_n == 2) { add(0, bytes, address, data); add(1, bytes, address, data); }
            else add(place_n == 0 ? 0u : 1u, bytes, address, data);
            continue;
        }
        // PCSX2 cheat lines: "<8 hex digits> <value>". Only the data-write
        // forms (leading nibble 0..3) are used; mastercodes/conditionals are
        // skipped. These are applied continuously so a later game store does
        // not undo them.
        {
            char t1[16] = {}, t2[32] = {};
            const int n = std::sscanf(text.c_str(), "%15s %31s", t1, t2);
            const auto hexlen = [](const char *t) {
                std::size_t k = 0;
                while (t[k] && std::isxdigit(static_cast<unsigned char>(t[k]))) ++k;
                return k;
            };
            if (n >= 1 && hexlen(t1) == 8 && t1[8] == 0) {
                // A cheat line. Mastercodes/conditionals (leading nibble 8 and
                // up) are not data writes, so they are ignored here.
                if (n == 2 && hexlen(t2) >= 1 && t2[hexlen(t2)] == 0) {
                    const unsigned long address = std::strtoul(t1, nullptr, 16);
                    const unsigned width = static_cast<unsigned>(address >> 28);
                    if (width <= 3)
                        add(1, 1u << width, address & 0x0FFFFFFFu, std::strtoull(t2, nullptr, 16));
                }
                continue;
            }
        }
        // Legacy recomp format: place address bytes value.
        unsigned place = 0, bytes = 0;
        unsigned long address = 0;
        unsigned long long value = 0;
        if (std::sscanf(text.c_str(), "%u %lx %u %llx", &place, &address, &bytes, &value) == 4)
            add(place, bytes, address, value);
    }
    if (log)
        std::fprintf(stderr, "[patch] %s: %zu patch%s\n", path.c_str(), g_options.patches.size(),
                     g_options.patches.size() == 1 ? "" : "es");
    return true;
}

bool parse_axis(const std::string &spec, AxisPress &press) {
    static const char *const kNames[4] = {"rx", "ry", "lx", "ly"};
    std::size_t at = 0;
    const auto field = [&](std::string &out) {
        const auto colon = spec.find(':', at);
        if (colon == std::string::npos) return false;
        out = spec.substr(at, colon - at);
        at = colon + 1;
        return true;
    };
    std::string frame, axis, value;
    if (!field(frame) || !field(axis)) return false;
    const auto tail = spec.find(':', at);
    value = spec.substr(at, tail == std::string::npos ? std::string::npos : tail - at);
    press.frame = std::strtoull(frame.c_str(), nullptr, 10);
    press.axis = -1;
    for (int i = 0; i < 4; ++i)
        if (axis == kNames[i]) press.axis = i;
    if (press.axis < 0) return false;
    press.value = static_cast<std::uint8_t>(std::strtoul(value.c_str(), nullptr, 0));
    press.frames = 6;
    if (tail != std::string::npos) press.frames = static_cast<std::uint32_t>(std::strtoul(spec.c_str() + tail + 1, nullptr, 10));
    return press.frame || press.frames;
}

std::uint32_t internal_scale() { return gs::gpu::scale(); }
std::uint32_t internal_width() { return gs::gpu::internal_width(); }
std::uint32_t internal_height() { return gs::gpu::internal_height(); }
std::uint32_t present_width() { return gs::gpu::present_width(); }
std::uint32_t present_height() { return gs::gpu::present_height(); }

void set_render_scale_for_width(unsigned width) {
    // A configured resolution wins over the legacy PS2_SCALE env.
    if (g_options.out_width == 0 && std::getenv("PS2_SCALE")) return;
    const std::uint32_t scale = gs::gpu::preferred_scale(width);
    if (scale != gs::gpu::scale()) gs::gpu::set_scale(scale);
}

void refresh_hud_scale() { gs::gpu::refresh_hud_scale(); }

void set_vsync(bool on) {
    g_options.vsync = on;
    gs::gpu::set_vsync(on);
}

void set_texture_filter(int filter) {
    g_options.filter = filter;
    // The per-texture native grid is selected inside the GPU draw path from
    // g_options.filter; the global grid stays on the supersampled default.
}

bool mode_select_active() {
    // The top-level Mode Select screen keeps a live mode object and the scene
    // manager reports scene 1 with the transition flag cleared. Other menus
    // (character select, battle) report different values.
    const std::uint32_t object = ld32(0x00617D0Cu);
    if (object == 0) return false;
    if (ld32(object) != 4u) return false;
    const std::uint32_t manager = ld32(0x00617D00u);
    if (manager == 0) return false;
    return ld32(manager + 12u) == 1u && ld32(manager + 16u) == 0u;
}

bool master_mode_menu_active() {
    // After Start the Master Mode hub keeps its mode object alive in state 2
    // and the scene manager reports scene 4 with the transition flag set.
    // State 3/4 is the temporary Quit dialog, where the hint is hidden.
    static bool seen_active = false;
    static bool pause_seen = false;
    static int clear_frames = 0;
    const auto reset = [] {
        seen_active = false;
        pause_seen = false;
        clear_frames = 0;
        return false;
    };
    const std::uint32_t object = ld32(0x00617D0Cu);
    if (object == 0) return reset();
    const std::uint32_t state = ld32(object);
    if (state != 2u) return reset();
    const std::uint32_t manager = ld32(0x00617D00u);
    if (manager == 0) return reset();
    if (ld32(manager + 12u) != 4u || ld32(manager + 16u) != 1u) return reset();
    // The world keeps bit 0x800 set; the Item/Quit/Status pause clears it.
    // A short continuous-clear debounce avoids the brief false clears during
    // transitions/cutscenes that used to flash the OPCOES prompt (and let
    // Square open the menu) while the player was still in the world.
    const bool menu_flag = ld32(0x00618074u) == 1u;
    const bool field_active = (ld32(0x00617EB8u) & 0x800u) != 0u;
    if (field_active) {
        seen_active = true;
        pause_seen = false;
        clear_frames = 0;
    } else if (seen_active && clear_frames < 1000) {
        ++clear_frames;
        // Latch the pause overlay when its Map icon is drawn while the field
        // bit is clear. The world can clear the bit too, but it does not draw
        // the icon in the pause band.
        if (gs::gpu::pause_overlay_active()) pause_seen = true;
    }
    return menu_flag || (seen_active && clear_frames >= 30 && pause_seen);
}

bool recomp_menu_available() { return mode_select_active() || master_mode_menu_active(); }

bool menu_screen_native_aspect() {
    // The game's 2D screens are native 4:3. Scene ids seen in this build:
    // 0 title/loading, 1 Mode Select, 2 battle/gameplay (3D), 3 character
    // select/VS/Practice, 4 Master Mode (3D world + 2D pause overlay).
    const std::uint32_t manager = ld32(0x00617D00u);
    if (manager == 0) return mode_select_active();
    const std::uint32_t scene = ld32(manager + 12u);
    if (scene == 0u || scene == 1u || scene == 3u) return true;
    // Everything else (battle, Master Mode world AND its pause overlay) uses
    // the chosen presentation aspect/FOV. The pause menu must stay 16:9/21:9.
    return false;
}

void dump_recomp_menu() {
    menu::menu().print(stderr);
}

double gpu_busy_ms() { return gs::gpu_busy_ms(); }

#ifdef __EMSCRIPTEN__
EM_ASYNC_JS(void, ps2_browser_yield, (), {
    if (!Module.yieldPort) {
        const channel = new MessageChannel();
        Module.yieldWaiters = [];
        channel.port1.onmessage = () => Module.yieldWaiters.shift()();
        Module.yieldPort = channel.port2;
    }
    await new Promise((resolve) => { Module.yieldWaiters.push(resolve); Module.yieldPort.postMessage(0); });
});
void browser_yield() { ps2_browser_yield(); }
#else
void browser_yield() {}
#endif

std::uint64_t frames_run() { return g_frames; }
std::uint64_t primitives_drawn() { return gs::primitives_drawn(); }
void set_pad_buttons(std::uint16_t pressed, int pad) { iop::sio2::set_buttons(pad, pressed); }
void set_pad_axis(int axis, std::uint8_t value, int pad) { iop::sio2::set_axis(pad, axis, value); }
void set_pad_pressure(int button, std::uint8_t value, int pad) { iop::sio2::set_pressure(pad, button, value); }
void set_pad_connected(int pad, bool connected) { iop::sio2::set_connected(pad, connected); }
std::uint16_t pad_rumble(int pad) { return iop::sio2::rumble(pad); }
void set_audio_sink(std::function<void(const std::int16_t *, std::size_t)> sink) { iop::spu2::set_sink(std::move(sink)); }

}

namespace gen {
// Animation step per logic update: 1.0 at 30 fps, 0.5 at 60 fps (NTSC speed)
// and 25/60 at 60 fps (original PAL speed).
std::uint32_t g_anim_dt_bits = 0x3F800000u;
// The disc is PAL (25 fps): PS2_60FPS defaults to the original PAL speed.
// PS2_60FPS_NTSC selects the 30 fps NTSC speed instead.
// 60 fps at the original PAL speed is the default now. PS2_NO_RETIME restores
// the plain 30 fps NTSC behaviour, PS2_60FPS_NTSC selects 60 fps at NTSC speed.
float g_time_scale = [] {
    if (const char *s = std::getenv("PS2_SPEED")) {
        const float v = std::strtof(s, nullptr);
        if (v > 0.0f && v < 4.0f) return v;
    }
    if (g_env_no_retime) return 1.0f;
    if (g_env_60fps_ntsc) return 0.5f;
    return 25.0f / 60.0f;
}();
}
