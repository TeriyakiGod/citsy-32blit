#include "launcher.hpp"
#include "game.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace blit;

namespace {

constexpr Pen kBg{10, 12, 20};
constexpr Pen kPanel{20, 24, 38};
constexpr Pen kPanelHi{32, 40, 62};
constexpr Pen kAccent{86, 196, 140};
constexpr Pen kAccentDim{46, 110, 82};
constexpr Pen kSelect{72, 96, 168};
constexpr Pen kSelectHot{96, 124, 200};
constexpr Pen kText{236, 236, 244};
constexpr Pen kMuted{132, 140, 160};
constexpr Pen kWarn{220, 88, 72};
constexpr Pen kBattery{80, 204, 96};
constexpr Pen kBatteryLow{220, 160, 48};
constexpr Pen kBezel{28, 30, 40};
constexpr Pen kBlack{0, 0, 0};
constexpr Pen kWhite{255, 255, 255};

uint32_t hash32(std::string_view s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

Pen cart_pen(uint32_t seed) {
    const uint8_t r = static_cast<uint8_t>(80 + (seed & 0x7f));
    const uint8_t g = static_cast<uint8_t>(70 + ((seed >> 8) & 0x7f));
    const uint8_t b = static_cast<uint8_t>(90 + ((seed >> 16) & 0x7f));
    return Pen(r, g, b);
}

} // namespace

void LauncherUI::update(uint32_t time_ms) {
    now_ = time_ms;
}

void LauncherUI::notify_move() {
    bounce_until_ = now_ + 90;
    bounce_dir_ = 1;
}

void LauncherUI::notify_press() {
    press_until_ = now_ + 80;
}

Point LauncherUI::origin() const {
    return Point(
        (static_cast<int>(screen.bounds.w) - kSize) / 2,
        (static_cast<int>(screen.bounds.h) - kSize) / 2);
}

Rect LauncherUI::canvas() const {
    const auto o = origin();
    return Rect(o.x, o.y, kSize, kSize);
}

Point LauncherUI::at(int x, int y) const {
    const auto o = origin();
    return Point(o.x + x, o.y + y);
}

Rect LauncherUI::box(int x, int y, int w, int h) const {
    const auto o = origin();
    return Rect(o.x + x, o.y + y, w, h);
}

void LauncherUI::fill(Rect r, Pen pen) {
    screen.pen = pen;
    screen.rectangle(r);
}

void LauncherUI::outline(Rect r, Pen pen) {
    screen.pen = pen;
    screen.rectangle(Rect(r.x, r.y, r.w, 1));
    screen.rectangle(Rect(r.x, r.y + r.h - 1, r.w, 1));
    screen.rectangle(Rect(r.x, r.y, 1, r.h));
    screen.rectangle(Rect(r.x + r.w - 1, r.y, 1, r.h));
}

void LauncherUI::hline(int x, int y, int w, Pen pen) {
    screen.pen = pen;
    screen.rectangle(box(x, y, w, 1));
}

void LauncherUI::text(std::string_view msg, Point p, Pen pen) {
    screen.pen = pen;
    screen.text(msg, minimal_font, p, true);
}

void LauncherUI::draw_bezel() {
    screen.pen = kBlack;
    screen.clear();

    const Rect c = canvas();
    if (screen.bounds.w > kSize || screen.bounds.h > kSize) {
        fill(Rect(c.x - 3, c.y - 3, c.w + 6, c.h + 6), kBezel);
        outline(Rect(c.x - 3, c.y - 3, c.w + 6, c.h + 6), Pen(60, 64, 80));
    }
    fill(c, kBg);
    screen.clip = c;
}

void LauncherUI::draw_battery(Point p, const HardwareStatus& hw) {
    const int pct = hw.battery_percent();
    const Pen body = (!hw.battery_known() || hw.on_usb() || pct > 20) ? kBattery : kBatteryLow;

    // body 13×7, nub 2×3
    screen.pen = kMuted;
    screen.rectangle(Rect(p.x, p.y, 13, 7));
    screen.pen = kBg;
    screen.rectangle(Rect(p.x + 1, p.y + 1, 11, 5));
    screen.pen = kMuted;
    screen.rectangle(Rect(p.x + 13, p.y + 2, 2, 3));

    const int inner = std::max(0, std::min(11, (pct * 11 + 50) / 100));
    if (inner > 0) {
        screen.pen = body;
        screen.rectangle(Rect(p.x + 1, p.y + 1, inner, 5));
    }

    if (hw.on_usb() || !hw.battery_known()) {
        // tiny lightning in the nub area
        screen.pen = kWhite;
        screen.pixel(Point(p.x + 16, p.y + 1));
        screen.pixel(Point(p.x + 16, p.y + 2));
        screen.pixel(Point(p.x + 17, p.y + 3));
        screen.pixel(Point(p.x + 16, p.y + 4));
        screen.pixel(Point(p.x + 16, p.y + 5));
    }
}

void LauncherUI::draw_status_bar(std::string_view title, const HardwareStatus& hw, uint32_t time_ms) {
    fill(box(0, 0, kSize, kStatusH), kPanel);
    hline(0, kStatusH, kSize, kAccentDim);

    char title_buf[12];
    const std::size_t n = std::min(title.size(), sizeof(title_buf) - 1);
    std::memcpy(title_buf, title.data(), n);
    title_buf[n] = '\0';
    text(title_buf, at(3, 3), kText);

    const uint32_t sec = (time_ms / 1000) % 3600;
    char clock[8];
    std::snprintf(clock, sizeof(clock), "%02u:%02u",
                  static_cast<unsigned>(sec / 60),
                  static_cast<unsigned>(sec % 60));
    text(clock, at(52, 3), kMuted);

    draw_battery(at(107, 3), hw);
}

void LauncherUI::draw_cart_icon(Point p, uint32_t seed, bool selected) {
    const Pen fill_c = selected ? cart_pen(seed) : Pen(60, 66, 82);
    screen.pen = fill_c;
    screen.rectangle(Rect(p.x, p.y, 12, 12));
    screen.pen = kBlack;
    screen.rectangle(Rect(p.x + 2, p.y + 2, 8, 5));
    screen.pen = selected ? kAccent : kMuted;
    screen.rectangle(Rect(p.x + 3, p.y + 3, 6, 3));
    screen.pen = kPanel;
    screen.rectangle(Rect(p.x + 2, p.y + 8, 8, 2));
}

void LauncherUI::draw_gear_icon(Point p, bool selected) {
    screen.pen = selected ? kAccent : kMuted;
    screen.rectangle(Rect(p.x + 4, p.y, 4, 12));
    screen.rectangle(Rect(p.x, p.y + 4, 12, 4));
    screen.pen = kBg;
    screen.rectangle(Rect(p.x + 5, p.y + 5, 2, 2));
}

void LauncherUI::draw_footer(std::string_view hint) {
    fill(box(0, kSize - kFooterH, kSize, kFooterH), kPanel);
    hline(0, kSize - kFooterH, kSize, kAccentDim);
    text(hint, at(3, kSize - kFooterH + 3), kMuted);
}

void LauncherUI::draw_main_menu(const GameEntry* games, int game_count, int selected, uint32_t /*time_ms*/) {
    const int rows = game_count + 1; // last row is Settings
    const int list_top = kStatusH + 2;
    const int list_h = kSize - kStatusH - kFooterH - 3;
    const int row_h = 16;
    const int visible = std::max(1, list_h / row_h);

    int start = 0;
    if (selected >= visible) {
        start = selected - visible + 1;
    }

    const bool bouncing = now_ < bounce_until_;
    const bool pressed = now_ < press_until_;

    for (int i = 0; i < visible && start + i < rows; ++i) {
        const int idx = start + i;
        const int y = list_top + i * row_h + ((idx == selected && bouncing) ? bounce_dir_ : 0);
        const bool is_sel = idx == selected;
        const bool is_settings = idx == game_count;

        if (is_sel) {
            fill(box(2, y, kSize - 4, row_h - 1), pressed ? kSelectHot : kSelect);
        }

        const Point icon = at(5, y + 2);
        if (is_settings) {
            draw_gear_icon(icon, is_sel);
            text("Settings", at(20, y + 4), is_sel ? kWhite : kMuted);
        } else {
            const auto& g = games[idx];
            draw_cart_icon(icon, hash32(g.label), is_sel);
            screen.pen = is_sel ? kWhite : kText;
            screen.text(g.label, minimal_font, box(20, y + 1, 104, 8), true);

            char meta[16];
            const int cur = std::min(99, idx + 1);
            const int tot = std::min(99, std::max(1, game_count));
            std::snprintf(meta, sizeof(meta), "%02d/%02d %s",
                          cur, tot, g.bundled ? "ROM" : "SD");
            text(meta, at(20, y + 8), is_sel ? kAccent : kMuted);
        }
    }

    if (rows == 1 && game_count == 0) {
        text("No .bitsy files", at(20, list_top + 22), kMuted);
        text("drop onto SD", at(20, list_top + 32), kMuted);
    }

    draw_footer("A play   X set   ^v");
}

void LauncherUI::draw_slider(int y, std::string_view label, int steps, bool selected) {
    if (selected) {
        fill(box(2, y - 1, kSize - 4, 18), kSelect);
    }
    text(label, at(6, y + 1), selected ? kWhite : kText);

    const int bar_x = 6;
    const int bar_y = y + 10;
    const int bar_w = 96;
    fill(box(bar_x, bar_y, bar_w, 5), kPanelHi);
    const int fill_w = (bar_w * steps) / HardwareStatus::kSteps;
    if (fill_w > 0) {
        fill(box(bar_x, bar_y, fill_w, 5), selected ? kAccent : kAccentDim);
    }

    char val[8];
    std::snprintf(val, sizeof(val), "%d", std::clamp(steps, 0, HardwareStatus::kSteps));
    text(val, at(108, y + 4), selected ? kWhite : kMuted);
}

void LauncherUI::draw_settings(const HardwareStatus& hw, int selected_row) {
    text("sound / screen", at(6, kStatusH + 4), kMuted);
    draw_slider(kStatusH + 16, "Volume", hw.volume(), selected_row == 0);
    draw_slider(kStatusH + 38, "Bright", hw.brightness(), selected_row == 1);

    char volts[16];
    if (hw.on_usb() || !hw.battery_known()) {
        std::snprintf(volts, sizeof(volts), "power  USB");
    } else {
        std::snprintf(volts, sizeof(volts), "batt  %d%%", hw.battery_percent());
    }
    text(volts, at(6, kStatusH + 64), kMuted);

    draw_footer("<> adj  B save/back");
}

void LauncherUI::draw_pause_overlay(int selected, std::string_view game_title) {
    screen.clip = canvas();
    fill(box(0, 0, kSize, kSize), Pen(0, 0, 0, 130));

    const Rect panel = box(14, 28, 100, 72);
    fill(panel, kPanel);
    outline(panel, kAccent);

    char title_buf[14];
    const std::size_t n = std::min(game_title.size(), sizeof(title_buf) - 1);
    std::memcpy(title_buf, game_title.data(), n);
    title_buf[n] = '\0';
    text(title_buf[0] ? title_buf : "paused", at(20, 32), kMuted);

    static constexpr const char* kItems[] = {"Resume", "Restart", "Exit"};
    const bool pressed = now_ < press_until_;
    for (int i = 0; i < 3; ++i) {
        const int y = 46 + i * 16;
        if (i == selected) {
            fill(box(20, y - 1, 88, 14), pressed ? kSelectHot : kSelect);
            text(kItems[i], at(28, y + 2), kWhite);
        } else {
            text(kItems[i], at(28, y + 2), kText);
        }
    }
}

void LauncherUI::draw_error(std::string_view message) {
    fill(box(0, 0, kSize, kSize), Pen(40, 10, 12));
    fill(box(0, 0, kSize, kStatusH), kPanel);
    text("ERROR", at(3, 3), kWarn);
    screen.pen = kWhite;
    screen.text(message, minimal_font, box(6, 18, kSize - 12, kSize - 36), true);
    draw_footer("A / B / MENU back");
}

void LauncherUI::draw_brightness_veil(int brightness_steps) {
    const int steps = std::clamp(brightness_steps, 0, HardwareStatus::kSteps);
    if (steps >= HardwareStatus::kSteps) return;
    // Keep a floor so the UI never goes fully black (user could not recover).
    const int dim = ((HardwareStatus::kSteps - steps) * 180) / HardwareStatus::kSteps;
    if (dim <= 0) return;
    screen.pen = Pen(0, 0, 0, static_cast<uint8_t>(dim));
    screen.rectangle(canvas());
}
