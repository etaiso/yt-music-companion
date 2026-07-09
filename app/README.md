# ytmboard bridge (native Rust)

Branch T of the standalone bridge (see
`docs/superpowers/specs/2026-07-08-standalone-bridge-app-design.md`). A native
Rust port of the Node bridge: ytmdesktop Companion Server → `now_playing_vm` →
board, plus `_ytmboard._tcp` mDNS and RGB565 cover art.

## Layout
- `bridge-core/` — the library (auth, ytmd, normalize, cover, board server, mDNS,
  orchestrator). Host-testable pure logic + async I/O.
- `bridge-cli/` — thin CLI; developer parity with the old `npm start`.
- `src-tauri/`, `ui/` — the tray app (Part 2).

## Build & run (Windows)
The default MSVC toolchain fails to link here; use the GNU toolchain + MinGW gcc.
`rust-toolchain.toml` pins gnu.

```sh
PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo run -p bridge-cli
```

Requires ytmdesktop with Companion Server **and** "enable companion
authorization" on. First run prints a code — click **Allow** within ~30s. The
token is cached at `~/.ytmboard/token.json`.

## Test
```sh
PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core
```
