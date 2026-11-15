#include "game_settings.hpp"

#include "recomp_menu.hpp"
#include "practice_settings.hpp"

#include "ps2/system.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifdef __linux__
#include <unistd.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

namespace ps2::menu {

namespace {
#ifdef _WIN32
void env_set(const char *name, const char *value) { _putenv_s(name, value); }
void env_unset(const char *name) { _putenv_s(name, ""); }
#else
void env_set(const char *name, const char *value) { setenv(name, value, 1); }
void env_unset(const char *name) { unsetenv(name); }
#endif

struct Ini {
    std::map<std::string, std::map<std::string, std::string>> sections;
    bool found = false;
};

// Launchers do not always run with the project directory as the working
// directory (desktop shortcuts, `./build/dev-naruto/ps2run` from a shell...).
// Look for the ini next to the cwd and then next to/above the executable, so
// the saved settings are found no matter how the game is opened.
std::string find_settings_file() {
    if (const char *env = std::getenv("PS2_SETTINGS"); env && *env) return env;
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> candidates;
    const auto add_tree = [&](fs::path dir, int levels) {
        for (int i = 0; i < levels && !dir.empty(); ++i) {
            candidates.push_back(dir / "game_settings.ini");
            const fs::path parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
    };
    const fs::path cwd = fs::current_path(ec);
    if (!ec) add_tree(cwd, 5);
#ifdef __linux__
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = '\0';
        add_tree(fs::path(buf).parent_path(), 6);
    }
#elif defined(_WIN32)
    char buf[4096];
    const DWORD n = GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof buf));
    if (n > 0 && n < sizeof buf) {
        buf[n] = '\0';
        add_tree(fs::path(buf).parent_path(), 6);
    }
#endif
    for (const fs::path &p : candidates)
        if (fs::exists(p, ec)) return p.string();
    return "game_settings.ini";
}

std::string &path_storage() {
    static std::string path = find_settings_file();
    return path;
}

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(std::string s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

bool parse_pair(const std::string &text, unsigned &w, unsigned &h) {
    return std::sscanf(text.c_str(), "%ux%u", &w, &h) == 2 && w >= 320 && h >= 200 && w <= 16384 && h <= 16384;
}

Ini read_ini() {
    Ini ini;
    std::ifstream in(path_storage());
    if (!in) return ini;
    ini.found = true;
    std::string line, section;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[') {
            const auto end = line.find(']');
            if (end != std::string::npos) section = lower(trim(line.substr(1, end - 1)));
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos || section.empty()) continue;
        std::string key = lower(trim(line.substr(0, eq)));
        std::string value = trim(line.substr(eq + 1));
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        if (!key.empty() && !value.empty()) ini.sections[section][key] = value;
    }
    return ini;
}

void set_env(const char *name, const std::string &value) {
    // Startup values do not override an explicit environment variable; the
    // runtime menu does (it is the user's newest choice).
    env_set(name, value.c_str());
}

std::string option_or(const std::string &value, const std::vector<std::string> &options, const std::string &fallback) {
    for (const std::string &o : options)
        if (lower(o) == lower(value)) return o;
    return fallback;
}

void set_item(const char *section, const char *item, const std::string &value) {
    if (!value.empty()) menu().set_value(section, item, value);
}

const Item *find_item(const char *section, const char *id) {
    for (const Section &s : menu().sections()) {
        if (s.id != section) continue;
        for (const Item &it : s.items)
            if (it.id == id) return &it;
    }
    return nullptr;
}

std::string &screen_mode_storage() {
    static std::string mode = "Windowed";
    return mode;
}

std::string &ini_aspect_storage() {
    static std::string aspect;
    return aspect;
}

bool &user_aspect_env_storage() {
    static bool set = false;
    return set;
}

