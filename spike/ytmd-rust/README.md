# ytmd Rust spike — Phase 0 de-risking

**Throwaway.** This is the decision-gate spike from
[`docs/superpowers/specs/2026-07-08-standalone-bridge-app-design.md`](../../docs/superpowers/specs/2026-07-08-standalone-bridge-app-design.md).
It proves (or disproves) that the ytmdesktop Companion protocol can be driven
from Rust well enough to justify **Branch T** (Tauri + native Rust bridge). It
is not shipped as-is; its code informs whichever branch wins.

## What it does

1. **Auth handshake** — requests a Companion code, waits for you to click
   **Allow**, receives and persists a token (`~/.ytmboard/spike-token.json`).
2. **Realtime** — connects the Socket.IO channel and prints live track state
   (`artist — title`, playback, position) as songs change.
3. **mDNS (bonus)** — advertises `_ytmboard._tcp` on `:8765` and browses for it
   from the same process to confirm a client can resolve it.

It uses a **distinct** app identity (`ytmboardspike`) and its own token file, so
running it will **not** clobber the real bridge's saved token.

## Prerequisites (human-in-the-loop)

- **Rust** — already installed on this machine (`rustup` + the
  `stable-x86_64-pc-windows-gnu` toolchain). `rust-toolchain.toml` pins the GNU
  toolchain for this dir automatically. (The default MSVC toolchain fails here:
  an MSYS2 `link.exe` shadows MSVC's linker and no VS C++ Build Tools are
  installed — GNU + MinGW sidesteps both.)
- **MinGW gcc on PATH** — the GNU toolchain links with `C:\msys64\mingw64\bin\gcc`,
  which isn't on PATH by default. The run command below adds it.
- **ytmdesktop running** with **Settings → Integrations → Companion Server**
  enabled, and **music playing**.
- You'll **click Allow** once when the code appears.

## Run

From **Git Bash** in this directory:

```sh
PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo run
```

or from **PowerShell**:

```powershell
$env:Path = "$HOME\.cargo\bin;C:\msys64\mingw64\bin;$env:Path"; cargo run
```

The crates are already built, so this starts immediately. It will print the auth
code, wait for your **Allow** click, then stream live track state until Ctrl-C.

## Pass criteria (report the outcome)

- [ ] **1.** Code prints, you click Allow, `Authorized. Token saved` appears.
- [ ] **2.** `[state-update] playing  <artist> — <title>` lines appear and
      change when you skip tracks.
- [ ] **3.** `[mdns] discovered _ytmboard._tcp...` line appears.

- **All three clean → Branch T** (Tauri + native Rust bridge).
- **Painful** (protocol/version friction, flaky auth, missing crate behavior) →
  **Branch E** (Electron + existing Node bridge).

Paste the console output (especially any errors) back into the design thread.
