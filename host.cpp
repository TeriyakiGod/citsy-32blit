#include "host.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico.h"
#define CITSY_NOT_IN_FLASH __not_in_flash_func
#else
#define CITSY_NOT_IN_FLASH
#endif

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

// Match 32blit `pack_rgb565` (R in the low bits, B in the high bits).
[[nodiscard]] constexpr uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
}

void CITSY_NOT_IN_FLASH(blit_indices_rgb565)(
    const uint8_t* src,
    uint16_t* dest,
    const uint16_t* lut,
    int count
) {
    int i = 0;
    // Unroll a little so the Cortex-M33 can dual-issue the LUT loads.
    for (; i + 4 <= count; i += 4) {
        dest[i]     = lut[src[i]];
        dest[i + 1] = lut[src[i + 1]];
        dest[i + 2] = lut[src[i + 2]];
        dest[i + 3] = lut[src[i + 3]];
    }
    for (; i < count; ++i) {
        dest[i] = lut[src[i]];
    }
}

void overlay_textbox(
    std::array<std::uint8_t, kVideo * kVideo>& video,
    const citsy::TextboxView& textbox
) {
    if (!textbox.visible || textbox.pixels.empty() ||
        textbox.width <= 0 || textbox.height <= 0) {
        return;
    }
    const int tw = textbox.width;
    const int th = textbox.height;
    const int x0 = std::max(0, textbox.x);
    const int y0 = std::max(0, textbox.y);
    const int x1 = std::min(kVideo, textbox.x + tw);
    const int y1 = std::min(kVideo, textbox.y + th);
    const int copy_w = x1 - x0;
    if (copy_w <= 0 || y1 <= y0) return;

    const int src_x = x0 - textbox.x;
    const int src_y = y0 - textbox.y;
    const auto* src_base = textbox.pixels.data();
    const std::size_t src_n = textbox.pixels.size();

    for (int row = 0; row < y1 - y0; ++row) {
        const std::size_t si =
            static_cast<std::size_t>((src_y + row) * tw + src_x);
        if (si >= src_n) break;
        const std::size_t n = std::min(
            static_cast<std::size_t>(copy_w), src_n - si);
        std::memcpy(
            &video[static_cast<std::size_t>((y0 + row) * kVideo + x0)],
            src_base + si,
            n);
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

    overlay_textbox(video_, textbox);

    lut565_.fill(0);
    palette_len_ = static_cast<int>(std::min(palette.size(), size_t{256}));
    for (int i = 0; i < palette_len_; ++i) {
        const auto& c = palette[static_cast<size_t>(i)];
        pens_[i] = blit::Pen(c.r, c.g, c.b);
        lut565_[static_cast<size_t>(i)] = pack_rgb565(c.r, c.g, c.b);
    }
    for (int i = palette_len_; i < 256; ++i) {
        pens_[i] = blit::Pen(0, 0, 0);
    }

    sound1_ = sound1;
    sound2_ = sound2;
    has_frame_ = true;
}

void CitsyBlitHost::draw() const {
    if (!has_frame_) {
        blit::screen.pen = pens_[0];
        blit::screen.clear();
        return;
    }

    const int sw = static_cast<int>(blit::screen.bounds.w);
    const int sh = static_cast<int>(blit::screen.bounds.h);

    // Device OLED: 128×128 RGB565. Expand palette indices in one pass into
    // the HAL's off-screen RGB565 framebuffer (double-buffered on RP2350).
    // The SSD1351 driver DMA's that whole page in one WRITE_RAM burst.
    // stretch_blit() would call get_pixel+pbf per pixel
    // (function pointer + Pen reconstruct + RGB565 pack) — ~10–20× slower.
    if (blit::screen.format == blit::PixelFormat::RGB565 &&
        blit::screen.data != nullptr &&
        sw >= kVideo && sh >= kVideo) {
        const int scale = std::max(1, std::min(sw / kVideo, sh / kVideo));
        auto* dest = reinterpret_cast<uint16_t*>(blit::screen.data);

        if (scale == 1 && sw == kVideo && sh == kVideo) {
            blit_indices_rgb565(video_.data(), dest, lut565_.data(), kVideo * kVideo);
            return;
        }

        const int dw = kVideo * scale;
        const int dh = kVideo * scale;
        const int dx = (sw - dw) / 2;
        const int dy = (sh - dh) / 2;

        if (dx != 0 || dy != 0 || dw != sw || dh != sh) {
            blit::screen.pen = blit::Pen(0, 0, 0);
            blit::screen.clear();
        }

        for (int y = 0; y < kVideo; ++y) {
            const uint8_t* src = video_.data() + y * kVideo;
            for (int sy = 0; sy < scale; ++sy) {
                uint16_t* row = dest + (dy + y * scale + sy) * sw + dx;
                if (scale == 1) {
                    blit_indices_rgb565(src, row, lut565_.data(), kVideo);
                } else {
                    for (int x = 0; x < kVideo; ++x) {
                        const uint16_t p = lut565_[src[x]];
                        for (int sx = 0; sx < scale; ++sx) {
                            *row++ = p;
                        }
                    }
                }
            }
        }
        return;
    }

    blit::screen.pen = pens_[0];
    blit::screen.clear();

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
    out.filter_enable = false;

    if (!was_active || out.adsr_phase == blit::ADSRPhase::OFF
        || out.adsr_phase == blit::ADSRPhase::RELEASE) {
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
