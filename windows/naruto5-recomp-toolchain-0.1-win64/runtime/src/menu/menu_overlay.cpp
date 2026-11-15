#include "menu_overlay.hpp"

#include "menu_font.hpp"
#include "practice_settings.hpp"
#include "recomp_menu.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ps2::menu {

namespace {

constexpr int kCanvasW = 1920;
constexpr int kCanvasH = 1080;
constexpr int kPanelX = 160;
constexpr int kPanelY = 64;
constexpr int kPanelW = 1600;
constexpr int kPanelH = 952;
constexpr int kPanelR = 26;
constexpr int kTitleH = 112;
constexpr int kLeftW = 430;
constexpr int kRowsTop = 274;
constexpr int kRowH = 66;
constexpr int kDescY = 772;
constexpr int kDescH = 176;
constexpr int kFooterY = 984;

constexpr std::uint32_t rgba(int r, int g, int b, int a = 255) {
    return static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) |
           (static_cast<std::uint32_t>(b) << 16) | (static_cast<std::uint32_t>(a) << 24);
}

constexpr std::uint32_t kBorder = rgba(244, 178, 84);
constexpr std::uint32_t kBorderDark = rgba(160, 92, 28);
constexpr std::uint32_t kOrange = rgba(238, 170, 74);
constexpr std::uint32_t kOrangeDark = rgba(126, 68, 18);
constexpr std::uint32_t kOrangeLight = rgba(255, 192, 104);
constexpr std::uint32_t kCream = rgba(255, 236, 202);
constexpr std::uint32_t kYellow = rgba(255, 224, 96);
constexpr std::uint32_t kYellowSoft = rgba(246, 218, 164);
constexpr std::uint32_t kInk = rgba(42, 14, 4);
constexpr std::uint32_t kPanelTop = rgba(58, 26, 16, 252);
constexpr std::uint32_t kPanelBottom = rgba(20, 8, 4, 255);
constexpr std::uint32_t kTitleTop = rgba(250, 190, 104);
constexpr std::uint32_t kTitleBottom = rgba(211, 131, 53);
constexpr std::uint32_t kSelectTop = rgba(250, 188, 104);
constexpr std::uint32_t kSelectBottom = rgba(219, 136, 58);
constexpr std::uint32_t kRowA = rgba(42, 18, 12, 246);
constexpr std::uint32_t kRowB = rgba(36, 14, 9, 246);
constexpr std::uint32_t kDescBg = rgba(14, 5, 3, 248);

struct Canvas {
    std::vector<std::uint32_t> px = std::vector<std::uint32_t>(kCanvasW * kCanvasH);
    void clear() { std::fill(px.begin(), px.end(), 0u); }
};

void fill_rect(Canvas &c, int x, int y, int w, int h, std::uint32_t color) {
    const int x0 = std::max(x, 0), y0 = std::max(y, 0);
    const int x1 = std::min(x + w, kCanvasW), y1 = std::min(y + h, kCanvasH);
    if (x1 <= x0 || y1 <= y0) return;
    for (int yy = y0; yy < y1; ++yy) {
        const auto row = static_cast<std::size_t>(yy) * kCanvasW;
        std::fill(c.px.begin() + static_cast<std::ptrdiff_t>(row + x0),
                  c.px.begin() + static_cast<std::ptrdiff_t>(row + x1), color);
    }
}

int round_inset(int y, int h, int radius) {
    const int edge = std::min(y, h - 1 - y);
    if (edge >= radius || radius <= 0) return 0;
    const float dx = static_cast<float>(radius - edge);
    const float inner = std::sqrt(std::max(0.0f, static_cast<float>(radius) * radius - dx * dx));
    return radius - static_cast<int>(std::lround(inner));
}

void fill_round_rect(Canvas &c, int x, int y, int w, int h, int radius, std::uint32_t color) {
    radius = std::max(0, std::min({radius, w / 2, h / 2}));
    for (int yy = 0; yy < h; ++yy) {
        const int inset = round_inset(yy, h, radius);
        fill_rect(c, x + inset, y + yy, w - inset * 2, 1, color);
    }
}

void unpack(std::uint32_t v, int &r, int &g, int &b, int &a) {
    r = static_cast<int>(v & 0xFFu);
    g = static_cast<int>((v >> 8) & 0xFFu);
    b = static_cast<int>((v >> 16) & 0xFFu);
    a = static_cast<int>((v >> 24) & 0xFFu);
}

