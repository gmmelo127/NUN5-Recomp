#include "window.hpp"

#include "ps2/system.hpp"
#include "menu/game_settings.hpp"
#include "menu/recomp_menu.hpp"

#include <SDL.h>
#include <SDL_syswm.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

namespace {
#ifdef _WIN32
void env_set(const char *name, const char *value) { _putenv_s(name, value); }
void env_unset(const char *name) { _putenv_s(name, ""); }
#else
void env_set(const char *name, const char *value) { setenv(name, value, 1); }
void env_unset(const char *name) { unsetenv(name); }
#endif
extern "C" unsigned long long ps2_scene_updates();

enum Button : std::uint16_t {
    kSelect = 1u << 0, kL3 = 1u << 1, kR3 = 1u << 2, kStart = 1u << 3,
    kUp = 1u << 4, kRight = 1u << 5, kDown = 1u << 6, kLeft = 1u << 7,
    kL2 = 1u << 8, kR2 = 1u << 9, kL1 = 1u << 10, kR1 = 1u << 11,
    kTriangle = 1u << 12, kCircle = 1u << 13, kCross = 1u << 14, kSquare = 1u << 15,
};

struct KeyBinding {
    SDL_Scancode key;
    std::uint16_t button;
};

constexpr KeyBinding kKeys[] = {
    {SDL_SCANCODE_UP, kUp},         {SDL_SCANCODE_DOWN, kDown},     {SDL_SCANCODE_LEFT, kLeft},
    {SDL_SCANCODE_RIGHT, kRight},   {SDL_SCANCODE_Z, kCross},       {SDL_SCANCODE_X, kCircle},
    {SDL_SCANCODE_A, kSquare},      {SDL_SCANCODE_S, kTriangle},    {SDL_SCANCODE_Q, kL1},
    {SDL_SCANCODE_W, kR1},          {SDL_SCANCODE_1, kL2},          {SDL_SCANCODE_2, kR2},
    {SDL_SCANCODE_RETURN, kStart},  {SDL_SCANCODE_BACKSPACE, kSelect},
};

struct PadBinding {
    SDL_GameControllerButton button;
    std::uint16_t mask;
};

constexpr PadBinding kPadButtons[] = {
    {SDL_CONTROLLER_BUTTON_A, kCross},          {SDL_CONTROLLER_BUTTON_B, kCircle},
    {SDL_CONTROLLER_BUTTON_X, kSquare},         {SDL_CONTROLLER_BUTTON_Y, kTriangle},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, kL1},  {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, kR1},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK, kL3},     {SDL_CONTROLLER_BUTTON_RIGHTSTICK, kR3},
    {SDL_CONTROLLER_BUTTON_BACK, kSelect},      {SDL_CONTROLLER_BUTTON_START, kStart},
    {SDL_CONTROLLER_BUTTON_DPAD_UP, kUp},       {SDL_CONTROLLER_BUTTON_DPAD_DOWN, kDown},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, kLeft},   {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, kRight},
};

SDL_Window *g_window = nullptr;
SDL_Renderer *g_renderer = nullptr;
SDL_Texture *g_texture = nullptr;
SDL_AudioDeviceID g_audio = 0;
// the keyboard drives the first controller port along with the first game controller, a second one is player 2
SDL_GameController *g_pads[2] = {};
std::uint32_t g_tex_w = 0, g_tex_h = 0;
std::uint64_t g_last = 0;
int g_display_hz = 60;
std::uint64_t g_game_frames = 0, g_presents = 0;
std::vector<std::uint32_t> g_pixels;
bool g_gpu_present = false;
bool parse_size(const std::string &v, int &w, int &h) {
    return std::sscanf(v.c_str(), "%dx%d", &w, &h) == 2 && w >= 320 && h >= 200;
}

void apply_screen_mode(const std::string &mode, int w, int h) {
    if (!g_window) return;
    const Uint32 flags = SDL_GetWindowFlags(g_window);
    if (mode == "Windowed") {
        if (flags & SDL_WINDOW_FULLSCREEN) SDL_SetWindowFullscreen(g_window, 0);
        if (flags & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(g_window);
        if (w > 0 && h > 0) SDL_SetWindowSize(g_window, w, h);
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        SDL_ShowCursor(SDL_ENABLE);
    } else if (mode == "Borderless") {
        SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
        SDL_ShowCursor(SDL_DISABLE);
    } else {
        SDL_DisplayMode dm{};
        const int display = SDL_GetWindowDisplayIndex(g_window);
        if (SDL_GetDesktopDisplayMode(display, &dm) == 0) {
            if (w > 0 && h > 0) { dm.w = w; dm.h = h; }
            dm.refresh_rate = 0;
            dm.format = SDL_PIXELFORMAT_RGBA8888;
            SDL_SetWindowDisplayMode(g_window, &dm);
        }
        SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN);
        SDL_ShowCursor(SDL_DISABLE);
    }
}

// The configured Resolution owns the internal raster scale. The window may be
// smaller (the compositor can cap it, or --window-size can request a smaller
// frame), but that must not silently lower the internal resolution: a 1080p
// setting renders at the 1080p scale even in a 720p window.
void refresh_internal_scale() {
    int pw = 0, ph = 0;
    if (g_window) SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    const unsigned width = ps2::g_options.out_width > 0
                               ? static_cast<unsigned>(ps2::g_options.out_width)
                               : static_cast<unsigned>(pw);
    if (width > 0) ps2::set_render_scale_for_width(width);
}

