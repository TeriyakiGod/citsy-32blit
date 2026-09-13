#include "host.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace {

constexpr int kVideo = citsy::kVideoSize;

[[nodiscard]] uint16_t pulse_width(citsy::PulseWave pulse) {
    switch (pulse) {
        case citsy::PulseWave::Eighth:  return 0x1fff;
        case citsy::PulseWave::Quarter: return 0x3fff;
        case citsy::PulseWave::Half:
        default:                        return 0x7fff;
    }
}

} // namespace

void CitsyBlitHost::poll_input() {
    using blit::Button;
    keys_[static_cast<int>(citsy::Button::Up)]    = blit::buttons & Button::DPAD_UP;
    keys_[static_cast<int>(citsy::Button::Down)]  = blit::buttons & Button::DPAD_DOWN;
    keys_[static_cast<int>(citsy::Button::Left)]  = blit::buttons & Button::DPAD_LEFT;
    keys_[static_cast<int>(citsy::Button::Right)] = blit::buttons & Button::DPAD_RIGHT;
    keys_[static_cast<int>(citsy::Button::Ok)]    = blit::buttons & (Button::A | Button::B);
    // MENU is the system overlay key unless the launcher explicitly releases it.
    keys_[static_cast<int>(citsy::Button::Menu)]  = !menu_captured_ && (blit::buttons & Button::MENU);
}

void CitsyBlitHost::set_delta(double dt_ms) {
    dt_ms_ = dt_ms > 0 ? dt_ms : 16.667;
}

double CitsyBlitHost::delta_time_ms() const {
    return dt_ms_;
}

bool CitsyBlitHost::button(citsy::Button code) const {
    const int i = static_cast<int>(code);
    if (i < 0 || i >= 6) return false;
    return keys_[i];
}

void CitsyBlitHost::log(std::string_view message) {
    blit::debug(std::string(message));
}

void CitsyBlitHost::present(
    citsy::GraphicsMode /*gfx_mode*/,
    citsy::TextMode /*txt_mode*/,
    std::span<const citsy::Color> palette,
    std::span<const std::uint8_t> video,
    std::span<const std::uint8_t> /*map1*/,
    std::span<const std::uint8_t> /*map2*/,
    citsy::TextboxView textbox,
    citsy::SoundChannel sound1,
    citsy::SoundChannel sound2
) {
    // compose_room() always fills the 128×128 video buffer, including Map mode.
    const auto n = std::min(video.size(), video_.size());
    if (n > 0) {
        std::memcpy(video_.data(), video.data(), n);
    }
    if (n < video_.size()) {
        std::memset(video_.data() + n, 0, video_.size() - n);
    }

    palette_len_ = static_cast<int>(std::min(palette.size(), size_t{256}));
    for (int i = 0; i < 256; ++i) {
        pens_[i] = blit::Pen(0, 0, 0);
    }
    for (int i = 0; i < palette_len_; ++i) {
        const auto& c = palette[static_cast<size_t>(i)];
        pens_[i] = blit::Pen(c.r, c.g, c.b);
    }

    if (textbox.visible && !textbox.pixels.empty() &&
        textbox.width > 0 && textbox.height > 0) {
        const int tw = textbox.width;
        const int th = textbox.height;
        for (int row = 0; row < th; ++row) {
            const int dy = textbox.y + row;
            if (dy < 0 || dy >= kVideo) continue;
            for (int col = 0; col < tw; ++col) {
                const int dx = textbox.x + col;
                if (dx < 0 || dx >= kVideo) continue;
                const size_t si = static_cast<size_t>(row * tw + col);
                if (si >= textbox.pixels.size()) continue;
                video_[static_cast<size_t>(dy * kVideo + dx)] = textbox.pixels[si];
            }
        }
    }

    sound1_ = sound1;
    sound2_ = sound2;
    has_frame_ = true;
}

void CitsyBlitHost::draw() const {
    blit::screen.pen = pens_[0];
    blit::screen.clear();
    if (!has_frame_) return;

    auto* pixels = const_cast<uint8_t*>(video_.data());
    blit::Surface src(pixels, blit::PixelFormat::P, blit::Size(kVideo, kVideo));
    src.palette = const_cast<blit::Pen*>(pens_);

    const int scale = std::max(1, static_cast<int>(std::min(
        blit::screen.bounds.w / kVideo,
        blit::screen.bounds.h / kVideo)));
    const int dw = kVideo * scale;
    const int dh = kVideo * scale;
    const int dx = (static_cast<int>(blit::screen.bounds.w) - dw) / 2;
    const int dy = (static_cast<int>(blit::screen.bounds.h) - dh) / 2;

    blit::screen.stretch_blit(
        &src,
        blit::Rect(0, 0, kVideo, kVideo),
        blit::Rect(dx, dy, dw, dh));
}

void CitsyBlitHost::apply_channel(int index, const citsy::SoundChannel& ch) {
    auto& out = blit::channels[index];
    bool& was_active = (index == 0) ? sound1_was_active_ : sound2_was_active_;

    if (!ch.active || ch.frequency_hz <= 0) {
        if (was_active) {
            out.off();
            was_active = false;
        }
        return;
    }

    out.waveforms = blit::Waveform::SQUARE;
    out.frequency = static_cast<uint16_t>(std::clamp(ch.frequency_hz, 1, 65535));
    out.volume = static_cast<uint16_t>(
        std::clamp(ch.volume, 0.0f, 1.0f) * 0xffff);
    out.pulse_width = pulse_width(ch.pulse);
    out.attack_ms = 2;
    out.decay_ms = 1;
    out.sustain = 0xffff;
    out.release_ms = 2;

    if (!was_active || out.adsr_phase == blit::ADSRPhase::OFF) {
        out.trigger_sustain();
    }
    was_active = true;
}

void CitsyBlitHost::play_audio() {
    apply_channel(0, sound1_);
    apply_channel(1, sound2_);
}

void CitsyBlitHost::stop_audio() {
    blit::channels[0].off();
    blit::channels[1].off();
    sound1_was_active_ = false;
    sound2_was_active_ = false;
}
