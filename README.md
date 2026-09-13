# citsy-32blit

A [32blit](https://github.com/chili-chip/32blit-sdk) player for the [citsy](https://github.com/TeriyakiGod/citsy) engine — a headless C++ reimplementation of [Bitsy](https://bitsy.org).

This repository is generated from the [chili-chip/game-template](https://github.com/chili-chip/game-template). It is the Phase 4 host: citsy stays engine-only; this project opens a window (or drives the handheld), maps buttons, blits the 128×128 framebuffer, and plays square-wave audio.

The player boots into a 128×128 retro launcher (game carousel, settings, pause overlay) designed for a Waveshare RP2350 Plus + 128×128 16-bit SPI OLED, using the chili-chip 32blit SDK.

## Requirements

- CMake 3.20+
- C++20 compiler
- [32blit SDK](https://github.com/chili-chip/32blit-sdk) (sibling checkout at `../32blit-sdk`, or set `32BLIT_DIR`)
- [32blit tools](https://github.com/32blit/32blit-tools): `pip install 32blit`
- SDL2, SDL2_image, SDL2_net (desktop)
- A [citsy](https://github.com/TeriyakiGod/citsy) checkout (sibling at `../citsy`, or `-DCITSY_DIR=...`)

## Desktop build

From this directory, with `32blit-sdk` and `citsy` as siblings:

```bash
cmake --preset linux
cmake --build --preset linux
./out/build/linux/citsy-32blit.elf
```

Equivalent without presets:

```bash
cmake -B build -D32BLIT_DIR=../32blit-sdk
cmake --build build
./build/citsy-32blit.elf
```

The launcher lists bundled games (`Mossland`, `Sandbox`) plus any `.bitsy` files next to the binary. A game is started only when you press **A**, or when a launch path is passed (see below).

## Loading `.bitsy` files

Games are plain-text Bitsy exports. This player loads them in three ways:

1. **Bundled assets** — `assets/mossland.bitsy` and `assets/sandbox.bitsy` are packed into the binary by the 32blit asset tool (`assets.yml`, `type: raw/binary`). They are registered with `File::add_buffer_file` so they behave like real files.
2. **Filesystem** — any `*.bitsy` next to the executable, or in a `games/` folder, appears in the launcher. On desktop the search path is the binary directory (`SDL_GetBasePath`). On device this includes the SD card when the chili-chip HAL mounts it.
3. **Launch path** — skip the menu and boot a file the same way the 32blit console does:

```bash
./out/build/linux/citsy-32blit.elf --launch_path /path/to/game.bitsy
```

Author games at [bitsy.org](https://bitsy.org), export `.bitsy`, and drop the file in.

### Controls

| Action | 32blit | Desktop keyboard |
|---|---|---|
| Move cursor / walk | D-pad | Arrows / WASD |
| Confirm / interact | A | Z |
| Back / cancel | B | X |
| Settings | X or MENU (from launcher) | C or Escape |
| Pause overlay (in game) | MENU | Escape or `2` |
| Volume / brightness | Left / Right in Settings | Left / Right |

Pause overlay items: **Resume**, **Restart**, **Exit** (back to the launcher). MENU or B on the overlay resumes.

Settings (volume and OLED/backlight brightness) are written to 32blit save slot 0 when you leave the screen. On RP2350, battery voltage is sampled from ADC3 (GPIO29, VSYS÷3 on Pico-class boards such as the Waveshare RP2350 Plus).

## VGC Zero / device

```bash
cmake --preset vgc
cmake --build --preset vgc
```

Or:

```bash
cmake -B build.vgc \
  -D32BLIT_DIR=../32blit-sdk \
  -DPICO_SDK_PATH=../pico-sdk \
  -DPICO_BOARD=chilichip_vgc \
  -DPICO_PLATFORM=rp2350-arm-s \
  -DCMAKE_TOOLCHAIN_FILE=../32blit-sdk/pico2.toolchain
cmake --build build.vgc
```

See the [VGC notes in the 32blit SDK](https://github.com/chili-chip/32blit-sdk/blob/master/docs/vgc.md).

Optional compile definitions:

- `CITSY_BATTERY_ADC_PIN` / `CITSY_BATTERY_ADC_CHANNEL` — override the battery sense pin (default GPIO29 / ADC3)
- `CITSY_BACKLIGHT_PIN` — PWM brightness pin; if unset, brightness is a software veil (correct for SSD1351 OLED, which has no backlight)

## How it talks to citsy

```
32blit init/update/render  →  launcher state machine  →  CitsyBlitHost  →  citsy::Engine  →  .bitsy text
```

States: `MainMenu` → `Settings` / `GameRunning` → `PauseOverlay`. Each frame the host reports `delta_time_ms()` and button state. The engine writes a 128×128 colour-index buffer (and two square-wave channels). `CitsyBlitHost::draw()` palettes that buffer and integer-scales it into the framebuffer (1× on the 128×128 OLED).

The core library never links 32blit. See [citsy Host API](https://teriyakigod.github.io/citsy/host/).