// Keep save states next to the settings file, not in whatever directory the
// launcher happened to use.
std::string state_file_path() {
    const std::filesystem::path settings(ps2::menu::settings_path());
    const std::filesystem::path dir = settings.has_parent_path() ? settings.parent_path()
                                                                 : std::filesystem::path(".");
    return (dir / "states" / "naruto.ps2").string();
}

const ps2::menu::Item *menu_find(const char *id) {
    for (const ps2::menu::Section &s : ps2::menu::menu().sections())
        for (const ps2::menu::Item &it : s.items)
            if (it.id == id) return &it;
    return nullptr;
}

// Applies every staged Display setting atomically. This is only called when
// the player presses X/Apply, which is also the only moment the ini is written.

static float selected_aspect() {
    const char *ar = std::getenv("PS2_PRESENT_AR");
    if (ar) {
        if (std::strcmp(ar, "4:3") == 0) return 4.0f / 3.0f;
        if (std::strcmp(ar, "16:9") == 0) return 16.0f / 9.0f;
        if (std::strcmp(ar, "21:9") == 0) return 21.0f / 9.0f;
        if (std::strcmp(ar, "32:9") == 0) return 32.0f / 9.0f;
        float a = 0.0f, b = 0.0f;
        if (std::sscanf(ar, "%f:%f", &a, &b) == 2 && a > 0.0f && b > 0.0f) return a / b;
        const float v = static_cast<float>(std::atof(ar));
        if (v > 0.5f && v < 8.0f) return v;
    }
    return ps2::g_options.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
}

// Keep a windowed game at the selected presentation aspect. Maximized and
// fullscreen windows are left free; internal rendering stays locked to the
// configured Resolution. SDL2 has no window aspect-ratio constraint, so the
// height is adjusted on resize.
static bool g_aspect_resizing = false;
static void apply_window_aspect(bool resize) {
    if (!g_window || g_aspect_resizing) return;
    const Uint32 flags = SDL_GetWindowFlags(g_window);
    const bool free = (flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_MAXIMIZED)) != 0;
    if (free || !resize) return;
    int w = 0, h = 0;
    SDL_GetWindowSize(g_window, &w, &h);
    if (w > 0) {
        const int target = std::max(1, static_cast<int>(std::lround(static_cast<double>(w) / selected_aspect())));
        if (target != h) {
            g_aspect_resizing = true;
            SDL_SetWindowSize(g_window, w, target);
            g_aspect_resizing = false;
        }
    }
}

void apply_menu_all() {
    const ps2::menu::Item *res = menu_find("resolution");
    const ps2::menu::Item *mode = menu_find("screen_mode");
    int w = ps2::g_options.out_width, h = ps2::g_options.out_height;
    if (res) parse_size(res->current(), w, h);
    if (w > 0 && h > 0) {
        ps2::g_options.out_width = w;
        ps2::g_options.out_height = h;
    }
    if (mode) {
        apply_screen_mode(mode->current(), w, h);
    } else if (g_window && w > 0 && h > 0) {
        SDL_SetWindowSize(g_window, w, h);
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    if (const ps2::menu::Item *it = menu_find("aspect")) {
        env_set("PS2_PRESENT_AR", it->current().c_str());
        ps2::apply_fov();
        ps2::refresh_hud_scale();
        apply_window_aspect(true);
    }
    if (const ps2::menu::Item *it = menu_find("invert_camera"))
        ps2::g_options.invert_camera = it->current() == "On";
    refresh_internal_scale();
    if (const ps2::menu::Item *it = menu_find("anti_aliasing"))
        ps2::g_options.aa = it->current() == "FXAA Ultra" ? 3 : it->current() == "FXAA HQ" ? 2
                           : it->current() == "FXAA" ? 1 : 0;
    if (const ps2::menu::Item *it = menu_find("texture_filter"))
        ps2::set_texture_filter(it->current() == "Nearest" ? 0 : it->current() == "Bilinear" ? 1 : -1);
    if (const ps2::menu::Item *it = menu_find("aniso"))
        ps2::g_options.aniso = std::atoi(it->current().c_str());
    if (const ps2::menu::Item *it = menu_find("draw_distance"))
        ps2::g_options.draw_distance = it->current() == "Very Far" ? 2 : it->current() == "Far" ? 1 : 0;
    if (const ps2::menu::Item *it = menu_find("graphic_api")) {
        const std::string v = it->current();
        if (v == "Vulkan") env_set("PS2_GPU_BACKEND", "vulkan");
        else if (v == "DirectX 12") env_set("PS2_GPU_BACKEND", "dx12");
        else env_unset("PS2_GPU_BACKEND");
        env_unset("PS2_PRESENT_SOFTWARE");
    }
    if (const ps2::menu::Item *it = menu_find("mtvu")) {
        ps2::g_options.mtvu = it->current() == "On";
        if (ps2::g_options.mtvu) env_unset("PS2_NO_VU_THREAD");
        else env_set("PS2_NO_VU_THREAD", "1");
    }
    if (const ps2::menu::Item *it = menu_find("hud_aspect")) {
        const std::string v = it->current();
        if (v == "Auto") env_unset("PS2_HUD_ASPECT");
        else if (v == "Off") env_set("PS2_HUD_ASPECT", "0");
        else if (v == "4:3") env_set("PS2_HUD_ASPECT", "1.0");
        else if (v == "16:9") env_set("PS2_HUD_ASPECT", "0.75");
        else if (v == "21:9") env_set("PS2_HUD_ASPECT", "0.571429");
        else if (v == "32:9") env_set("PS2_HUD_ASPECT", "0.375");
        ps2::refresh_hud_scale();
    }
    if (const ps2::menu::Item *it = menu_find("vsync")) ps2::set_vsync(it->current() == "On");
    if (const ps2::menu::Item *it = menu_find("fps")) {
        int cap = std::atoi(it->current().c_str());
        if (cap > 0) {
            if (ps2::g_options.vsync && g_display_hz > 0) cap = std::min(cap, g_display_hz);
            ps2::g_options.hz = cap;
            ps2::menu::menu().set_value("recomp", "fps", std::to_string(cap));
        }
    }
    if (const ps2::menu::Item *it = menu_find("hud_scale")) {
        if (it->current() == "Auto") env_unset("PS2_HUD_SCALE");
        else env_set("PS2_HUD_SCALE", it->current().c_str());
        ps2::refresh_hud_scale();
    }
    ps2::menu::settings_save();
}

bool create_renderer() {
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    if (!g_renderer) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

bool attach_gpu() {
    if (std::getenv("PS2_SDL_RENDERER")) return false;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(g_window, &info)) return false;
    ps2::WindowTarget target;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    target.width = static_cast<std::uint32_t>(w);
    target.height = static_cast<std::uint32_t>(h);
    switch (info.subsystem) {
#if defined(SDL_VIDEO_DRIVER_X11)
    case SDL_SYSWM_X11:
        target.kind = ps2::WindowTarget::X11;
        target.display = info.info.x11.display;
        target.window = static_cast<std::uint64_t>(info.info.x11.window);
        break;
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    case SDL_SYSWM_WAYLAND:
        target.kind = ps2::WindowTarget::Wayland;
        target.display = info.info.wl.display;
        target.surface = info.info.wl.surface;
        break;
#endif
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
    case SDL_SYSWM_WINDOWS:
        target.kind = ps2::WindowTarget::Windows;
        target.display = info.info.win.hinstance;
        target.window = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(info.info.win.window));
        break;
#endif
    default: return false;
    }
    return ps2::attach_window(target);
}

