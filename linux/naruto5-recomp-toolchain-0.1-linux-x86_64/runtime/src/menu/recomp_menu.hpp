#pragma once
// Editable recomp settings menu, based on the game's Practice Settings screen.
// The data lives in runtime/src/menu/practice_settings.hpp (generated from
// menu/practice_settings.json); this file only contains navigation/state.
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace ps2::menu {

struct Item {
    std::string id;
    std::string label;
    std::vector<std::string> options;
    int value = 0;
    std::string description;

    const std::string &current() const {
        static const std::string empty;
        return value >= 0 && value < static_cast<int>(options.size()) ? options[static_cast<std::size_t>(value)] : empty;
    }
};

struct Section {
    std::string id;
    std::string title;
    std::vector<Item> items;
};

class Menu {
public:
    Menu();

    void toggle();
    void open();
    void close();
    bool is_open() const { return open_; }

    void up();
    void down();
    void next_section();
    void prev_section();
    void left();
    void right();
    void confirm();
    void back();
    void reset();

    // Restores one value from game_settings.ini (or any other ini-like source).
    // Programmatic values are considered already applied, so they never light
    // the "unsaved changes" indicator.
    bool set_value(const std::string &section, const std::string &item, const std::string &value);

    const Item *current() const;
    Section *current_section();
    std::vector<Section> &sections() { return sections_; }
    const std::vector<Section> &sections() const { return sections_; }

    std::size_t section_index() const { return section_; }
    std::size_t row_index() const { return row_; }
    // Bumped whenever anything that changes the drawing changes, so the GPU
    // overlay can cache the menu texture between frames.
    std::uint64_t revision() const { return revision_; }

    // Staged values differ from the last applied (or loaded) values. The menu
    // only writes game_settings.ini when confirm/apply is pressed.
    bool dirty() const;
    bool is_modified(std::size_t section, std::size_t row) const;

    // Called after a value changes, so the host can preview it if desired. The
    // shipped host applies everything only in on_apply (when X is pressed).
    std::function<void(const Item &)> on_change;
    // Called by confirm(): the host applies all staged values and saves the ini.
    std::function<void(const Item &)> on_apply;
    // Called after close(); unsaved values have already been discarded.
    std::function<void()> on_close;

    void print(std::FILE *out = stderr) const;

private:
    void clamp();
    void touch() { ++revision_; }
    static bool set_in(std::vector<Section> &sections, const std::string &section, const std::string &item,
                       const std::string &value);
    std::vector<Section> sections_;
    std::vector<Section> applied_;
    std::size_t section_ = 0;
    std::size_t row_ = 0;
    bool open_ = false;
    std::uint64_t revision_ = 1;
};

Menu &menu();

} // namespace ps2::menu
