#pragma once
// game_settings.ini persistence and startup application for the recomp menu.
//
// The menu data itself lives in menu/practice_settings.json (labels, options,
// descriptions) and every value is saved to game_settings.ini in the game
// folder (the process working directory) as a plain, editable ini file.
#include <string>

namespace ps2::menu {

// Reads game_settings.ini and applies the [recomp] display values to the
// runtime (resolution / PS2_PRESENT_AR / PS2_SCALE / PS2_HUD_SCALE / vsync).
// Call this before parsing the command line so explicit CLI options win.
bool settings_load();

// After the command line has been parsed: make the menu show the values that
// actually took effect (so --resolution 4k is reflected in the menu).
void settings_sync_effective();

// After the window is open: remember the real drawable size and display mode.
void settings_sync_runtime(unsigned width, unsigned height, bool fullscreen, bool borderless);

// Writes every menu value to game_settings.ini.
bool settings_save();
// Creates game_settings.ini with the current (effective) values if it does not
// exist yet, so the file is always present in the game folder.
void settings_ensure_file();

// Path used for the ini (PS2_SETTINGS overrides the default file name).
const std::string &settings_path();
const std::string &settings_screen_mode();

} // namespace ps2::menu