std::uint8_t axis(SDL_GameController *pad, SDL_GameControllerAxis a) {
    const int v = SDL_GameControllerGetAxis(pad, a);
    return static_cast<std::uint8_t>(std::clamp((v + 32768) >> 8, 0, 255));
}

void rumble(int port) {
    static double last_strong[2] = {};
    static bool last_weak[2] = {};
    static Uint32 sent[2] = {};
    SDL_GameController *pad = g_pads[port];
    if (!pad) return;
    const std::uint16_t motors = ps2::pad_rumble(port);
    const double strong = (motors >> 8) < 16 ? 0.0 : std::sqrt((motors >> 8) / 255.0);
    const bool weak = motors & 1u;
    const Uint32 now = SDL_GetTicks();
    if (std::fabs(strong - last_strong[port]) <= 0.08 && weak == last_weak[port] && (!(strong > 0.0 || weak) || now - sent[port] < 100)) return;
    last_strong[port] = strong;
    last_weak[port] = weak;
    sent[port] = now;
    SDL_GameControllerRumble(pad, static_cast<Uint16>(strong * 65535.0), weak ? 0xFFFF : 0, strong > 0.0 || weak ? 200 : 0);
}

// a new controller takes the first free port, the second one plugs in player 2
void open_pad(int device) {
    const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device);
    for (SDL_GameController *pad : g_pads)
        if (pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == id) return;
    for (int port = 0; port < 2; ++port)
        if (!g_pads[port]) {
            g_pads[port] = SDL_GameControllerOpen(device);
            if (g_pads[port])
                std::fprintf(stderr, "[input] controller %d: %s\n", port, SDL_GameControllerName(g_pads[port]));
            else
                std::fprintf(stderr, "[input] controller %d: open failed (%s)\n", port, SDL_GetError());
            return;
        }
}