// Applies one [recomp] value to the runtime environment/options.
void apply_display(const std::map<std::string, std::string> &values, std::string &screen_mode) {
    if (auto it = values.find("resolution"); it != values.end()) {
        unsigned w = 0, h = 0;
        if (parse_pair(it->second, w, h)) {
            ps2::g_options.out_width = static_cast<int>(w);
            ps2::g_options.out_height = static_cast<int>(h);
        }
    }
    if (auto it = values.find("invert_camera"); it != values.end())
        ps2::g_options.invert_camera = lower(it->second) == "on";
    if (auto it = values.find("aspect"); it != values.end()) ini_aspect_storage() = it->second;
    if (auto it = values.find("fps"); it != values.end()) {
        const int cap = std::atoi(it->second.c_str());
        if (cap > 0) ps2::g_options.hz = cap;
    }
    if (auto it = values.find("hud_scale"); it != values.end()) {
        const std::string v = it->second;
        if (lower(v) != "auto") set_env("PS2_HUD_SCALE", v);
    }
    if (auto it = values.find("anti_aliasing"); it != values.end()) {
        const std::string v = lower(it->second);
        ps2::g_options.aa = v == "off" ? 0 : v.find("ultra") != std::string::npos ? 3
                         : v.find("hq") != std::string::npos ? 2
                         : v.find("fxaa") != std::string::npos ? 1 : 0;
    }
    if (auto it = values.find("texture_filter"); it != values.end()) {
        const std::string v = lower(it->second);
        ps2::g_options.filter = v == "nearest" ? 0 : v == "bilinear" ? 1 : -1;
    }
    if (auto it = values.find("aniso"); it != values.end())
        ps2::g_options.aniso = std::atoi(it->second.c_str());
    if (auto it = values.find("draw_distance"); it != values.end()) {
        const std::string v = lower(it->second);
        ps2::g_options.draw_distance = v == "far" ? 1 : v == "very far" ? 2 : 0;
    }
    if (auto it = values.find("graphic_api"); it != values.end()) {
        const std::string v = lower(it->second);
        if (v == "vulkan") env_set("PS2_GPU_BACKEND", "vulkan");
#ifdef _WIN32
        else if (v == "directx 12" || v == "dx12") env_set("PS2_GPU_BACKEND", "dx12");
#endif
        else env_unset("PS2_GPU_BACKEND");
        env_unset("PS2_PRESENT_SOFTWARE");
    }
    if (auto it = values.find("mtvu"); it != values.end()) {
        ps2::g_options.mtvu = lower(it->second) != "off";
        if (ps2::g_options.mtvu) env_unset("PS2_NO_VU_THREAD");
        else env_set("PS2_NO_VU_THREAD", "1");
    }
    if (auto it = values.find("hud_aspect"); it != values.end()) {
        const std::string v = lower(it->second);
        if (v == "auto") env_unset("PS2_HUD_ASPECT");
        else if (v == "off") env_set("PS2_HUD_ASPECT", "0");
        else if (v == "4:3") env_set("PS2_HUD_ASPECT", "1.0");
        else if (v == "16:9") env_set("PS2_HUD_ASPECT", "0.75");
        else if (v == "21:9") env_set("PS2_HUD_ASPECT", "0.571429");
        else if (v == "32:9") env_set("PS2_HUD_ASPECT", "0.375");
    } else {
        // Default Off: the HUD X correction touched world-shared textures and
        // produced misplaced scenery objects in Master Mode.
        env_set("PS2_HUD_ASPECT", "0");
    }
    if (auto it = values.find("vsync"); it != values.end())
        ps2::g_options.vsync = lower(it->second) != "off";
    if (auto it = values.find("screen_mode"); it != values.end()) {
        const std::string v = lower(it->second);
        if (v == "fullscreen") screen_mode = "Fullscreen";
        else if (v == "borderless") screen_mode = "Borderless";
        else if (v == "windowed") screen_mode = "Windowed";
        else screen_mode = it->second;
    }
}

} // namespace

const std::string &settings_path() { return path_storage(); }
const std::string &settings_screen_mode() { return screen_mode_storage(); }

bool settings_load() {
    // Remember whether the user/launcher provided PS2_PRESENT_AR on its own.
    // The ini aspect only fills in when the environment did not set one, and
    // --widescreen (a CLI option) still wins over the ini value.
    user_aspect_env_storage() = std::getenv("PS2_PRESENT_AR") != nullptr;
    const Ini ini = read_ini();
    if (!ini.found) {
        std::fprintf(stderr, "[settings] %s not found; using defaults\n", path_storage().c_str());
        return false;
    }
    std::map<std::string, std::string> saved;
    for (const auto &[section, values] : ini.sections) {
        std::string target = section;
        if (section == "display" || section == "graphics" || section == "video") target = "recomp";
        for (const auto &[key, value] : values) {
            // Drop options removed from this build before they are re-added to
            // the menu by set_value (old game_settings.ini files may still say
            // Software/OpenGL/DirectX 12).
            if (target == "recomp" && key == "graphic_api") {
                const std::string gv = lower(value);
                bool ok = gv == "auto" || gv == "vulkan";
#ifdef _WIN32
                ok = ok || gv == "directx 12" || gv == "dx12";
#endif
                if (!ok) continue;
            }
            // Apply values by their real section id. Non-display sections
            // (e.g. [game] invert_camera) must reach the runtime too, not only
            // the menu, or settings_sync_effective overwrites them at startup.
            if (target == "recomp" || target == "game") saved[key] = value;
            menu().set_value(target, key, value);
        }
    }
    apply_display(saved, screen_mode_storage());
    std::fprintf(stderr, "[settings] loaded %s\n", path_storage().c_str());
    return true;
}

