#pragma once

#include "32blit.hpp"
#include <citsy/host.hpp>

#include <array>
#include <cstdint>

/// 32blit implementation of citsy::Host.
///
/// Engine::update() calls present() with logical buffers; this class copies
/// them and later blits to the 32blit framebuffer in draw().
class CitsyBlitHost : public citsy::Host {
public:
    void poll_input();
    void set_delta(double dt_ms);

    void draw() const;
    void play_audio();
    void stop_audio();
    void set_menu_captured(bool captured) { menu_captured_ = captured; }

    [[nodiscard]] double delta_time_ms() const override;
    [[nodiscard]] bool button(citsy::Button code) const override;
    void log(std::string_view message) override;

    void present(
        citsy::GraphicsMode gfx_mode,
        citsy::TextMode txt_mode,
        std::span<const citsy::Color> palette,
        std::span<const std::uint8_t> video,
        std::span<const std::uint8_t> map1,
        std::span<const std::uint8_t> map2,
        citsy::TextboxView textbox,
        citsy::SoundChannel sound1,
        citsy::SoundChannel sound2
    ) override;

private:
    void apply_channel(int index, const citsy::SoundChannel& ch);

    double dt_ms_ = 16.667;
    bool keys_[6] = {};
    bool has_frame_ = false;
    bool menu_captured_ = true;

    std::array<std::uint8_t, citsy::kVideoSize * citsy::kVideoSize> video_{};
    blit::Pen pens_[256]{};
    int palette_len_ = 3;

    citsy::SoundChannel sound1_{};
    citsy::SoundChannel sound2_{};
    bool sound1_was_active_ = false;
    bool sound2_was_active_ = false;
};