void poll_port(int port) {
    static bool plugged[2] = {};
    SDL_GameController *pad = g_pads[port];
    if (pad && !SDL_GameControllerGetAttached(pad)) {
        std::fprintf(stderr, "[input] controller %d disconnected\n", port);
        SDL_GameControllerClose(pad);
        g_pads[port] = pad = nullptr;
    }
    if (port > 0 && plugged[port] != (pad != nullptr)) {
        plugged[port] = pad != nullptr;
        ps2::set_pad_connected(port, plugged[port]);
    }
    std::uint16_t pressed = 0;
    std::uint8_t l2 = 0xFF, r2 = 0xFF;
    std::uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    if (port == 0) {
        const Uint8 *keys = SDL_GetKeyboardState(nullptr);
        for (const KeyBinding &b : kKeys)
            if (keys[b.key]) pressed |= b.button;
        if (keys[SDL_SCANCODE_LEFT]) lx = 0x00;
        if (keys[SDL_SCANCODE_RIGHT]) lx = 0xFF;
        if (keys[SDL_SCANCODE_UP]) ly = 0x00;
        if (keys[SDL_SCANCODE_DOWN]) ly = 0xFF;
    }
    if (pad) {
        for (const PadBinding &b : kPadButtons)
            if (SDL_GameControllerGetButton(pad, b.button)) pressed |= b.mask;
        const auto trigger = [&](SDL_GameControllerAxis a, std::uint16_t bit, std::uint8_t &pressure) {
            const int v = SDL_GameControllerGetAxis(pad, a);
            if (v <= 2000 || (pressed & bit)) return;
            pressed |= bit;
            pressure = static_cast<std::uint8_t>(std::max(1, v * 255 / 32767));
        };
        trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT, kL2, l2);
        trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, kR2, r2);
        if (lx == 0x80) lx = axis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        if (ly == 0x80) ly = axis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        rx = axis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
        ry = axis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
    }
    // While the recomp menu is open it owns the controls: do not also feed the
    // game, or Square/arrows would scroll the Mode Select list underneath.
    static bool masked[2] = {};
    const bool menu_open = ps2::menu::menu().is_open();
    if (masked[port] != menu_open) {
        masked[port] = menu_open;
        std::fprintf(stderr, "[input] pad %d %s\n", port, menu_open ? "masked by recomp menu" : "released");
    }
    if (menu_open) {
        pressed = 0;
        l2 = r2 = 0xFF;
        lx = ly = rx = ry = 0x80;
    }
    rumble(port);
    static std::uint64_t last[2] = {~0ull, ~0ull};
    const std::uint64_t state = pressed | (static_cast<std::uint64_t>(rx) << 16) | (static_cast<std::uint64_t>(ry) << 24) | (static_cast<std::uint64_t>(lx) << 32) |
                                (static_cast<std::uint64_t>(ly) << 40) | (static_cast<std::uint64_t>(l2) << 48) | (static_cast<std::uint64_t>(r2) << 56);
    if (state == last[port]) return;
    last[port] = state;
    ps2::set_pad_pressure(8, l2, port);
    ps2::set_pad_pressure(9, r2, port);
    ps2::set_pad_buttons(pressed, port);
    if (ps2::g_options.invert_camera) {
        rx = static_cast<std::uint8_t>(255 - rx);
        ry = static_cast<std::uint8_t>(255 - ry);
    }
    ps2::set_pad_axis(0, rx, port);
    ps2::set_pad_axis(1, ry, port);
    ps2::set_pad_axis(2, lx, port);
    ps2::set_pad_axis(3, ly, port);
}

void poll() {
    auto &menu = ps2::menu::menu();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) ps2::halt("window closed");
        if (e.type == SDL_KEYDOWN) {
            const SDL_Scancode sc = e.key.keysym.scancode;
            bool handled = false;
            if (!e.key.repeat) {
                if (sc == SDL_SCANCODE_ESCAPE) {
                    if (menu.is_open()) { menu.back(); handled = true; }
                } else if (sc == SDL_SCANCODE_A) {
                    // Square opens the settings menu in Mode Select and closes it
                    // while the menu is already open. The overlay is WebGPU-only;
                    // in Software mode never open an invisible menu and mask the
                    // pad (that made the joystick look dead).
                    if (menu.is_open()) { menu.back(); handled = true; }
                    else if (ps2::recomp_menu_available()) {
                        if (g_gpu_present) {
                            menu.open();
                        } else {
                            static bool warned = false;
                            if (!warned) {
                                warned = true;
                                std::fprintf(stderr, "[menu] overlay disabled by Graphic API=Software; use Auto/Vulkan\n");
                            }
                        }
                        handled = true;
                    }
                } else if (menu.is_open()) {
                    if (sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_W) { menu.up(); handled = true; }
                    else if (sc == SDL_SCANCODE_DOWN || sc == SDL_SCANCODE_S) { menu.down(); handled = true; }
                    else if (sc == SDL_SCANCODE_LEFT) { menu.left(); handled = true; }
                    else if (sc == SDL_SCANCODE_Q) { menu.prev_section(); handled = true; }
                    else if (sc == SDL_SCANCODE_E) { menu.next_section(); handled = true; }
                    else if (sc == SDL_SCANCODE_RIGHT) { menu.right(); handled = true; }
                    else if (sc == SDL_SCANCODE_X || sc == SDL_SCANCODE_Z || sc == SDL_SCANCODE_RETURN) { menu.confirm(); handled = true; }
                    else if (sc == SDL_SCANCODE_BACKSPACE) { menu.back(); handled = true; }
                }
            } else if (menu.is_open()) {
                handled = sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN || sc == SDL_SCANCODE_LEFT ||
                          sc == SDL_SCANCODE_RIGHT || sc == SDL_SCANCODE_W || sc == SDL_SCANCODE_S ||
                          sc == SDL_SCANCODE_X || sc == SDL_SCANCODE_Z || sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_A ||
                          sc == SDL_SCANCODE_BACKSPACE || sc == SDL_SCANCODE_ESCAPE;
            }
            if (handled) continue;
        }
        if (e.type == SDL_CONTROLLERBUTTONDOWN) {
            switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_X: {  // Square
                if (menu.is_open()) {
                    menu.back();
                } else if (ps2::recomp_menu_available()) {
                    if (g_gpu_present) {
                        menu.open();
                    } else {
                        static bool warned = false;
                        if (!warned) {
                            warned = true;
                            std::fprintf(stderr, "[menu] overlay disabled by Graphic API=Software; use Auto/Vulkan\n");
                        }
                    }
                }
                break;
            }
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                if (menu.is_open()) menu.prev_section();
                break;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                if (menu.is_open()) menu.next_section();
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                if (menu.is_open()) menu.up();
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                if (menu.is_open()) menu.down();
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                if (menu.is_open()) menu.left();
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                if (menu.is_open()) menu.right();
                break;
            case SDL_CONTROLLER_BUTTON_A:
                if (menu.is_open()) menu.confirm();
                break;
            case SDL_CONTROLLER_BUTTON_B:
                if (menu.is_open()) menu.back();
                break;
            default: break;
            }
        }
        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F11 && !e.key.repeat) {
            const bool full = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN;
            SDL_SetWindowFullscreen(g_window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            SDL_ShowCursor(full ? SDL_ENABLE : SDL_DISABLE);
        }
        if (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_MAXIMIZED ||
                                           e.window.event == SDL_WINDOWEVENT_RESTORED))
            apply_window_aspect(e.window.event == SDL_WINDOWEVENT_RESTORED);
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && g_gpu_present) {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(g_window, &w, &h);
            ps2::resize_window(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
            refresh_internal_scale();
            if (!g_aspect_resizing) apply_window_aspect(true);
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED) open_pad(e.cdevice.which);
        if (e.type == SDL_CONTROLLERDEVICEREMOVED)
            for (SDL_GameController *&pad : g_pads)
                if (pad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
                    SDL_GameControllerClose(pad);
                    pad = nullptr;
                }
    }
    // If the game leaves Mode Select while the menu is open (for example after
    // an auto-transition), close it and release the masked controls.
    if (menu.is_open() && !ps2::recomp_menu_available()) menu.close();
    // Hotplug watchdog: some drivers drop the controller without delivering a
    // removal event. Re-scan once a second so the pad cannot stay dead.
    static Uint32 last_pad_scan = 0;
    const Uint32 now = SDL_GetTicks();
    if (now - last_pad_scan >= 1000) {
        last_pad_scan = now;
        for (SDL_GameController *&pad : g_pads)
            if (pad && !SDL_GameControllerGetAttached(pad)) {
                SDL_GameControllerClose(pad);
                pad = nullptr;
            }
        if (!g_pads[0])
            for (int i = 0; i < SDL_NumJoysticks(); ++i)
                if (SDL_IsGameController(i)) {
                    open_pad(i);
                    if (g_pads[0]) break;
                }
    }
    poll_port(0);
    poll_port(1);
}