std::uint32_t lerp_color(std::uint32_t a, std::uint32_t b, float t) {
    int ar, ag, ab, aa, br, bg, bb, ba;
    unpack(a, ar, ag, ab, aa);
    unpack(b, br, bg, bb, ba);
    const auto mix = [t](int x, int y) { return static_cast<int>(x + (y - x) * t + 0.5f); };
    return rgba(mix(ar, br), mix(ag, bg), mix(ab, bb), mix(aa, ba));
}

void vgradient_round(Canvas &c, int x, int y, int w, int h, int radius, std::uint32_t top, std::uint32_t bottom) {
    radius = std::max(0, std::min({radius, w / 2, h / 2}));
    for (int yy = 0; yy < h; ++yy) {
        const float t = h > 1 ? static_cast<float>(yy) / static_cast<float>(h - 1) : 0.0f;
        const int inset = round_inset(yy, h, radius);
        fill_rect(c, x + inset, y + yy, w - inset * 2, 1, lerp_color(top, bottom, t));
    }
}

int next_codepoint(const std::string &s, std::size_t &i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        ++i;
        return c;
    }
    if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
        const int cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
        i += 2;
        return cp;
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
        const int cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                       (static_cast<unsigned char>(s[i + 2]) & 0x3F);
        i += 3;
        return cp;
    }
    ++i;
    return '?';
}

const std::uint16_t *glyph_rows(int cp) {
    if (cp >= kFontFirst && cp < kFontFirst + kFontCount)
        return &kFont[static_cast<std::size_t>(cp - kFontFirst) * kFontH];
    if (cp >= kFontLatinFirst && cp < kFontLatinFirst + kFontLatinCount)
        return &kFontLatin[static_cast<std::size_t>(cp - kFontLatinFirst) * kFontH];
    return &kFont[static_cast<std::size_t>('?' - kFontFirst) * kFontH];
}

int text_width(const std::string &text, int scale) {
    std::size_t i = 0, count = 0;
    while (i < text.size()) {
        next_codepoint(text, i);
        ++count;
    }
    return static_cast<int>(count) * kFontW * scale;
}

void text(Canvas &c, int x, int y, const std::string &s, int scale, std::uint32_t color, int max_width = -1) {
    int cx = x;
    const int limit = max_width >= 0 ? x + max_width : kCanvasW;
    std::size_t i = 0;
    while (i < s.size()) {
        const int cp = next_codepoint(s, i);
        if (cx + kFontW * scale > limit) break;
        const std::uint16_t *rows = glyph_rows(cp);
        for (int gy = 0; gy < kFontH; ++gy) {
            const std::uint16_t bits = rows[gy];
            if (!bits) continue;
            for (int gx = 0; gx < kFontW; ++gx)
                if (bits & (1u << gx)) fill_rect(c, cx + gx * scale, y + gy * scale, scale, scale, color);
        }
        cx += kFontW * scale;
    }
}

void text_right(Canvas &c, int right, int y, const std::string &s, int scale, std::uint32_t color) {
    text(c, right - text_width(s, scale), y, s, scale, color);
}

void text_center(Canvas &c, int cx, int y, const std::string &s, int scale, std::uint32_t color) {
    text(c, cx - text_width(s, scale) / 2, y, s, scale, color);
}

std::vector<std::string> wrap(const std::string &text_in, int max_chars, int max_lines) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t i = 0;
    while (i < text_in.size() && static_cast<int>(lines.size()) < max_lines) {
        while (i < text_in.size() && text_in[i] == ' ') ++i;
        std::size_t j = i;
        while (j < text_in.size() && text_in[j] != ' ') ++j;
        const std::string word = text_in.substr(i, j - i);
        if (word.empty()) break;
        if (!line.empty() && static_cast<int>(line.size() + 1 + word.size()) > max_chars) {
            lines.push_back(line);
            line.clear();
            if (static_cast<int>(lines.size()) == max_lines) break;
        }
        if (!line.empty()) line += ' ';
        line += word;
        i = j;
    }
    if (!line.empty() && static_cast<int>(lines.size()) < max_lines) lines.push_back(line);
    if (lines.empty()) lines.push_back("");
    return lines;
}

std::string short_title(const std::string &title) {
    const std::string suffix = " Settings";
    if (title.size() > suffix.size() &&
        title.compare(title.size() - suffix.size(), suffix.size(), suffix) == 0)
        return title.substr(0, title.size() - suffix.size());
    return title;
}

