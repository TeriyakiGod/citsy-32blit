# citsy-32blit

A [32blit](https://github.com/32blit/32blit-sdk) game that runs the [citsy](https://github.com/TeriyakiGod/citsy) engine — a headless C++ reimplementation of [Bitsy](https://bitsy.org).

This repository is generated from the [chili-chip/game-template](https://github.com/chili-chip/game-template). It is the Phase 4 host: citsy stays engine-only; this project opens a window, maps buttons, blits the 128×128 framebuffer, and plays square-wave audio.

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

The bundled `playable.bitsy` game starts immediately.

## Loading `.bitsy` files

Games are plain-text Bitsy exports. This player loads them in three ways:

1. **Bundled assets** — `assets/playable.bitsy` and `assets/two_rooms.bitsy` are packed into the binary by the 32blit asset tool (`assets.yml`, `type: raw/binary`). They are registered with `File::add_buffer_file` so they behave like real files.
2. **Filesystem** — any `*.bitsy` next to the executable, or in a `games/` folder, appears in the in-game list. On desktop the search path is the binary directory (`SDL_GetBasePath`).
3. **Launch path** — pass a file the same way the 32blit console does:

```bash
./out/build/linux/citsy-32blit.elf --launch_path /path/to/game.bitsy
```

Author games at [bitsy.org](https://bitsy.org), export `.bitsy`, and drop the file in.

### Controls

| Bitsy | 32blit | Desktop keyboard |
|---|---|---|
| Move | D-pad | Arrows / WASD |
| Interact / advance dialog | A or B | Z or X |
| Game list | MENU | Escape or `2` |

MENU while playing returns to the game list. A on a list entry starts that game.

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

## How it talks to citsy

```
32blit init/update/render  →  CitsyBlitHost (citsy::Host)  →  citsy::Engine  →  .bitsy text
```

Each frame the host reports `delta_time_ms()` and button state. The engine writes a 128×128 colour-index buffer (and two square-wave channels). `CitsyBlitHost::draw()` palettes that buffer and integer-scales it into the 320×240 framebuffer.

The core library never links 32blit. See [citsy Host API](https://teriyakigod.github.io/citsy/host/).