void show_pixels(const std::uint32_t *rgba, std::uint32_t w, std::uint32_t h) {
    if (w != g_tex_w || h != g_tex_h) {
        if (g_texture) SDL_DestroyTexture(g_texture);
        g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, static_cast<int>(w), static_cast<int>(h));
        SDL_RenderSetLogicalSize(g_renderer, static_cast<int>(w), static_cast<int>(w) * (ps2::g_options.widescreen ? 9 : 3) / (ps2::g_options.widescreen ? 16 : 4));
        g_tex_w = w;
        g_tex_h = h;
    }
    SDL_UpdateTexture(g_texture, nullptr, rgba, static_cast<int>(w * 4));
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
    SDL_RenderPresent(g_renderer);
}

void present() {
    // --hz is the screen presentation rate; --fps is the emulated vblank/game
    // rate. When hz is lower than fps, skip presents and let the monitor keep
    // showing the last image.
    if (ps2::g_options.hz > 0 && ps2::g_options.hz < g_display_hz) {
        static std::uint64_t last = 0;
        const std::uint64_t frequency = SDL_GetPerformanceFrequency();
        const std::uint64_t now = SDL_GetPerformanceCounter();
        const std::uint64_t period = frequency / static_cast<std::uint64_t>(ps2::g_options.hz);
        if (last && now - last < period) return;
        last = now;
    }
    const bool interp = ps2::g_options.interpolate;
    const int mode = interp ? (ps2::game_frame_new() ? 1 : 2) : 0;
    if (g_gpu_present) {
        ps2::present_frame(mode);
        ++g_presents;
        return;
    }
    std::uint32_t w = 0, h = 0;
    if (ps2::display_frame(g_pixels, w, h)) {
        show_pixels(g_pixels.data(), w, h);
        ++g_presents;
    }
}

constexpr Uint32 kAudioBytesPerMs = 48 * 4;

void pace() {
    if (ps2::g_options.fps < 0) return;  // unlocked: run as fast as the host can
    const int target = ps2::g_options.pal ? 50 : 60;
    if (target <= 0) return;
    // Pace the logical vblank (and therefore the presents) at the native rate.
    // The old audio-only path allowed the emulation to run in bursts until
    // 80 ms of sound was queued, which made the render thread present two or
    // three frames back-to-back and then wait ~20 ms: the 1% low showed ~23 FPS
    // even though the average was 60. Sleeping the remaining time every vblank
    // keeps the cadence stable; the audio queue is only an underrun guard.
    const std::uint64_t frequency = SDL_GetPerformanceFrequency();
    const std::uint64_t period = frequency / static_cast<std::uint64_t>(target);
    const std::uint64_t now = SDL_GetPerformanceCounter();
    if (!g_last) {
        g_last = now;
    } else if (now - g_last < period) {
        const std::uint64_t remain = period - (now - g_last);
        SDL_Delay(static_cast<Uint32>((remain * 1000u + frequency - 1u) / frequency));
        g_last += period;
    } else if (now - g_last > period * 2u) {
        g_last = now;  // long stall (load/state): do not accumulate a backlog
    } else {
        g_last += period;
    }
    if (g_audio) {
        // Keep a small safety margin against audio underruns. This no longer
        // drives the frame cadence, only the maximum queue depth.
        while (SDL_GetQueuedAudioSize(g_audio) > 60 * kAudioBytesPerMs) SDL_Delay(1);
    }
}

