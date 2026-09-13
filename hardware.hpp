#pragma once

#include <cstdint>

/// Persistent launcher preferences and RP2350 telemetry.
///
/// Volume is applied through `blit::volume`. Brightness uses GPIO PWM when a
/// dedicated backlight pin is available, and a software veil otherwise (OLED
/// panels such as the 128×128 SSD1351 have no PWM backlight).
class HardwareStatus {
public:
    static constexpr int kSteps = 10;
    static constexpr uint8_t kDefaultVolume = 8;
    static constexpr uint8_t kDefaultBrightness = 10;

    void init();
    void update(uint32_t time_ms);

    void set_volume(int steps);
    void set_brightness(int steps);
    void apply();
    void save() const;

    [[nodiscard]] int volume() const { return volume_; }
    [[nodiscard]] int brightness() const { return brightness_; }

    /// 0–100. USB / unknown supply reports 100.
    [[nodiscard]] int battery_percent() const { return battery_percent_; }
    [[nodiscard]] float battery_volts() const { return battery_volts_; }
    [[nodiscard]] bool on_usb() const { return on_usb_; }
    [[nodiscard]] bool battery_known() const { return battery_known_; }

private:
    static constexpr uint32_t kMagic = 0x43535431; // 'CST1'
    static constexpr int kSaveSlot = 0;
    static constexpr uint32_t kSampleMs = 400;

    struct Persist {
        uint32_t magic = 0;
        uint8_t volume = kDefaultVolume;
        uint8_t brightness = kDefaultBrightness;
        uint8_t pad[2] = {};
    };

    void load();
    void sample_battery();
    void apply_volume() const;
    void apply_brightness() const;

    int volume_ = kDefaultVolume;
    int brightness_ = kDefaultBrightness;
    int battery_percent_ = 100;
    float battery_volts_ = 4.2f;
    bool on_usb_ = true;
    bool battery_known_ = false;
    bool adc_ready_ = false;
    uint32_t last_sample_ms_ = 0;
};
