#include "recomp_menu.hpp"
#include "practice_settings.hpp"

#include <algorithm>
#include <cctype>

namespace ps2::menu {

namespace {
bool same_text(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
} // namespace

Menu::Menu() : sections_(practice_settings_sections()), applied_(sections_) {
#ifndef _WIN32
    // The wgpu-native build on Linux has no DX12. Hide the option instead of
    // letting the player select a backend that cannot create a device.
    for (Section &s : sections_)
        for (Item &it : s.items)
            if (it.id == "graphic_api") {
                auto &o = it.options;
                o.erase(std::remove(o.begin(), o.end(), "DirectX 12"), o.end());
                if (it.value >= static_cast<int>(o.size())) it.value = 0;
            }
    applied_ = sections_;
#endif
}

void Menu::open() {
    if (open_) return;
    open_ = true;
    clamp();
    touch();
}
void Menu::close() {
    if (!open_) return;
    // Closing without pressing Apply discards the staged values: only confirm()
    // writes game_settings.ini.
    if (dirty()) {
        sections_ = applied_;
        clamp();
    }
    open_ = false;
    touch();
    if (on_close) on_close();
}
void Menu::toggle() { open_ ? close() : open(); }

void Menu::clamp() {
    if (sections_.empty()) { section_ = row_ = 0; return; }
    section_ = std::min(section_, sections_.size() - 1);
    row_ = std::min(row_, sections_[section_].items.empty() ? 0 : sections_[section_].items.size() - 1);
}

const Item *Menu::current() const {
    if (sections_.empty()) return nullptr;
    const Section &s = sections_[section_];
    if (s.items.empty()) return nullptr;
    return &s.items[std::min(row_, s.items.size() - 1)];
}

Section *Menu::current_section() {
    if (sections_.empty()) return nullptr;
    return &sections_[section_];
}

void Menu::down() {
    if (sections_.empty()) return;
    if (row_ + 1 < sections_[section_].items.size()) {
        ++row_;
    } else if (section_ + 1 < sections_.size()) {
        ++section_;
        row_ = 0;
    } else {
        section_ = 0;
        row_ = 0;
    }
    touch();
}
void Menu::next_section() {
    if (sections_.empty()) return;
    section_ = (section_ + 1) % sections_.size();
    row_ = 0;
    ++revision_;
}

void Menu::prev_section() {
    if (sections_.empty()) return;
    section_ = (section_ + sections_.size() - 1) % sections_.size();
    row_ = 0;
    ++revision_;
}

void Menu::up() {
    if (sections_.empty()) return;
    if (row_ > 0) {
        --row_;
    } else if (section_ > 0) {
        --section_;
        row_ = sections_[section_].items.empty() ? 0 : sections_[section_].items.size() - 1;
    } else {
        section_ = sections_.size() - 1;
        row_ = sections_[section_].items.empty() ? 0 : sections_[section_].items.size() - 1;
    }
    touch();
}

void Menu::left() {
    Item *it = const_cast<Item *>(current());
    if (!it || it->options.empty()) return;
    it->value = (it->value + static_cast<int>(it->options.size()) - 1) % static_cast<int>(it->options.size());
    touch();
    if (on_change) on_change(*it);
}
void Menu::right() {
    Item *it = const_cast<Item *>(current());
    if (!it || it->options.empty()) return;
    it->value = (it->value + 1) % static_cast<int>(it->options.size());
    touch();
    if (on_change) on_change(*it);
}
void Menu::confirm() {
    const Item *it = current();
    if (!it) return;
    // Action rows ("Press X": exit game, save/load state) execute immediately
    // and must not silently commit unrelated staged display changes.
    const bool action = it->options.size() == 1 && it->options.front() == "Press X";
    if (!action) {
        // Commit the staged values first, then let the host apply everything
        // and write the ini. confirm() is the only place that persists values.
        applied_ = sections_;
        touch();
    }
    if (on_apply) on_apply(*it);
}
void Menu::back() { close(); }

bool Menu::set_in(std::vector<Section> &sections, const std::string &section, const std::string &item,
                  const std::string &value) {
    for (Section &s : sections) {
        if (s.id != section) continue;
        for (Item &it : s.items) {
            if (it.id != item) continue;
            auto found = std::find(it.options.begin(), it.options.end(), value);
            if (found == it.options.end())
                found = std::find_if(it.options.begin(), it.options.end(),
                                     [&](const std::string &option) { return same_text(option, value); });
            if (found != it.options.end()) {
                it.value = static_cast<int>(found - it.options.begin());
            } else {
                it.options.push_back(value);
                it.value = static_cast<int>(it.options.size()) - 1;
            }
            return true;
        }
    }
    return false;
}

bool Menu::set_value(const std::string &section, const std::string &item, const std::string &value) {
    const bool changed = set_in(sections_, section, item, value);
    if (changed) {
        // Values that come from the ini / startup sync are already applied.
        set_in(applied_, section, item, value);
        touch();
    }
    return changed;
}

bool Menu::is_modified(std::size_t section, std::size_t row) const {
    if (section >= sections_.size() || section >= applied_.size()) return false;
    const Section &a = sections_[section];
    const Section &b = applied_[section];
    if (row >= a.items.size() || row >= b.items.size()) return false;
    return a.items[row].value != b.items[row].value;
}

bool Menu::dirty() const {
    if (sections_.size() != applied_.size()) return true;
    for (std::size_t s = 0; s < sections_.size(); ++s) {
        if (sections_[s].items.size() != applied_[s].items.size()) return true;
        for (std::size_t i = 0; i < sections_[s].items.size(); ++i)
            if (sections_[s].items[i].value != applied_[s].items[i].value) return true;
    }
    return false;
}

void Menu::reset() {
    sections_ = practice_settings_sections();
    applied_ = sections_;
    section_ = row_ = 0;
    clamp();
    touch();
}

void Menu::print(std::FILE *out) const {
    std::fprintf(out, "[recomp menu] %s%s\n", open_ ? "open" : "closed", dirty() ? " (unsaved changes)" : "");
    for (std::size_t s = 0; s < sections_.size(); ++s) {
        std::fprintf(out, "  [%s] %s\n", sections_[s].id.c_str(), sections_[s].title.c_str());
        for (std::size_t i = 0; i < sections_[s].items.size(); ++i) {
            const Item &it = sections_[s].items[i];
            std::fprintf(out, "  %c%c %s: %s  (%s)\n", (s == section_ && i == row_) ? '>' : ' ',
                         is_modified(s, i) ? '*' : ' ', it.label.c_str(), it.current().c_str(),
                         it.description.c_str());
        }
    }
}

Menu &menu() {
    static Menu instance;
    return instance;
}

} // namespace ps2::menu