Canvas &canvas_storage() {
    static Canvas canvas;
    return canvas;
}

// Resamples the fixed 1920x1080 canvas into the viewport-aspect texture. A
// transparent background keeps the game visible (hint); the menu passes the
// dark scrim so the whole game window is dimmed.
void blend_canvas(const Canvas &c, std::vector<std::uint32_t> &out, int w, int h, std::uint32_t background) {
    if (w == kCanvasW && h == kCanvasH) {
        out = c.px;
        return;
    }
    out.assign(static_cast<std::size_t>(w) * h, background);
    const float s = std::min(static_cast<float>(w) / kCanvasW, static_cast<float>(h) / kCanvasH);
    const float ox = (static_cast<float>(w) - kCanvasW * s) * 0.5f;
    const float oy = (static_cast<float>(h) - kCanvasH * s) * 0.5f;
    const bool one_to_one = std::fabs(s - 1.0f) < 0.0001f && std::fabs(ox - std::round(ox)) < 0.001f &&
                            std::fabs(oy - std::round(oy)) < 0.001f;

    const auto over = [](std::uint32_t src, std::uint32_t dst) {
        const int sa = static_cast<int>((src >> 24) & 0xFFu);
        if (sa == 0) return dst;
        if (sa == 255) return src;
        int sr, sg, sb, unused, dr, dg, db, da;
        unpack(src, sr, sg, sb, unused);
        unpack(dst, dr, dg, db, da);
        const int ia = 255 - sa;
        const int ra = sa + da * ia / 255;
        const int rr = (sr * sa + dr * da * ia / 255) / std::max(1, ra);
        const int rg = (sg * sa + dg * da * ia / 255) / std::max(1, ra);
        const int rb = (sb * sa + db * da * ia / 255) / std::max(1, ra);
        return rgba(rr, rg, rb, ra);
    };

    if (one_to_one) {
        const int dx = static_cast<int>(std::lround(ox));
        const int dy = static_cast<int>(std::lround(oy));
        for (int y = 0; y < kCanvasH; ++y) {
            const int ty = y + dy;
            if (ty < 0 || ty >= h) continue;
            for (int x = 0; x < kCanvasW; ++x) {
                const int tx = x + dx;
                if (tx < 0 || tx >= w) continue;
                const std::uint32_t src = c.px[static_cast<std::size_t>(y) * kCanvasW + x];
                std::uint32_t &dst = out[static_cast<std::size_t>(ty) * w + tx];
                dst = over(src, dst);
            }
        }
        return;
    }

    for (int y = 0; y < h; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f - oy) / s - 0.5f;
        for (int x = 0; x < w; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f - ox) / s - 0.5f;
            if (fx < -0.5f || fy < -0.5f || fx > kCanvasW - 0.5f || fy > kCanvasH - 0.5f) continue;
            const int x0 = static_cast<int>(std::floor(fx));
            const int y0 = static_cast<int>(std::floor(fy));
            const float tx = fx - x0, ty = fy - y0;
            const auto sample = [&](int sx, int sy) -> std::uint32_t {
                sx = std::clamp(sx, 0, kCanvasW - 1);
                sy = std::clamp(sy, 0, kCanvasH - 1);
                return c.px[static_cast<std::size_t>(sy) * kCanvasW + sx];
            };
            const std::uint32_t a00 = sample(x0, y0), a10 = sample(x0 + 1, y0);
            const std::uint32_t a01 = sample(x0, y0 + 1), a11 = sample(x0 + 1, y0 + 1);
            const std::uint32_t top = lerp_color(a00, a10, tx);
            const std::uint32_t bottom = lerp_color(a01, a11, tx);
            const std::uint32_t src = lerp_color(top, bottom, ty);
            std::uint32_t &dst = out[static_cast<std::size_t>(y) * w + x];
            dst = over(src, dst);
        }
    }
}

void draw_badge(Canvas &c, int x, int y, int w, int h, const std::string &label) {
    fill_round_rect(c, x + 2, y + 3, w, h, 12, rgba(0, 0, 0, 110));
    fill_round_rect(c, x, y, w, h, 12, kOrangeLight);
    fill_round_rect(c, x + 2, y + 2, w - 4, h - 4, 10, rgba(255, 205, 130));
    text(c, x + 20, y + (h - kFontH * 2) / 2, label, 2, kInk);
}

