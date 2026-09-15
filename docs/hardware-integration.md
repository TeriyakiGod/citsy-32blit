# Hardware & Pico SDK integration

citsy-32blit never talks to the OLED, ADC, or PWM from the engine. The chili-chip 32blit HAL owns the display and audio; this player samples battery voltage and applies launcher settings on top of that HAL.

All drawing still goes through `blit::screen`, `blit::Pen`, `blit::Rect`, and `blit::Point`. Pico SDK calls (`adc_read()`, `pwm_set_gpio_level()`) live only in `hardware.cpp`, compiled when `PICO_ON_DEVICE` is set.

## Display driver

The chili-chip VGC board selects the **SSD1351** DBI/SPI driver:

```cmake
# 32blit-sdk/32blit-pico/board/chilichip_vgc/config.cmake
blit_driver(display dbi_ssd1351)
```

Board pins (`32blit-sdk/32blit-pico/board/chilichip_vgc/config.h`):

| Signal | GPIO | Role |
|---|---|---|
| `LCD_SCK_PIN` | 18 | SPI clock |
| `LCD_MOSI_PIN` | 19 | SPI MOSI (panel data) |
| `LCD_CS_PIN` | 17 | Chip select |
| `LCD_DC_PIN` | 21 | Data / command |
| `LCD_RESET_PIN` | 20 | Panel reset |
| Resolution | 128×128 | `DISPLAY_WIDTH` / `DISPLAY_HEIGHT` |
| SPI clock cap | **20 MHz** | `LCD_MAX_CLOCK` (datasheet serial-write cycle 50 ns; do not 30–40 MHz) |
| Rotation | 2 | `LCD_ROTATION` (180°) |

There is **no** dedicated backlight pin on this OLED. `LCD_BACKLIGHT_PIN` is left undefined in the board header (the SDK default of GPIO 20 would collide with reset, so the player refuses to PWM that pin).

### SPI timing and transfer

The chili-chip HAL (`32blit-pico/display/dbi_ssd1351.cpp`) bit-bangs the SSD1351 protocol through a PIO SPI program. This player does not override that driver.

PIO SPI program (`dbi-spi.pio`):