double process_cpu_seconds() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0.0;
    auto ticks = [](const FILETIME &t) {
        ULARGE_INTEGER v;
        v.LowPart = t.dwLowDateTime;
        v.HighPart = t.dwHighDateTime;
        return v.QuadPart;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 1e7;
#else
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) + static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
#endif
}

double system_cpu_percent() {
#ifdef _WIN32
    static unsigned long long last_total = 0, last_idle = 0;
    FILETIME idle_ft{}, kernel_ft{}, user_ft{};
    if (!GetSystemTimes(&idle_ft, &kernel_ft, &user_ft)) return 0.0;
    auto ticks = [](const FILETIME &t) {
        ULARGE_INTEGER v;
        v.LowPart = t.dwLowDateTime;
        v.HighPart = t.dwHighDateTime;
        return v.QuadPart;
    };
    const unsigned long long total = ticks(kernel_ft) + ticks(user_ft);
    const unsigned long long idle = ticks(idle_ft);
    const unsigned long long dt = total - last_total, di = idle - last_idle;
    last_total = total; last_idle = idle;
    return dt ? 100.0 * static_cast<double>(dt - di) / static_cast<double>(dt) : 0.0;
#else
    static unsigned long long last_total = 0, last_idle = 0;
    unsigned long long total = 0, idle = 0;
    if (std::FILE *f = std::fopen("/proc/stat", "r")) {
        char line[256] = {};
        if (std::fgets(line, sizeof line, f)) {
            unsigned long long v[10] = {};
            std::sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                        &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
            for (int i = 0; i < 10; ++i) total += v[i];
            idle = v[3] + v[4];
        }
        std::fclose(f);
    }
    const unsigned long long dt = total - last_total, di = idle - last_idle;
    last_total = total; last_idle = idle;
    return dt ? 100.0 * static_cast<double>(dt - di) / static_cast<double>(dt) : 0.0;
#endif
}

double system_gpu_percent() {
#ifdef _WIN32
    return 0.0;  // GPU busy time is not read from Windows in this build
#else
    for (int i = 0; i < 4; ++i) {
        char path[96];
        std::snprintf(path, sizeof path, "/sys/class/drm/card%d/device/gpu_busy_percent", i);
        if (std::FILE *f = std::fopen(path, "r")) {
            int v = 0;
            const bool ok = std::fscanf(f, "%d", &v) == 1;
            std::fclose(f);
            if (ok) return static_cast<double>(v);
        }
    }
    return 0.0;
#endif
}

double resident_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
        return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    return 0.0;
#else
    long pages = 0, resident = 0;
    if (std::FILE *f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        std::fclose(f);
    }
    return static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
#endif
}

void show_speed() {
    static std::uint64_t start = 0, frames = 0;
    static double cpu_start = 0.0, gpu_start = 0.0;
    const std::uint64_t now = SDL_GetPerformanceCounter();
    if (!start) {
        start = now;
        cpu_start = process_cpu_seconds();
        gpu_start = ps2::gpu_busy_ms();
    }
    ++frames;
    const double seconds = static_cast<double>(now - start) / static_cast<double>(SDL_GetPerformanceFrequency());
    if (seconds < 1.0) return;
    const double cpu = process_cpu_seconds(), gpu = ps2::gpu_busy_ms();
    char title[768];
    const double queued = g_audio ? SDL_GetQueuedAudioSize(g_audio) / static_cast<double>(kAudioBytesPerMs) : 0.0;
    static unsigned long long scene_last = 0;
    static double vu_last = 0.0;
    const unsigned long long scene_total = ps2_scene_updates();
    const double scene = static_cast<double>(scene_total - scene_last) / seconds;
    scene_last = scene_total;
    const double vu_total = ps2::vu_thread_idle_ms();
    const double vu_idle = (vu_total - vu_last) / (seconds * 10.0);
    vu_last = vu_total;
    (void)scene; (void)vu_idle; (void)queued;
    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(g_window, &win_w, &win_h);
    const std::uint32_t iscale = ps2::internal_scale();
    const std::uint32_t pres_w = ps2::present_width();
    const std::uint32_t pres_h = ps2::present_height();
    const int cfg_w = ps2::g_options.out_width > 0 ? ps2::g_options.out_width : win_w;
    const int cfg_h = ps2::g_options.out_height > 0 ? ps2::g_options.out_height : win_h;
    const std::uint32_t iw = ps2::internal_width();
    const std::uint32_t ih = ps2::internal_height();
    const double render_fps = static_cast<double>(g_presents) / seconds;
    const double logic_fps = static_cast<double>(g_game_frames) / seconds;
    const double frame_ms = render_fps > 0.0 ? 1000.0 / render_fps : 0.0;
    std::snprintf(title, sizeof title,
                  "FPS Render %.1f | FPS Logica %.1f | Frame Time %.1f ms | Hz %d | Uso CPU EE %.0f%% | Uso GPU GS %.0f%% | Uso RAM %.0f MB | Uso CPU real %.0f%% | Uso GPU real %.0f%% | Janela %dx%d | Resolucao apresentada %ux%u | Resolucao configurada %dx%d | Resolucao interna %ux%u | Supersampling %ux",
                  render_fps, logic_fps, frame_ms, g_display_hz,
                  std::max(0.0, (cpu - cpu_start) / seconds * 100.0),
                  std::max(0.0, (gpu - gpu_start) / (seconds * 10.0)),
                  resident_mb(), system_cpu_percent(), system_gpu_percent(),
                  win_w, win_h, pres_w, pres_h, cfg_w, cfg_h, iw, ih, iscale);
    SDL_SetWindowTitle(g_window, title);
    if (std::getenv("PS2_STATS")) std::fprintf(stderr, "[stats] %s\n", title);
    start = now;
    frames = 0;
    g_game_frames = 0;
    g_presents = 0;
    cpu_start = cpu;
    gpu_start = gpu;
}

