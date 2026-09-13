#pragma once

#include "32blit.hpp"
#include "hardware.hpp"

#include <cstdint>
#include <string_view>

struct GameEntry;

/// Pixel-art launcher chrome for a 128×128 canvas.
///
/// On larger desktop framebuffers the 128×128 UI is letterboxed in the centre.
/// All coordinates passed to blit primitives are offset by `origin()`.
class LauncherUI {
public:
    static constexpr int kSize = 128;
    static constexpr int kStatusH = 13;
    static constexpr int kFooterH = 12;

    void update(uint32_t time_ms);
    void notify_move();
    void notify_press();

    void draw_bezel();
    void draw_status_bar(std::string_view title, const HardwareStatus& hw, uint32_t time_ms);
    void draw_main_menu(const GameEntry* games, int game_count, int selected, uint32_t time_ms);
    void draw_settings(const HardwareStatus& hw, int selected_row);
    void draw_pause_overlay(int selected, std::string_view game_title);
    void draw_error(std::string_view message);
    void draw_footer(std::string_view hint);
    void draw_brightness_veil(int brightness_steps);

    [[nodiscard]] blit::Point origin() const;
    [[nodiscard]] blit::Rect canvas() const;

private:
    void fill(blit::Rect r, blit::Pen pen);
    void outline(blit::Rect r, blit::Pen pen);
    void hline(int x, int y, int w, blit::Pen pen);
    void text(std::string_view msg, blit::Point p, blit::Pen pen);
    void draw_battery(blit::Point p, const HardwareStatus& hw);
    void draw_cart_icon(blit::Point p, uint32_t seed, bool selected);
    void draw_gear_icon(blit::Point p, bool selected);
    void draw_slider(int y, std::string_view label, int steps, bool selected);

    [[nodiscard]] blit::Point at(int x, int y) const;
    [[nodiscard]] blit::Rect box(int x, int y, int w, int h) const;

    uint32_t bounce_until_ = 0;
    uint32_t press_until_ = 0;
    uint32_t now_ = 0;
    int bounce_dir_ = 0;
};
