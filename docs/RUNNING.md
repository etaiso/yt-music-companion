# Running the YT Music Companion

A complete guide to building, running, and flashing this project. There are **two
ways to run the UI**:

1. **Desktop simulator** (`sim/`) — runs the exact same `ui/` code on your Mac in an
   SDL window. Fastest loop; no hardware needed. Start here.
2. **Device firmware** (`firmware/`) — the ESP-IDF build that flashes onto the
   Waveshare ESP32-S3-Touch-AMOLED-2.16.

Both render the **Now Playing** screen from mock data. There is no network code yet —
controls just log their intent over serial (device) or stdout (sim).

---

## 1. Desktop simulator (start here)

The simulator is the fastest way to see changes. It compiles the shared `ui/*.c`
files against LVGL v9 and opens a 480×480 window that cycles through all six player
states every ~6 seconds. Clicking the transport controls prints the emitted command,
e.g. `emit: next (0)`.

### Prerequisites (macOS)

```bash
brew install cmake sdl2
```

LVGL itself is fetched automatically by CMake (`FetchContent`, pinned to `v9.2.2`), so
you only need CMake and SDL2.

### Build and run

```bash
cd sim
cmake -B build
cmake --build build
./build/ytm_sim
```

A window opens showing Now Playing. The ring visualizer animates from the mock
`level`, and the screen rotates through playing → paused → buffering → no-track → ad →
disconnected.

### Headless check (CI / no display)

Useful to confirm the build and state machine where there's no window server:

```bash
SIM_MAX_FRAMES=120 SDL_VIDEODRIVER=dummy ./build/ytm_sim
```

This renders N frames and exits `0`.

### Rebuilding after changes

After editing any file in `ui/` or `sim/`, just re-run the build step:

```bash
cmake --build build
```

If you change `CMakeLists.txt` or want a clean slate, delete the build dir:
`rm -rf build` and start from `cmake -B build` again.

---

## 2. Device firmware (ESP-IDF + flashing)

This is the real thing: it brings up the vendor BSP (CO5300 QSPI panel, CST9220
touch, LVGL v9) and renders Now Playing on the board.

### Hardware

- **Waveshare ESP32-S3-Touch-AMOLED-2.16** — ESP32-S3R8 (8 MB octal PSRAM, 16 MB
  flash), 480×480 AMOLED.
- A **USB-C cable** that carries data (not charge-only) to connect the board to your
  Mac.

### 2.1 Install ESP-IDF (one time)

The project targets **ESP-IDF v5.5 or newer** (`idf_component.yml` requires
`idf: ">=5.5"`). The recommended install:

```bash
mkdir -p ~/esp
cd ~/esp
git clone -b v5.5 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3
```

That downloads the toolchain for the ESP32-S3 target. After installing, every new
terminal session needs the environment exported:

```bash
. ~/esp/esp-idf/export.sh
```

(The leading dot matters — it sources the script into your current shell. Many people
alias this to `get_idf`.) You'll know it worked when `idf.py --version` prints a v5.5+
version.

> macOS USB driver note: recent ESP32-S3 boards expose a native USB-Serial-JTAG
> interface, so no extra driver is usually needed. If your board uses a USB-UART
> bridge chip and doesn't enumerate, install the relevant driver (CP210x or CH34x)
> from the vendor.

### 2.2 Build

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
```

What happens on first build:

- The **component manager** reads `main/idf_component.yml` and pulls the Waveshare BSP
  (`waveshare/esp32_s3_touch_amoled_2_16`, `^1.0.0`). That component configures the
  panel, touch, and LVGL v9 for this exact board and re-exports `lvgl.h`.
- `sdkconfig.defaults` is applied: octal PSRAM @ 80 MHz, 16 MB QIO flash, the custom
  `partitions.csv` (big-APP scheme), 1000 Hz FreeRTOS tick for smooth animation, and
  the LVGL Montserrat fonts.
- The shared UI in `../../ui` is compiled in alongside `main/main.c` (see
  `main/CMakeLists.txt`).

`set-target esp32s3` only needs to be run once per checkout (or after `fullclean`).

### 2.3 Flash and monitor

Plug in the board, then:

```bash
idf.py -p /dev/tty.usbmodem* flash monitor
```

- `-p /dev/tty.usbmodem*` is the serial port. On macOS the port is usually
  `/dev/tty.usbmodemXXXX` (native USB-JTAG) or `/dev/tty.usbserial-XXXX` (UART
  bridge). Run `ls /dev/tty.usb*` to find the exact name if the glob doesn't resolve.
- `flash` builds (if needed), erases, and writes the app.
- `monitor` opens the serial console. You should see:

  ```
  ytm: Now Playing slice up (mock data, 30 fps).
  ```

  and, as you tap the on-screen controls, lines like `emit: toggle_play (0)`,
  `emit: next (0)`, etc.

Exit the monitor with **Ctrl-]**.

If you only want to flash without the console, drop `monitor`. To monitor an
already-flashed board without reflashing, run `idf.py -p <port> monitor`.

### 2.4 Common device commands

```bash
idf.py build                      # compile only
idf.py -p <port> flash            # flash without opening monitor
idf.py -p <port> monitor          # serial console only
idf.py -p <port> flash monitor    # the usual loop
idf.py menuconfig                 # tweak config interactively
idf.py fullclean                  # wipe build/ (re-run set-target after)
idf.py -p <port> erase-flash      # full chip erase if things get weird
```

---

## 3. Troubleshooting

**`idf.py: command not found`** — you didn't source the environment. Run
`. ~/esp/esp-idf/export.sh` in this terminal.

**Component manager can't fetch the Waveshare BSP** — needs network access on first
build. Re-run `idf.py build`; the download is cached under `managed_components/`
afterward.

**Port not found / permission denied** — confirm the board enumerates with
`ls /dev/tty.usb*`. Try a different USB-C cable (charge-only cables won't work). If
the board is stuck, hold **BOOT**, tap **RESET**, release **BOOT** to force download
mode, then flash.

**Build fails after upstream changes** — `idf.py fullclean`, then
`idf.py set-target esp32s3 && idf.py build`.

**BSP header mismatch.** `main/main.c` includes `bsp/esp-bsp.h` and calls
`bsp_display_start()`, `bsp_display_backlight_on()`, `bsp_display_lock()/unlock()`
(the esp-bsp convention). If the pulled Waveshare component exposes a different header
or init entry point, that's the *only* board-specific surface — adjust the include and
those calls in `main.c` to match.

---

## 4. Notes

**Fonts.** This slice uses LVGL's built-in **Montserrat** (enabled in
`sdkconfig.defaults`) so it builds with no extra assets. Production swaps in bundled
**Inter ExtraBold/800**: convert Inter with `lv_font_conv`, add the generated `.c`,
and point the `FONT_*` macros in `ui/styles.h` at it. `styles.h` is the only file to
touch for theming and fonts.

**What's not in this slice.** No Wi-Fi, no host discovery, no real audio. The next
deliverable is the Mac-side ytmdesktop bridge (`docs/SPEC-ytmusic-adapter.md`), which
will replace the mock feed and wire the `emit(...)` stubs to real commands. See
`docs/PROJECT-OVERVIEW.md` for the full roadmap.

**Why the simulator matters.** Because `firmware/` and `sim/` compile the same
`ui/*.c`, you can do nearly all UI and animation work in the simulator and only flash
to verify on real hardware.
