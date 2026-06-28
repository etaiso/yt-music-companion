# Running the bridge on Windows

Why: a corporate-managed Mac (Jamf MDM + GlobalProtect/CrowdStrike) blocks the
board's **inbound** WebSocket connection to the bridge and the firewall can't be
edited. Running the bridge on a non-managed Windows PC sidesteps that entirely.

The **firmware needs no changes** — the board discovers the bridge over mDNS, so it
connects to whichever machine on the LAN advertises `_ytmboard._tcp`.

**Requirement:** the Windows PC and the board must be on the **same router / subnet**
(Ethernet, or the **2.4GHz** band of the same Wi-Fi — the ESP32-S3 is 2.4GHz only).

---

## 1. Install ytmdesktop

1. Download from https://ytmdesktop.app and install.
2. Open it → **Settings → Integration**:
   - enable **Companion Server**
   - enable **Enable companion authorization**
3. Play a song. (Windows stores the token via DPAPI — none of the macOS
   safeStorage/code-signing trouble.)

## 2. Install Node.js LTS

- https://nodejs.org , or in PowerShell: `winget install OpenJS.NodeJS.LTS`
- verify: `node --version` (need >= 18)

## 3. Get the bridge code and run it

In PowerShell:
```powershell
git clone https://github.com/etaiso/yt-music-companion.git
cd yt-music-companion\bridge
npm install
npm start
```
On first run the bridge prints a code and ytmdesktop shows an **Allow** prompt — click
**Allow** within ~30s. You should then see:
```
[board] WebSocket server on ws://0.0.0.0:8765
[mdns] advertising _ytmboard._tcp on :8765
Authorized. Token saved.
[ytmd] socket connected
```
The token is saved (under your user profile) and reused on later runs.

## 4. Allow node through Windows Firewall

The first time node opens the port, Windows pops **"Allow node.js to communicate on
these networks?"** → tick **Private networks** → **Allow access**.

If you missed the popup and the board can't connect, add the rule manually
(PowerShell as Administrator):
```powershell
New-NetFirewallRule -DisplayName "YTM Bridge 8765" -Direction Inbound -Protocol TCP -LocalPort 8765 -Action Allow -Profile Private
```

## 5. The board

No reflash needed — it auto-discovers the Windows bridge by mDNS. Within ~30s the
panel should switch from "can't reach the mac" to the live track + cover art.

### If the board doesn't find the bridge (Windows mDNS can be flaky)

Use the built-in static-host fallback instead of mDNS:

1. On the PC, get its IPv4: `ipconfig` → look for `IPv4 Address` (e.g. `192.168.50.x`).
2. On the Mac (with ESP-IDF), reconfigure the board once:
   ```bash
   cd firmware
   . ~/esp/esp-idf/export.sh
   idf.py menuconfig    # YT Music board -> "Bridge host fallback (optional)" = the PC IPv4
   idf.py -p /dev/cu.usbmodemXXXXX -b 115200 flash
   ```
   The board then connects straight to that IP and skips mDNS.

---

## Quick test without the board

With the bridge running, from another PowerShell window:
```powershell
npx wscat -c ws://localhost:8765
```
You should see `{"type":"state","data":{...}}` frames while music plays. Type
`{"cmd":"toggle_play"}` to confirm control works.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `Fatal: fetch failed` on `npm start` | ytmdesktop not running, or Companion Server not enabled |
| Board stuck "can't reach the mac" | PC and board on different subnets (5GHz vs 2.4GHz, or VPN), or firewall — see steps 4–5 |
| `connecting to ws://254.128.x` in board log | old firmware; reflash from current `main` (mDNS IPv4 fix landed) |
| Board log shows real IP then `Connection reset` | a firewall on the bridge host — allow node / port 8765 (step 4) |