1. **Command vs data** — `LCD_DC_PIN` is low for command bytes (`SET_COLUMN` `0x15`, `SET_ROW` `0x75`, `WRITE_RAM` `0x5C`, …) and high for pixel payload.
2. **Clock** — fractional PIO divider so SCK is `clk_sys / (clkdiv × 2)` and equals `LCD_MAX_CLOCK` (**20 MHz**, SSD1351 serial-write cycle 50 ns). Integer `ceil()` used to drop this to ~17.9 MHz at 250 MHz sysclk. **Do not** raise the cap to 30–40 MHz.
3. **Frame push** — after `render()`, the 128×128 RGB565 page is DMA’d as 16-bit words with CS held. The driver re-issues column/row + `WRITE_RAM` every frame and waits for PIO TX stall so the GRAM pointer cannot drift.
4. **Double buffer** — RP2350 `DOUBLE_BUFFERED_HIRES` allocates two 32 KiB pages. Game code draws the back buffer while the previous page is on the wire.
5. **TE / VSYNC** — SSD1351 modules (Waveshare 1.5") do not break out a tearing pin. If a later board wires `LCD_TE_PIN` or `LCD_VSYNC_PIN`, the HAL waits on the rising edge before DMA.

A 128×128 RGB565 frame is 32 768 bytes ≈ **13.1 ms** at 20 MHz, so the bus is not the 50 FPS cap (`update_display` still ticks at 20 ms without TE).

The player never issues these commands itself. It writes `blit::screen`; the HAL presents that buffer during `update_display()`. Games may call `ssd1351_set_master_contrast()` (`display.hpp`) to set command `0xC7`.

### Flicker, tearing, and rolling lines

Phone cameras and some viewers see **horizontal bars rolling on black** because the SSD1351 multiplexes rows with PWM. That is not SPI noise.

| Bottleneck | Previous HAL | chili-chip HAL now |
|---|---|---|
| `0xB3` CLOCK_DIV | `0xF1` (max osc, **÷2**) | `0xF0` (max osc, **÷1**) — ~2× panel refresh. Override with `-DSSD1351_CLOCK_DIV=0xF1` if a panel cannot tolerate /1 |
| Init gaps | no `0xB2` enhance, `0xBB` precharge voltage, `0xB9` linear LUT | CircuitPython/Newhaven values |
| SPI clkdiv | `ceil(sys / 2f)` | fractional, hits 20 MHz |
| GRAM pointer | left in `WRITE_RAM` across frames | window + `0x5C` every flip |
| Brightness | software black veil (pixels still PWM at full contrast) | command `0xC7` master contrast on device |

A phone at 30/60 fps will still beat against OLED PWM; that remaining roll is the camera shutter, not GRAM tearing.

!!! warning "No TE pin on this panel"
    Solomon SSD1351 does not expose a MIPI TE output on the Waveshare 7-pin module (VCC, GND, DIN, CLK, CS, DC, RST). Tear-free scan sync needs a board change. Until then: full-frame DMA faster than one multiplex period, plus double buffering so the CPU never mutates the page on the wire.

### Framebuffer mapping

On device, `set_screen_mode(ScreenMode::hires)` yields a **128×128 RGB565** surface — `blit::screen.bounds` is `(128, 128)` and `blit::screen.format` is `PixelFormat::RGB565`.

```text
citsy video[]     128×128 palette indices (uint8)
        │
        ▼
CitsyBlitHost     copies into video_[], builds pens_[256]
        │
        ▼
blit::Surface     PixelFormat::P + palette  →  stretch_blit 1×1
        │
        ▼
blit::screen      RGB565 128×128  →  SSD1351 WRITE_RAM
```

`CitsyBlitHost::draw()` wraps the index buffer as an 8-bit paletted surface and blits it 1:1 onto `blit::screen`. Integer scale is `min(screen.w, screen.h) / 128`, which is **1** on the OLED and up to **2** only if the host framebuffer is larger (desktop SDL is 320×240, so the 128×128 UI is letterboxed).

RGB565 packing is 5-6-5 (65,536 colours), matching the SSD1351 64K colour mode (`SET_REMAP` with the 64K / split / ABC bits set in the HAL).

!!! note "Map mode still uses video"
    citsy can present `GfxMap` (16×16 tile IDs). The engine’s compositor already expands rooms into the 128×128 video block, so the 32blit host always blits that video buffer — it does not draw tile IDs on the GPU.

## Battery voltage sensing

`HardwareStatus` (`hardware.cpp`) samples the pack on the RP2350 **12-bit ADC**.

| Parameter | Default |
|---|---|
| GPIO | **26** (`CITSY_BATTERY_ADC_PIN`) |
| ADC channel | **0** (`CITSY_BATTERY_ADC_CHANNEL`) |
| Reference | 3.3 V |
| Divider | ×3 (1/3 resistor divider into ADC0) |
| Sample period | 400 ms |
| LiPo empty | 3.2 V |
| LiPo full | 4.2 V |
| USB / 5 V rail | ≥ 4.55 V |

Init (device builds only):

```cpp
adc_init();
adc_gpio_init(26);          // ADC0
adc_select_input(0);
```

Each sample:

```cpp
const uint16_t raw = adc_read();                 // 0 … 4095
const float adc_v = (raw / 4095.0f) * 3.3f;      // volts at the pin
const float vbat  = adc_v * 3.0f;                // pack voltage
```

Percentage along a linear LiPo curve, then clamped to 0–100:

\[
p = \mathrm{clamp}\!\left(\frac{V_\mathrm{bat} - 3.2}{4.2 - 3.2},\; 0,\; 1\right) \times 100
\]

Edge cases handled in firmware:

- \(V_\mathrm{bat} < 1.5\,\mathrm{V}\) — floating ADC (no divider populated); treated as unknown / USB, UI shows a lightning nub instead of a false empty pack.
- \(V_\mathrm{bat} \ge 4.55\,\mathrm{V}\) — USB or boost rail; `on_usb()` is true, percent reports 100.
- Desktop SDL builds skip the ADC and report USB / 100%.

Override the pin at compile time if the sense net moves:

```bash
cmake --preset vgc -DCMAKE_CXX_FLAGS="-DCITSY_BATTERY_ADC_PIN=29 -DCITSY_BATTERY_ADC_CHANNEL=3"
```

(GPIO29 / ADC3 is the Pico-class VSYS÷3 node on an unmodified RP2350 Plus.)

The status bar maps percent onto an 11-pixel fill inside a 13×7 battery body (four visual “bars” by fill width). Low charge (≤ 20%, not on USB) switches the fill to amber.

## Display brightness

Settings expose **Bright** as 0–10 steps (`HardwareStatus::kSteps`). Two backends:

### PWM backlight (TFT / optional GPIO)

If `CITSY_BACKLIGHT_PIN` is defined, or `LCD_BACKLIGHT_PIN` is defined and is **not** the reset pin, that GPIO is claimed as PWM (wrap 65535) and the duty is gamma-corrected:

\[
\mathrm{pwm} = 65535 \times \left(\frac{\max(1, \mathrm{steps})}{10}\right)^{2.8}
\]

`pwm_set_gpio_level()` writes the compare value. Step 0 still uses a floor of 1 so the panel never blanks hard.

### Software veil (desktop) vs contrast (SSD1351 OLED)

The chili-chip 1.5" RGB OLED has **no PWM backlight**. On **device**, when the chili-chip HAL exports `ssd1351_set_master_contrast()`, the settings slider writes SSD1351 `CONTRAST_MASTER` (`0xC7`, 0–15). If that symbol is missing (older SDK), the launcher falls back to the software veil.

Desktop SDL has no OLED register, so the launcher still draws a translucent black rectangle:

```cpp
dim = ((10 - steps) * 180) / 10;   // 0 at full, 180 at minimum
screen.pen = Pen(0, 0, 0, dim);
screen.rectangle(canvas());
```

`CITSY_BACKLIGHT_PIN` (or a non-reset `LCD_BACKLIGHT_PIN`) still uses GPIO PWM for TFT panels.

## Audio control

citsy emits two `SoundChannel` structs (frequency, volume 0–1, pulse width, duration). `CitsyBlitHost::play_audio()` maps them onto `blit::channels[0]` and `[1]`:

| citsy | 32blit |
|---|---|
| `frequency_hz` | `channel.frequency` |
| `volume` (0–1) | `channel.volume` × `0xffff` |
| `PulseWave` | `pulse_width` (`0x1fff` / `0x3fff` / `0x7fff`) |
| square | `Waveform::SQUARE` + `trigger_sustain()` |

**Master volume** is `blit::volume` (`audio.hpp`), a `uint16_t` applied to the mixed sample in `get_audio_frame()`. The settings slider writes:

```text
blit::volume = 0xffff * steps / 10
```

On the RP2350 board, the HAL audio driver is PWM (`blit_driver(audio pwm)`) on `PICO_AUDIO_PWM_MONO_PIN` **GPIO 22**.

Pause overlay calls `CitsyBlitHost::stop_audio()` (`channels[n].off()`) so square waves do not leak under the menu.

## Persistent storage

Leaving Settings with **B** or **MENU** calls `HardwareStatus::save()`:

```cpp
struct Persist {
    uint32_t magic;       // 0x43535431  ('CST1')
    uint8_t  volume;      // 0–10
    uint8_t  brightness;  // 0–10
    uint8_t  pad[2];
};
blit::write_save(p, /*slot=*/0);
```

`blit::write_save` / `read_save` are the 32blit save API. On Pico they land in the HAL flash storage window (`FLASH_STORAGE_OFFSET` in `32blit-pico/config.h`); on desktop they are files under the 32blit save path. `init()` loads slot 0, ignores a mismatched magic, and applies volume + brightness before the first frame.

Defaults if no save exists: volume **8/10**, brightness **10/10**.
