# Desktop simulator

Runs the **same** `ui/` code as the device on your Mac, so you can iterate on the
Now Playing screen and the ring animation without flashing. LVGL v9 is fetched
automatically; you only need CMake + SDL2.

## Prerequisites (macOS)

```bash
brew install cmake sdl2
```

## Prerequisites (Windows / MSYS2)

Use the [MSYS2](https://www.msys2.org/) MinGW-w64 toolchain — it ships an SDL2
CMake config, so `find_package(SDL2)` resolves with no extra glue.

```powershell
winget install MSYS2.MSYS2          # installs to C:\msys64
```

Then from an **"MSYS2 MinGW64"** shell (or any shell with `C:\msys64\mingw64\bin`
on `PATH`):

```bash
pacman -Sy
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake \
                   mingw-w64-x86_64-ninja mingw-w64-x86_64-SDL2
```

`C:\msys64\mingw64\bin` must be on `PATH` at build **and** run time so `SDL2.dll`
is found.

## Build & run

macOS/Linux:

```bash
cd sim
cmake -B build
cmake --build build
./build/ytm_sim
```

Windows (MSYS2 MinGW64) — use the Ninja generator and the MinGW gcc:

```bash
cd sim
cmake -B build -G Ninja -DCMAKE_C_COMPILER=gcc
cmake --build build
./build/ytm_sim.exe
```

A 480×480 window opens showing Now Playing. It cycles all six states every ~6 s;
clicking the transport controls prints the emitted command, e.g. `emit: next (0)`.

## Headless check (CI / no display)

```bash
SIM_MAX_FRAMES=120 SDL_VIDEODRIVER=dummy ./build/ytm_sim
```

Renders N frames and exits 0 — useful to confirm the build and state machine in an
environment without a window server.

## Status

The shared UI (`ui/*.c`) compiles clean against **LVGL v9.2.2** and the sim builds,
links, and runs on macOS and on Windows (MSYS2 MinGW64) — the headless run renders all
six states and exits 0.