void settings_sync_effective() {
    if (!user_aspect_env_storage()) {
        // The saved aspect wins over the --widescreen flag: that flag only
        // enables the widescreen patches, it is not an aspect choice. An
        // explicit PS2_PRESENT_AR from the launcher still wins over both.
        if (!ini_aspect_storage().empty()) env_set("PS2_PRESENT_AR", ini_aspect_storage().c_str());
        else if (ps2::g_options.widescreen) env_set("PS2_PRESENT_AR", "16:9");
        else env_unset("PS2_PRESENT_AR");
    }
    if (ps2::g_options.out_width > 0 && ps2::g_options.out_height > 0) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%dx%d", ps2::g_options.out_width, ps2::g_options.out_height);
        set_item("recomp", "resolution", buf);
    }
    if (const char *ar = std::getenv("PS2_PRESENT_AR"); ar && *ar) {
        const Item *it = find_item("recomp", "aspect");
        if (it) set_item("recomp", "aspect", option_or(ar, it->options, ""));
    } else {
        set_item("recomp", "aspect", ps2::g_options.widescreen ? "16:9" : "4:3");
    }
    set_item("game", "invert_camera", ps2::g_options.invert_camera ? "On" : "Off");
    {
        const int cap = ps2::g_options.hz > 0 ? ps2::g_options.hz : 60;
        set_item("recomp", "fps", std::to_string(cap));
    }
    if (const char *hud = std::getenv("PS2_HUD_SCALE"); hud && *hud) {
        const Item *it = find_item("recomp", "hud_scale");
        if (it) set_item("recomp", "hud_scale", option_or(hud, it->options, ""));
    }
    set_item("recomp", "anti_aliasing",
             ps2::g_options.aa >= 3 ? "FXAA Ultra" : ps2::g_options.aa == 2 ? "FXAA HQ" : ps2::g_options.aa == 1 ? "FXAA" : "Off");
    set_item("recomp", "texture_filter", ps2::g_options.filter == 0 ? "Nearest" : ps2::g_options.filter > 0 ? "Bilinear" : "Auto");
    {
        const int a = ps2::g_options.aniso;
        set_item("recomp", "aniso", a == 2 ? "2x" : a == 4 ? "4x" : a == 8 ? "8x" : a == 16 ? "16x" : "Off");
    }
    set_item("recomp", "draw_distance", ps2::g_options.draw_distance == 2 ? "Very Far" : ps2::g_options.draw_distance == 1 ? "Far" : "Default");
    if (const char *b = std::getenv("PS2_GPU_BACKEND"); b && lower(b) == "vulkan") set_item("recomp", "graphic_api", "Vulkan");
#ifdef _WIN32
    else if (const char *b2 = std::getenv("PS2_GPU_BACKEND"); b2 && lower(b2) == "dx12") set_item("recomp", "graphic_api", "DirectX 12");
#endif
    else set_item("recomp", "graphic_api", "Auto");
    set_item("recomp", "mtvu", std::getenv("PS2_NO_VU_THREAD") ? "Off" : "On");
    if (const char *ha = std::getenv("PS2_HUD_ASPECT")) {
        const float v = static_cast<float>(std::atof(ha));
        set_item("recomp", "hud_aspect", std::strcmp(ha, "0") == 0 ? "Off" :
                 v > 0.95f ? "4:3" : v > 0.70f ? "16:9" : v > 0.50f ? "21:9" : v > 0.30f ? "32:9" : "Auto");
    } else {
        set_item("recomp", "hud_aspect", "Off");
    }
    set_item("recomp", "vsync", ps2::g_options.vsync ? "On" : "Off");
}

void settings_sync_runtime(unsigned width, unsigned height, bool fullscreen, bool borderless) {
    if (width && height) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%ux%u", width, height);
        set_item("recomp", "resolution", buf);
    }
    set_item("recomp", "screen_mode", borderless ? "Borderless" : fullscreen ? "Fullscreen" : "Windowed");
    set_item("recomp", "vsync", ps2::g_options.vsync ? "On" : "Off");
}

void settings_ensure_file() {
    std::ifstream in(path_storage());
    if (!in) settings_save();
}

bool settings_save() {
    std::ofstream out(path_storage(), std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "[settings] cannot write %s\n", path_storage().c_str());
        return false;
    }
    out << "; " << kMenuTitle << " - " << kMenuSubtitle << "\n";
    out << "; Generated by the in-game recomp menu. Edit the menu, not this file,\n";
    out << "; unless you know what you are doing. Values are read at startup.\n\n";
    bool first = true;
    for (const Section &s : menu().sections()) {
        std::string rows;
        for (const Item &it : s.items) {
            // Action rows ("Press X": save/load state, exit) are commands, not
            // persisted values.
            if (it.options.size() == 1 && it.options.front() == "Press X") continue;
            rows += it.id + " = " + it.current() + "\n";
        }
        if (rows.empty()) continue;
        if (!first) out << "\n";
        first = false;
        out << '[' << s.id << "]\n" << rows;
    }
    out.flush();
    if (!out) {
        std::fprintf(stderr, "[settings] write failed for %s\n", path_storage().c_str());
        return false;
    }
    std::fprintf(stderr, "[settings] saved %s\n", path_storage().c_str());
    return true;
}

} // namespace ps2::menu
