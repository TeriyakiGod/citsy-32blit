#include "hardware.hpp"

#include "32blit.hpp"

#include <algorithm>
#include <cmath>

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "config.h"
#define CITSY_PICO_HW 1
#endif

namespace {

constexpr float kAdcVref = 3.3f;
constexpr float kDivider = 3.0f;     // 1/3 resistor divider on GPIO26 / ADC0
constexpr float kLipoEmpty = 3.2f;
constexpr float kLipoFull = 4.2f;
constexpr float kUsbVolts = 4.55f;

#ifdef CITSY_PICO_HW
#ifndef CITSY_BATTERY_ADC_PIN
#define CITSY_BATTERY_ADC_PIN 26
#endif
#ifndef CITSY_BATTERY_ADC_CHANNEL
#define CITSY_BATTERY_ADC_CHANNEL 0
#endif

int backlight_pin() {
#ifdef CITSY_BACKLIGHT_PIN
    return CITSY_BACKLIGHT_PIN;
#elif defined(LCD_BACKLIGHT_PIN)
#if defined(LCD_RESET_PIN) && (LCD_BACKLIGHT_PIN == LCD_RESET_PIN)
    return -1;
#else
    return LCD_BACKLIGHT_PIN;
#endif
#else
    return -1;
#endif
}
#endif

int clamp_steps(int steps) {
    return std::clamp(steps, 0, HardwareStatus::kSteps);
}

} // namespace

void HardwareStatus::init() {
#ifdef CITSY_PICO_HW
    adc_init();
    adc_gpio_init(CITSY_BATTERY_ADC_PIN);
    adc_select_input(CITSY_BATTERY_ADC_CHANNEL);
    adc_ready_ = true;

    const int pin = backlight_pin();
    if (pin >= 0) {
        gpio_set_function(static_cast<uint>(pin), GPIO_FUNC_PWM);
        const uint slice = pwm_gpio_to_slice_num(static_cast<uint>(pin));
        pwm_set_wrap(slice, 65535);
        pwm_set_enabled(slice, true);
    }
#endif
    load();
    apply();
    sample_battery();
}

void HardwareStatus::update(uint32_t time_ms) {
    if (last_sample_ms_ == 0 || time_ms - last_sample_ms_ >= kSampleMs) {
        last_sample_ms_ = time_ms;
        sample_battery();
    }
}

void HardwareStatus::set_volume(int steps) {
    volume_ = clamp_steps(steps);
    apply_volume();
}

void HardwareStatus::set_brightness(int steps) {
    brightness_ = clamp_steps(steps);
    apply_brightness();
}

void HardwareStatus::apply() {
    apply_volume();
    apply_brightness();
}

void HardwareStatus::save() const {
    Persist p{};
    p.magic = kMagic;
    p.volume = static_cast<uint8_t>(volume_);
    p.brightness = static_cast<uint8_t>(brightness_);
    blit::write_save(p, kSaveSlot);
}

void HardwareStatus::load() {
    Persist p{};
    if (blit::read_save(p, kSaveSlot) && p.magic == kMagic) {
        volume_ = clamp_steps(p.volume);
        brightness_ = clamp_steps(p.brightness);
    }
}

void HardwareStatus::sample_battery() {
#ifdef CITSY_PICO_HW
    if (!adc_ready_) {
        on_usb_ = true;
        battery_known_ = false;
        battery_percent_ = 100;
        battery_volts_ = 5.0f;
        return;
    }

    adc_select_input(CITSY_BATTERY_ADC_CHANNEL);
    const uint16_t raw = adc_read();
    const float adc_v = (static_cast<float>(raw) / 4095.0f) * kAdcVref;
    battery_volts_ = adc_v * kDivider;

    if (battery_volts_ < 1.5f) {
        // Floating ADC (no divider populated) — treat as unknown / USB.
        on_usb_ = true;
        battery_known_ = false;
        battery_percent_ = 100;
        return;
    }

    battery_known_ = true;
    if (battery_volts_ >= kUsbVolts) {
        on_usb_ = true;
        battery_percent_ = 100;
        return;
    }

    on_usb_ = false;
    const float t = (battery_volts_ - kLipoEmpty) / (kLipoFull - kLipoEmpty);
    battery_percent_ = static_cast<int>(std::clamp(t, 0.0f, 1.0f) * 100.0f + 0.5f);
#else
    on_usb_ = true;
    battery_known_ = false;
    battery_percent_ = 100;
    battery_volts_ = 5.0f;
#endif
}

void HardwareStatus::apply_volume() const {
    const int steps = std::max(0, volume_);
    blit::volume = static_cast<uint16_t>((0xffffu * static_cast<uint32_t>(steps)) / kSteps);
}

void HardwareStatus::apply_brightness() const {
#ifdef CITSY_PICO_HW
    const int pin = backlight_pin();
    if (pin < 0) return;

    const float n = static_cast<float>(std::max(1, brightness_)) / static_cast<float>(kSteps);
    const float gamma = 2.8f;
    const uint16_t pwm = static_cast<uint16_t>(std::pow(n, gamma) * 65535.0f + 0.5f);
    pwm_set_gpio_level(static_cast<uint>(pin), pwm);
#else
    (void)brightness_;
#endif
}