void on_vblank() {
    show_speed();
    poll();
    present();
    pace();
}

void queue_audio(const std::int16_t *samples, std::size_t frames) {
    if (SDL_GetQueuedAudioSize(g_audio) > 500 * kAudioBytesPerMs) return;
    SDL_QueueAudio(g_audio, samples, static_cast<Uint32>(frames * 4));
}

}

static bool create_game_window(bool fullscreen, int win_w, int win_h, bool exact, bool borderless) {
    // The initial size comes from Display Settings > Resolution; the window
    // can still be maximized/resized, but that does not change the internal
    // raster scale (which stays fixed by the Resolution setting).
    const Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    g_window = SDL_CreateWindow("ps2-web-recomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, flags);
    if (!g_window) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    if (fullscreen) {
        // An explicit --resolution asks for that exact mode: a desktop-fullscreen
        // window gets the *logical* desktop size on scaled displays, and the
        // compositor then upscales our buffer (blurry output).
        if (exact && !borderless) {
            SDL_DisplayMode mode{};
            mode.w = win_w;
            mode.h = win_h;
            mode.refresh_rate = 0;
            mode.format = SDL_PIXELFORMAT_RGBA8888;
            if (SDL_SetWindowDisplayMode(g_window, &mode) != 0)
                std::fprintf(stderr, "[window] %dx%d mode unavailable (%s), using desktop size\n", win_w, win_h, SDL_GetError());
        }
        SDL_SetWindowFullscreen(g_window, (exact && !borderless) ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    return true;
}

static bool drawable_size(int &pw, int &ph) {
    pw = ph = 0;
    if (!g_window) return false;
    SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    return pw > 0 && ph > 0;
}

static void close_game_window() {
    if (!g_window) return;
    SDL_DestroyWindow(g_window);
    g_window = nullptr;
}

bool open_window(bool fullscreen, bool borderless) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    const bool exact = ps2::g_options.out_width > 0 && ps2::g_options.out_height > 0;
    const int win_w = ps2::g_options.window_width > 0 ? ps2::g_options.window_width
                    : ps2::g_options.out_width > 0 ? ps2::g_options.out_width
                    : (ps2::g_options.widescreen ? 1280 : 960);
    int win_h = ps2::g_options.window_height > 0 ? ps2::g_options.window_height
              : ps2::g_options.out_height > 0 ? ps2::g_options.out_height
              : 720;
    if (!fullscreen && win_w > 0) {
        const float aspect = selected_aspect();
        win_h = std::max(1, static_cast<int>(std::lround(static_cast<double>(win_w) / aspect)));
    }
    const int tol = 4;
    const auto fits = [&](int pw, int ph) { return std::abs(pw - win_w) <= tol && std::abs(ph - win_h) <= tol; };
    if (!create_game_window(fullscreen, win_w, win_h, exact, borderless)) return false;
    apply_window_aspect(false);
    {
        // The drawable is what the compositor will show 1:1; use it (not the
        // requested resolution) to pick the internal raster scale.
        int pw = 0, ph = 0;
        drawable_size(pw, ph);
        if (pw > 0 && ph > 0) {
            if (exact && !fits(pw, ph)) {
                std::fprintf(stderr,
                             "[window] asked for %dx%d, drawable is %dx%d -- the compositor capped it\n"
                             "         (the surface cannot be larger than the desktop's logical size;\n"
                             "          with desktop scaling >100%% that is why resolutions look the same).\n"
                             "         a window also cannot be taller than the desktop work area,\n"
                             "         so 4k needs --fullscreen (which switches the output mode).\n",
                             win_w, win_h, pw, ph);
            }
            std::fprintf(stderr, "[window] driver %s, drawable %dx%d (requested %dx%d)\n",
                         SDL_GetCurrentVideoDriver(), pw, ph, win_w, win_h);
        }
    }
    SDL_DisplayMode dm{};
    if (SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(g_window), &dm) == 0 && dm.refresh_rate > 0)
        g_display_hz = dm.refresh_rate;
    if (ps2::g_options.hz <= 0) ps2::g_options.hz = g_display_hz;
    // V-Sync On caps to the monitor; V-Sync Off may present faster than it.
    if (ps2::g_options.vsync && g_display_hz > 0 && ps2::g_options.hz > g_display_hz)
        ps2::g_options.hz = g_display_hz;
    ps2::g_options.hz = std::max(30, std::min(ps2::g_options.hz, 240));
    std::fprintf(stderr, "[window] display %d Hz, presentation cap %d Hz\n", g_display_hz, ps2::g_options.hz);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    g_audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_audio) {
        SDL_PauseAudioDevice(g_audio, 0);
        ps2::set_audio_sink(queue_audio);
    }
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i)) open_pad(i);
    if (fullscreen) SDL_ShowCursor(SDL_DISABLE);

    auto &menu = ps2::menu::menu();
    // Only X/Apply commits and writes game_settings.ini. on_close intentionally
    // does not save: closing discards the staged values.
    menu.on_change = nullptr;
    menu.on_apply = [](const ps2::menu::Item &item) {
        if (item.id == "save_state") {
            const std::filesystem::path path(state_file_path());
            std::error_code ec;
            if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
            ps2::request_save_state(path.string());
            std::fprintf(stderr, "[menu] save state requested -> %s\n", path.string().c_str());
            return;
        }
        if (item.id == "load_state") {
            const std::string path = state_file_path();
            ps2::request_load_state(path);
            std::fprintf(stderr, "[menu] load state requested <- %s\n", path.c_str());
            return;
        }
        if (item.id == "exit_game") {
            SDL_MessageBoxButtonData buttons[2] = {
                {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Sair do jogo"},
                {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, "Cancelar"},
            };
            SDL_MessageBoxData box = {};
            box.flags = SDL_MESSAGEBOX_WARNING;
            box.window = g_window;
            box.title = "Recomp Settings";
            box.message = "Deseja realmente sair do jogo?";
            box.numbuttons = 2;
            box.buttons = buttons;
            int choice = 1;
            if (SDL_ShowMessageBox(&box, &choice) == 0 && choice == 0) {
                std::fprintf(stderr, "[menu] exit game confirmed\n");
                ps2::halt("exit game");
            } else {
                std::fprintf(stderr, "[menu] exit game cancelled\n");
            }
            return;
        }
        apply_menu_all();
    };
    menu.on_close = nullptr;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    // Keep the configured Resolution in the menu; the drawable may be smaller
    // only when the desktop cannot physically fit the window.
    const unsigned cfg_w = ps2::g_options.out_width > 0 ? static_cast<unsigned>(ps2::g_options.out_width) : static_cast<unsigned>(pw);
    const unsigned cfg_h = ps2::g_options.out_height > 0 ? static_cast<unsigned>(ps2::g_options.out_height) : static_cast<unsigned>(ph);
    ps2::menu::settings_sync_runtime(cfg_w, cfg_h, fullscreen, borderless);
    return true;
}

