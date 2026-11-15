#include "ps2/system.hpp"

#ifdef PS2_WINDOW
#include "window.hpp"
#endif
#ifdef PS2_PROFILE
#include "profile.hpp"
#endif
#include "menu/game_settings.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

void register_generated_code();
void register_generated_vu();

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: ps2run <elf> [--arg text]... [--frames n] [--ntsc] [--misses file]\n"
                 "              [--window-size WxH] [--disc image.iso] [--memcard card.ps2] [--pal | --ntsc] [--trace-syscalls] [--trace-io] [--trace-threads]\n"
                 "              [--trace-iop] [--trace-iop-hle] [--frames-dir dir] [--frame-every n]\n"
                 "              [--press frame:button[+button][:frames]]... [--window] [--fullscreen]\n"
                 "              [--save-state frame:file] [--load-state file] [--widescreen] [--progressive] [--fps n|unlocked] [--hz n] [--filter nearest|bilinear] [--interpolate] [--patches file]\n");
    return 2;
}

}

bool parse_resolution(const std::string &v) {
    std::string t;
    for (char c : v) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t == "480p") { ps2::g_options.out_width = 854; ps2::g_options.out_height = 480; return true; }
    if (t == "720p" || t == "hd") { ps2::g_options.out_width = 1280; ps2::g_options.out_height = 720; return true; }
    if (t == "900p") { ps2::g_options.out_width = 1600; ps2::g_options.out_height = 900; return true; }
    if (t == "1080p" || t == "fhd") { ps2::g_options.out_width = 1920; ps2::g_options.out_height = 1080; return true; }
    if (t == "1440p" || t == "2k" || t == "qhd") { ps2::g_options.out_width = 2560; ps2::g_options.out_height = 1440; return true; }
    if (t == "4k" || t == "2160p" || t == "uhd") { ps2::g_options.out_width = 3840; ps2::g_options.out_height = 2160; return true; }
    unsigned w = 0, h = 0;
    if (std::sscanf(v.c_str(), "%ux%u", &w, &h) == 2 && w && h) {
        ps2::g_options.out_width = static_cast<int>(w);
        ps2::g_options.out_height = static_cast<int>(h);
        return true;
    }
    std::fprintf(stderr, "bad --resolution %s (use 720p, 1080p, 4k or WxH)\n", v.c_str());
    return false;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    // The recomp menu persists every value in game_settings.ini. Load it first
    // so explicit command line options still win (they are parsed below).
    ps2::menu::settings_load();
    std::vector<std::string> args{argv[1]};
    [[maybe_unused]] bool window = false, fullscreen = false, borderless = false;
    [[maybe_unused]] bool fullscreen_explicit = false;
    [[maybe_unused]] bool resolution_cli = false;
    // `--gs-dump <file>` replays a PCSX2 GS dump into the GS (no game boot).
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--gs-dump" && i + 1 < argc) ps2::g_options.gs_dump_path = argv[++i];
        else if (a == "--frames-dir" && i + 1 < argc) ps2::g_options.frames_dir = argv[++i];
        else if (a == "--resolution" && i + 1 < argc) {
            if (!parse_resolution(argv[++i])) return usage();
            resolution_cli = true;
        }
    }
    if (!ps2::g_options.gs_dump_path.empty()) {
        if (ps2::g_options.frames_dir.empty()) std::fprintf(stderr, "[gsdump] no --frames-dir given; replaying without dumping frames\n");
        return ps2::replay_gs_dump(ps2::g_options.gs_dump_path) ? 0 : 1;
    }
    std::string load_path;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--arg" && i + 1 < argc) args.push_back(argv[++i]);
        else if (a == "--frames" && i + 1 < argc) ps2::g_options.max_frames = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--ntsc" || a == "--pal") {
            ps2::g_options.pal = a == "--pal";
            ps2::g_options.rate_forced = true;
        }
        else if (a == "--misses" && i + 1 < argc) ps2::g_options.misses_path = argv[++i];
        else if (a == "--trace-syscalls") ps2::g_options.trace_syscalls = true;
        else if (a == "--widescreen") ps2::g_options.widescreen = true;
        else if (a == "--progressive") ps2::g_options.progressive = true;
        else if (a == "--fps" && i + 1 < argc) {
            const std::string v = argv[++i];
            ps2::g_options.fps = v == "unlocked" ? -1 : std::atoi(v.c_str());
        }
        else if (a == "--hz" && i + 1 < argc) ps2::g_options.hz = std::atoi(argv[++i]);
        else if (a == "--interpolate") ps2::g_options.interpolate = true;
        else if (a == "--filter" && i + 1 < argc) {
            const std::string v = argv[++i];
            ps2::g_options.filter = v == "bilinear" ? 1 : v == "nearest" ? 0 : -1;
        }
        else if (a == "--patches" && i + 1 < argc) {
            if (!ps2::load_patches(argv[++i])) return usage();
        }
        else if (a == "--trace-io") ps2::g_options.trace_io = true;
        else if (a == "--trace-threads") ps2::g_options.trace_threads = true;
        else if (a == "--disc" && i + 1 < argc) ps2::g_options.disc_path = argv[++i];
        else if (a == "--memcard" && i + 1 < argc) ps2::g_options.memcard_path = argv[++i];
        else if (a == "--trace-iop") ps2::g_options.trace_iop_modules = true;
        else if (a == "--trace-iop-hle") ps2::g_options.trace_iop_hle = true;
        else if (a == "--frames-dir" && i + 1 < argc) ps2::g_options.frames_dir = argv[++i];
        else if (a == "--resolution" && i + 1 < argc) {
            if (!parse_resolution(argv[++i])) return usage();
            resolution_cli = true;
        }
        else if (a == "--window-size" && i + 1 < argc) {
            unsigned w = 0, h = 0;
            if (std::sscanf(argv[++i], "%ux%u", &w, &h) != 2 || !w || !h) return usage();
            ps2::g_options.window_width = static_cast<int>(w);
            ps2::g_options.window_height = static_cast<int>(h);
        }
        else if (a == "--press" && i + 1 < argc) {
            ps2::Press press;
            if (!ps2::parse_press(argv[++i], press)) return usage();
            ps2::g_options.presses.push_back(press);
        }
        else if (a == "--axis" && i + 1 < argc) {
            ps2::AxisPress press;
            if (!ps2::parse_axis(argv[++i], press)) return usage();
            ps2::g_options.axis_presses.push_back(press);
        }