void draw_square_button(Canvas &c, int x, int y, int size, std::uint32_t color) {
    fill_round_rect(c, x, y, size, size, 6, color);
    fill_round_rect(c, x + 7, y + 7, size - 14, size - 14, 3, rgba(24, 9, 4, 238));
}

} // namespace

void render_overlay(std::vector<std::uint32_t> &out, int w, int h) {
    if (w <= 0 || h <= 0) return;
    const Menu &m = menu();
    if (!m.is_open()) {
        out.assign(static_cast<std::size_t>(w) * h, 0u);
        return;
    }

    Canvas &c = canvas_storage();
    c.clear();

    // Rounded panel with a light-orange border and a dark gradient body.
    fill_round_rect(c, kPanelX - 4, kPanelY - 4, kPanelW + 8, kPanelH + 8, kPanelR + 4, kBorder);
    fill_round_rect(c, kPanelX - 1, kPanelY - 1, kPanelW + 2, kPanelH + 2, kPanelR + 1, kBorderDark);
    vgradient_round(c, kPanelX, kPanelY, kPanelW, kPanelH, kPanelR, kPanelTop, kPanelBottom);

    // Rounded light-orange title bar. The title uses dark ink for contrast.
    const int title_x = kPanelX + 8, title_y = kPanelY + 8;
    const int title_w = kPanelW - 16, title_h = kTitleH;
    vgradient_round(c, title_x, title_y, title_w, title_h, 20, kTitleTop, kTitleBottom);
    fill_round_rect(c, title_x + 8, title_y + title_h - 3, title_w - 16, 3, 20, kBorderDark);
    text_center(c, kCanvasW / 2, title_y + 8, kMenuTitle, 3, kInk);
    text_center(c, kCanvasW / 2, title_y + 78, kMenuSubtitle, 1, rgba(65, 26, 8));

    const auto &sections = m.sections();
    const std::size_t sec = m.section_index();
    const std::size_t row = m.row_index();
    const bool multi = sections.size() > 1;
    const bool dirty = m.dirty();

    // "unsaved changes" badge in the title bar.
    if (dirty) draw_badge(c, kPanelX + kPanelW - 352, kPanelY + 26, 312, 50, "N\xC3\x83O SALVO");

    int content_x = kPanelX + 48;
    int content_w = kPanelW - 96;
    if (multi) {
        content_x = kPanelX + kLeftW + 36;
        content_w = kPanelW - kLeftW - 60;
        fill_rect(c, kPanelX + 28, kRowsTop - 8, kLeftW - 56, 2, kOrangeDark);
        text(c, kPanelX + 32, kRowsTop + 4, "SECTIONS", 1, kOrange);
        int tab_y = kRowsTop + 34;
        for (std::size_t i = 0; i < sections.size(); ++i) {
            const bool selected = i == sec;
            const int tx = kPanelX + 26, tw = kLeftW - 52, th = 82;
            if (selected) {
                vgradient_round(c, tx, tab_y, tw, th, 12, kSelectTop, kSelectBottom);
                fill_round_rect(c, tx, tab_y + 8, 6, th - 16, 3, kInk);
            } else {
                fill_round_rect(c, tx, tab_y, tw, th, 12, rgba(36, 14, 9, 238));
                fill_round_rect(c, tx, tab_y + 8, 6, th - 16, 3, kOrangeDark);
            }
            if (selected) text(c, tx + 20, tab_y + 12, ">", 2, kInk);
            text(c, tx + 54, tab_y + 12, short_title(sections[i].title), 2,
                 selected ? kInk : kYellowSoft, tw - 70);
            char counter[32];
            std::snprintf(counter, sizeof counter, "%zu/%zu", i + 1, sections.size());
            text_right(c, tx + tw - 16, tab_y + 50, counter, 1, selected ? kInk : kOrange);
            tab_y += th + 14;
        }
    }

    // Section title and rule.
    text(c, content_x, 208, sections.empty() ? "" : sections[sec].title, 2, kOrange, content_w - 12);
    fill_rect(c, content_x, 250, content_w, 2, kOrangeDark);

    if (!sections.empty() && !sections[sec].items.empty()) {
        const auto &items = sections[sec].items;
        const int n = static_cast<int>(items.size());
        const int rows_y = kRowsTop;
        const int rows_max = std::max(1, (kDescY - rows_y - 18) / kRowH);
        int first = 0;
        if (n > rows_max) first = std::clamp(static_cast<int>(row) - rows_max + 1, 0, n - rows_max);
        if (first > 0) text_right(c, content_x + content_w, 208, "MORE ^", 1, kOrange);
        if (first + rows_max < n) text_right(c, content_x + content_w, rows_y + rows_max * kRowH + 2, "MORE v", 1, kOrange);

        for (int vi = 0; vi < rows_max; ++vi) {
            const int i = first + vi;
            if (i >= n) break;
            const Item &it = items[static_cast<std::size_t>(i)];
            const bool selected = static_cast<std::size_t>(i) == row;
            const bool modified = m.is_modified(sec, static_cast<std::size_t>(i));
            const int y = rows_y + vi * kRowH;
            const int rh = kRowH - 8;
            if (selected) {
                vgradient_round(c, content_x, y, content_w, rh, 9, kSelectTop, kSelectBottom);
                fill_round_rect(c, content_x, y + 8, 6, rh - 16, 3, kInk);
                text(c, content_x + 18, y + 10, ">", 2, kInk);
            } else {
                fill_round_rect(c, content_x, y, content_w, rh, 9, (i & 1) ? kRowA : kRowB);
                fill_rect(c, content_x + 16, y + rh - 1, content_w - 32, 1, rgba(92, 44, 20));
            }
            std::string label = it.label;
            if (modified) label += " *";
            const std::uint32_t label_color = selected ? kInk : (modified ? kYellow : kCream);
            const std::uint32_t value_color = selected ? kInk : (modified ? kYellow : kCream);
            text(c, content_x + 52, y + 10, label, 2, label_color, content_w - 420);
            text_right(c, content_x + content_w - 26, y + 10, it.current(), 2, value_color);
        }

        if (n > rows_max) {
            const int bar_x = content_x + content_w - 8;
            const int bar_y = rows_y;
            const int bar_h = rows_max * kRowH - 8;
            fill_round_rect(c, bar_x, bar_y, 5, bar_h, 3, rgba(60, 24, 12));
            const int thumb_h = std::max(24, bar_h * rows_max / n);
            const int thumb_y = bar_y + (bar_h - thumb_h) * first / std::max(1, n - rows_max);
            fill_round_rect(c, bar_x, thumb_y, 5, thumb_h, 3, kOrange);
        }

        // Rounded description box.
        fill_round_rect(c, content_x, kDescY, content_w, kDescH, 14, kBorderDark);
        fill_round_rect(c, content_x + 2, kDescY + 2, content_w - 4, kDescH - 4, 12, kDescBg);
        text(c, content_x + 22, kDescY + 16, "DESCRIPTION", 1, kOrange);
        const Item &cur = items[std::min(row, items.size() - 1)];
        const int max_chars = (content_w - 52) / kFontW;
        const auto lines = wrap(cur.description, max_chars, 4);
        for (std::size_t i = 0; i < lines.size(); ++i)
            text(c, content_x + 22, kDescY + 48 + static_cast<int>(i) * 28, lines[i], 1, kCream);
    }

    fill_rect(c, kPanelX + 12, kFooterY - 10, kPanelW - 24, 1, rgba(120, 70, 30));
    text_center(c, kCanvasW / 2, kFooterY, kMenuFooter, 1, dirty ? kYellow : rgba(238, 204, 150));

    blend_canvas(c, out, w, h, 0u);
}

void render_hint(std::vector<std::uint32_t> &out, int w, int h, bool mode_select) {
    if (w <= 0 || h <= 0) return;
    Canvas &c = canvas_storage();
    c.clear();
    const int x = mode_select ? 1572 : 48, y = mode_select ? 40 : 960, box_w = 300, box_h = 76;
    fill_round_rect(c, x + 2, y + 4, box_w, box_h, 16, rgba(0, 0, 0, 120));
    fill_round_rect(c, x - 2, y - 2, box_w + 4, box_h + 4, 18, kBorderDark);
    fill_round_rect(c, x, y, box_w, box_h, 16, rgba(22, 9, 5, 228));
    text(c, x + 22, y + 18, "OP\xC3\x87\xC3\x95" "ES", 2, kCream);
    draw_square_button(c, x + 230, y + 20, 36, kOrangeLight);
    blend_canvas(c, out, w, h, 0u);
}

} // namespace ps2::menu
