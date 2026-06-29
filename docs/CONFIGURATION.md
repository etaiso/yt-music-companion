# Configuration

All the knobs for the board firmware and the host bridge, in one place.

- [Firmware (build-time, Kconfig)](#firmware-build-time-kconfig)
- [Bridge (runtime, env vars)](#bridge-runtime-env-vars)
- [The `COVER_PX` contract pair](#the-cover_px-contract-pair)

---

## Firmware (build-time, Kconfig)

The board is configured at **build time** — there is no runtime settings screen. Open
the menu with:

```sh
cd firmware
idf.py menuconfig      # YT Music board  ->  the options below
```

`menuconfig` writes to `firmware/sdkconfig` (applied on the next build). You can also
preset values in `firmware/sdkconfig.defaults` — but defaults only take effect when
`sdkconfig` is generated fresh, so delete `firmware/sdkconfig` first to force them.

All options live under the **"YT Music board"** menu:

| Option | Key | Default | Notes |
|--------|-----|---------|-------|
| Use live network feed | `YTM_USE_NET` | `y` | On: connect to the bridge over Wi-Fi. Off: drive the screen from `mock.c` for an offline hardware demo. |
| UI theme | `YTM_THEME_DARK` / `YTM_THEME_LIGHT` | Dark | V2 Dark (near-black + album glow) or the same layout in light tokens with the glow disabled. No runtime toggle. |
| Display brightness | `YTM_DISPLAY_BRIGHTNESS` | `40` | AMOLED brightness, 5–100%. Applied via panel command after init. Lower it if the screen is harsh. |
| Wi-Fi SSID | `YTM_WIFI_SSID` | `changeme` | **Must be set** for the live feed — network the bridge host is on. |
| Wi-Fi password | `YTM_WIFI_PASSWORD` | `changeme` | WPA2 passphrase; leave empty for an open network. |
| Bridge host fallback | `YTM_BRIDGE_HOST` | `""` | Set to skip mDNS and connect straight to a host IP (e.g. `192.168.1.50`). Empty = discover via `_ytmboard._tcp`. |
| Bridge port fallback | `YTM_BRIDGE_PORT` | `8765` | Port used with the host fallback, and if the mDNS TXT record omits one. |

> **You must set Wi-Fi credentials** (`YTM_WIFI_SSID` / `YTM_WIFI_PASSWORD`) before
> flashing a live build — the `changeme` defaults won't connect. The board is 2.4GHz
> only, so the SSID must be a 2.4GHz network on the same subnet as the bridge.

The Wi-Fi / bridge options only appear when `YTM_USE_NET` is enabled.

## Bridge (runtime, env vars)

The bridge needs no config for the common case (`npm start`). Override behavior with
environment variables:

| Env | Default | Purpose |
|-----|---------|---------|
| `YTMD_HOST` / `YTMD_PORT` | `127.0.0.1` / `9863` | ytmdesktop Companion Server (IPv4 on purpose). |
| `BOARD_PORT` | `8765` | Board-facing WebSocket port (advertised over mDNS). |
| `YTMBOARD_TOKEN_PATH` | `~/.ytmboard/token.json` | Where the Companion token is persisted. |
| `YTMD_APP_ID` | `ytmboard` | Companion app identity shown in ytmdesktop. |
| `COVER_PX` | `172` | Cover-art square size pushed to the board (see below). |

See [bridge/README.md](../bridge/README.md) and
[bridge/WINDOWS-SETUP.md](../bridge/WINDOWS-SETUP.md) for the full bridge guide.

## The `COVER_PX` contract pair

The cover-art size is a **matched pair** that must change together:

- Bridge: `COVER_PX` env (default `172`) — resizes art before sending.
- Board: `#define COVER_PX 172` in [firmware/main/net_backend.c](../firmware/main/net_backend.c).

The board rejects a cover whose dimensions don't match its expected size, so a mismatch
fails loudly instead of corrupting the buffer. **Never bump one without the other** —
call this out in any PR that touches it.
