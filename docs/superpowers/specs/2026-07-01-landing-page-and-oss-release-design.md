# Landing page + open-source release prep — design

- **Date:** 2026-07-01
- **Status:** Approved (core design); release-prep decisions defaulted (see below)
- **Branch:** `etais/upbeat-golick-16239a`

## Goal

Ship a public marketing landing page for **YT Music Companion**, built from the
Claude Design handoff (`Landing.dc.html`), and get the repo ready to flip from
private to public: license, open-source polish, and a scrub of internal data
from git history.

## Phase 1 — Build (non-destructive, normal PR)

### 1. `site/index.html` — the landing page

A single self-contained file (no framework, no build step). Recreates the
handoff design pixel-for-pixel:

- Sections: Nav → Hero → How it works → Features (6) → States gallery (3 device
  mocks) → Tech stack → Get started (2 code cards + gradient CTA) → Footer.
- **Device mock + canvas ring visualizer** ported faithfully from
  `NowPlayingDevice.dc.html` into a vanilla-JS `mountDevice()` factory. The
  Claude-Design `support.js` runtime is dropped (per handoff README). Each of the
  4 device instances runs its own `requestAnimationFrame` ring loop, reads its
  cover rect for the ring center, and honors `prefers-reduced-motion`.
- Copy-to-clipboard on the two code blocks (icon flips to ✓ for 1.6s).
- `githubUrl` → `https://github.com/etaiso/yt-music-companion`.

**Intentional deviations from a literal port:**
- **Responsiveness** — the prototype is fixed desktop width (1200px, 60px H1,
  hard multi-col grids) with no media queries. Add breakpoints (hero → 1 col,
  grids collapse, H1 scales, nav wraps) without altering the desktop pixel design.
- **Fonts via Google Fonts CDN** (Inter + Material Symbols Rounded), same as the
  prototype. Public site, no CSP sandbox, highest fidelity.

### 2. `.github/workflows/pages.yml`

Deploy `site/` to GitHub Pages on push to `main` using the official
`actions/configure-pages` + `upload-pages-artifact` + `deploy-pages`. Live URL:
`https://etaiso.github.io/yt-music-companion/`.

### 3. `design/youtube-music-companion-landing-page/`

Archive the raw handoff bundle (source of truth), matching the existing
`design/now-playing-screen-design/` convention.

### 4. Open-source polish

- `LICENSE` — **MIT**, © 2026 Etai Solomon.
- README: badge/link to the live site + a short "Third-party" attribution note
  (Inter font — SIL OFL 1.1; Material Symbols — Apache-2.0; LVGL — MIT;
  ESP-IDF — Apache-2.0).
- Keep it minimal: no CONTRIBUTING/CODE_OF_CONDUCT unless requested.

## Phase 2 — Release (DESTRUCTIVE, explicit confirmation required, done last)

Security scan result: **working tree is clean** (no secrets/keys, no internal email in
tracked content, Wi-Fi password is `changeme` placeholder, sdkconfig/token.json
git-ignored, lockfile on npmjs.org). But **git history** has two leaks:

1. **16 commits** authored/committed as `Etai Solomon <etai87@gmail.com>`.
2. **11 commits** whose `bridge/package-lock.json` contains
   `an internal registry` internal registry URLs.

**Scrub plan (default: surgical rewrite via `git-filter-repo`):**
1. Tag a safety backup ref (`backup/pre-scrub`) and confirm a local mirror exists.
2. `--mailmap`: `Etai Solomon <etai87@gmail.com> <etai87@gmail.com>`.
3. `--replace-text`: `https://registry.npmjs.org/` →
   `https://registry.npmjs.org/`.
4. Verify: no `internal`/`etai87@gmail.com` anywhere in the rewritten history.
5. Force-push `main` (after showing commands + explicit user go-ahead).
6. `gh repo edit --visibility public` + enable Pages (explicit go-ahead).

Alternative offered: squash all history into one clean "Initial public release"
commit. User may switch methods before Phase 2 runs.

## Decisions (defaulted; user may veto before Phase 2)

- License: **MIT**.
- History method: **surgical rewrite** (preserve commits).

## Implementation checklist

- [ ] Copy design bundle into `design/youtube-music-companion-landing-page/`
- [ ] Build `site/index.html` (structure, styles, `mountDevice()` ring port, copy buttons, responsive)
- [ ] Verify in preview server (layout, ring animation, copy, mobile, reduced-motion)
- [ ] `.github/workflows/pages.yml`
- [ ] `LICENSE` (MIT) + README polish
- [ ] PR to `main`
- [ ] (Phase 2, on explicit go-ahead) history scrub → force-push → public + Pages