#ifdef PS2_WINDOW
        else if (a == "--window") window = true;
        else if (a == "--fullscreen") { window = fullscreen = true; fullscreen_explicit = true; }
#endif
        else if (a == "--frame-every" && i + 1 < argc) ps2::g_options.frame_every = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--save-state" && i + 1 < argc) {
            const std::string spec = argv[++i];
            const auto colon = spec.find(':');
            if (colon == std::string::npos) return usage();
            ps2::g_options.save_state_frame = std::strtoull(spec.substr(0, colon).c_str(), nullptr, 10);
            ps2::g_options.save_state_path = spec.substr(colon + 1);
        }
        else if (a == "--load-state" && i + 1 < argc) load_path = argv[++i];
        else return usage();
    }

    // Show the values that actually took effect (CLI/env win over the ini),
    // then make sure the editable file exists in the game folder.
    ps2::menu::settings_sync_effective();
#ifdef PS2_WINDOW
    // --resolution on the command line implies fullscreen (a window cannot be
    // larger than the desktop work area, so it could not honour 1440p/4k).
    // Pass --window to keep it windowed anyway. Note: the resolution loaded
    // from the ini must NOT trigger this, or Screen Mode=Windowed would be
    // ignored and every launch would start fullscreen.
    if (!window && resolution_cli && ps2::g_options.out_width > 0 && ps2::g_options.frames_dir.empty()) {
        window = fullscreen = true;
        std::fprintf(stderr, "[window] --resolution implies --fullscreen (pass --window to override)\n");
    }
    // A normal desktop launch (no finite --frames limit, not a frame-dump run)
    // opens a window even when --window is omitted. --frames/--frames-dir are
    // the benchmark/headless paths.
    const bool headless = !ps2::g_options.frames_dir.empty();
    if (!window && !headless && ps2::g_options.max_frames == 0) {
        window = true;
        std::fprintf(stderr, "[window] desktop launch: opening the saved screen mode\n");
    }
#endif
    // The saved Screen Mode decides windowed/fullscreen/borderless; an
    // explicit --fullscreen always wins.
    if (window && !fullscreen_explicit) {
        const std::string &mode = ps2::menu::settings_screen_mode();
        if (mode == "Fullscreen") fullscreen = true;
        else if (mode == "Borderless") { fullscreen = true; borderless = true; }
    }
#ifdef PS2_WINDOW
    if (window && !open_window(fullscreen, borderless)) return 1;
#endif
    // Create the editable file once the real drawable size / screen mode are
    // known (or immediately in headless runs).
    ps2::menu::settings_ensure_file();
    register_generated_code();
    register_generated_vu();
    const auto emulate = [&]() -> int {
        std::uint32_t entry = 0;
        if (!ps2::load_elf(argv[1], entry)) {
            std::fprintf(stderr, "cannot load %s\n", argv[1]);
            return 1;
        }
        ps2::boot(entry, args);
        if (std::getenv("PS2_MENU_DUMP")) { ps2::dump_recomp_menu(); return 0; }
    if (!load_path.empty() && !ps2::load_state(load_path)) return 1;
#ifdef PS2_PROFILE
        const char *profile = std::getenv("PS2_PROFILE");
        const char *profile_from = std::getenv("PS2_PROFILE_FROM");
        if (profile) start_profile(profile_from ? std::strtoull(profile_from, nullptr, 10) : 0);
        const int result = ps2::run();
        if (profile) write_profile(profile);
        return result;
#else
        return ps2::run();
#endif
    };
#ifdef PS2_WINDOW
    if (window) return run_window(emulate);
#endif
    return emulate();
}