int run_window(const std::function<int()> &emulate) {
    ps2::init_graphics();
    refresh_internal_scale();
    g_gpu_present = std::getenv("PS2_PRESENT_SOFTWARE") ? false : attach_gpu();
    std::fprintf(stderr, "[window] %s\n", g_gpu_present ? "presenting through WebGPU" : "presenting through SDL");
    if (!g_gpu_present && std::getenv("PS2_PRESENT_SOFTWARE"))
        std::fprintf(stderr, "[menu] Graphic API=Software disables the Recomp Settings overlay; use Auto/Vulkan to open it\n");
    if (!g_gpu_present && !create_renderer()) return 1;
    const bool threads = !std::getenv("PS2_ONE_THREAD") && !std::getenv("PS2_TIME") && ps2::g_options.frames_dir.empty();
    if (!threads) {
        ps2::g_options.on_vblank = on_vblank;
        return emulate();
    }
    ps2::g_options.on_vblank = [] {
        int target = ps2::g_options.fps > 0 ? ps2::g_options.fps : ps2::g_options.hz;
        if (ps2::g_options.hz > 0) target = std::min(target, ps2::g_options.hz);
        target = std::max(target, 30);
        const int native = ps2::g_options.pal ? 50 : 60;
        // Wait for the real-time frame slot BEFORE posting the present. The
        // emulation can reach the vblank event early or late depending on how
        // much CPU work the game did in that frame; sleeping afterwards let the
        // presents land in pairs (2 ms then 25-33 ms apart). Aligning first
        // makes the render thread receive one present per slot.
        pace();
        ps2::present_log_mark('S');
        // Logical vblanks stay native (the game keeps its normal speed); the
        // requested FPS only controls how many frames are PRESENTED. A 30 FPS
        // target therefore shows every other field instead of slowing the game
        // (the old frame_cycles approach made 30 feel slow and miss the target).
        static double present_acc = 0.0;
        present_acc += static_cast<double>(target);
        int presents = 0;
        while (present_acc >= static_cast<double>(native) && presents < 4) {
            present_acc -= static_cast<double>(native);
            ++presents;
        }
        const bool interp = ps2::g_options.interpolate;
        if (ps2::game_frame_new()) ++g_game_frames;
        g_presents += static_cast<std::uint64_t>(presents);
        for (int i = 0; i < presents; ++i) {
            const int mode = interp
                                 ? (ps2::game_frame_new() && i == 0 ? 1 : (i == 0 ? 2 : 3))
                                 : 0;
            ps2::post_present(mode);
        }
    };
    ps2::start_render_thread();
    if (!std::getenv("PS2_NO_VU_THREAD")) {
        ps2::start_vu_thread();
        std::thread(ps2::run_vu_thread).detach();
    }
    std::atomic<bool> done{false};
    int result = 0;
    std::thread emulation([&] {
        result = emulate();
        done.store(true);
        ps2::wake_render_thread();
    });
    const std::function<void(const std::uint32_t *, std::uint32_t, std::uint32_t)> copy = show_pixels, none;
    while (!done.load()) {
        poll();
        if (ps2::render_pending(g_gpu_present, g_gpu_present ? none : copy, show_speed, [] { static const double b = std::getenv("PS2_RENDER_BUDGET") ? std::atof(std::getenv("PS2_RENDER_BUDGET")) : 8.0; return b; }()) == ps2::Rendered::idle) ps2::wait_render_work(4);
    }
    emulation.join();
    SDL_Quit();
    std::fflush(nullptr);
    std::_Exit(result);
}
