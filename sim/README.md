# Desktop simulator

Runs the **same** `ui/` code as the device on your Mac, so you can iterate on the
Now Playing screen and the ring animation without flashing. LVGL v9 is fetched
automatically; you only need CMake + SDL2.

## Prerequisites (macOS)

```bash
brew install cmake sdl2
```

## Build & run

```bash
cd sim
cmake -B build
cmake --build build
./build/ytm_sim
```

A 480×480 window opens showing Now Playing. It cycles all six states every ~6 s;
clicking the transport controls prints the emitted command, e.g. `emit: next (0)`.

## Theme (Dark / Light)

The sim builds the **Dark** theme by default (the V2 near-black look). To build the
**Light** theme — the same layout painted with light tokens and the album glow
disabled — pass the theme flag at configure time:

```bash
cmake -B build -DYTM_THEME=LIGHT
cmake --build build
```

This defines `YTM_THEME_LIGHT` for the build; omitting it (or `-DYTM_THEME=DARK`)
falls back to Dark. The same split is driven on firmware by the Kconfig "UI theme"
choice. Re-run `cmake -B build -DYTM_THEME=DARK` to switch back.

## Headless check (CI / no display)

```bash
SIM_MAX_FRAMES=120 SDL_VIDEODRIVER=dummy ./build/ytm_sim
```

Renders N frames and exits 0 — useful to confirm the build and state machine in an
environment without a window server.

## Status

The shared UI (`ui/*.c`) has been syntax-checked against **LVGL v9.2.2** headers and
compiles clean. The SDL link step runs on your machine via the steps above (the build
sandbox used during development had no SDL2 / root access to install it).
