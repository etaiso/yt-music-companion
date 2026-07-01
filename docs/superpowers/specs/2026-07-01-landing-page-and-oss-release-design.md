# Landing page — design

- **Date:** 2026-07-01
- **Status:** Built (see `site/index.html`)

## Goal

Ship a public marketing landing page for **YT Music Companion**, built from the
Claude Design handoff (`Landing.dc.html`), and prepare the repo for public release
(MIT license + light open-source polish).

## Deliverables

### `site/index.html` — the landing page

A single self-contained file (no framework, no build step). Recreates the handoff
design pixel-for-pixel:

- Sections: Nav → Hero → How it works → Features → States gallery → Tech stack →
  Get started (two code cards + gradient CTA) → Footer.
- The `NowPlayingDevice` mock is ported into a vanilla-JS `mountDevice()` factory
  (the Claude-Design `support.js` runtime is dropped, per the handoff README).
- Copy-to-clipboard on the two code blocks.
- `githubUrl` → `https://github.com/etaiso/yt-music-companion`.

**Intentional deviations from a literal port:**

- **Responsiveness** — the prototype is fixed desktop width (1200px, hard multi-col
  grids) with no media queries. Added breakpoints (hero → 1 col, grids collapse,
  headline scales, nav wraps) without altering the desktop pixel design.
- **Fonts via Google Fonts CDN** (Inter + Material Symbols Rounded), same as the
  prototype.

### `.github/workflows/pages.yml`

Deploy `site/` to GitHub Pages on push to `main`. Live URL:
`https://etaiso.github.io/yt-music-companion/`.

### `design/youtube-music-companion-landing-page/`

Archived design handoff bundle (matches the existing `design/now-playing-screen-design/`
convention).

### Open-source polish

- `LICENSE` — MIT, © 2026 Etai Solomon.
- README: landing-page pointer + license/attribution sections (Inter — SIL OFL 1.1;
  Material Symbols — Apache-2.0; LVGL — MIT; ESP-IDF — Apache-2.0).

## Update — design revision (2026-07-01)

The design was revised after the first build. Applied to `site/index.html` and the
archived bundle:

- **Ring visualizer removed** — the design dropped the canvas ring from the device
  (and the "Reactive ring visualizer" feature card). `mountDevice()` is now static.
- **ytmdesktop emphasized** — "Required" pill on the How-it-works card, stronger copy,
  and a "Prerequisite for live music" callout in Get started.
- **New feature card** — "Brightness & battery saving".
- **Gallery captions** reworded ("Live track & art blitted", "Dimmed idle state").
- **Tech stack** — hardware is now a link to Waveshare + "other ESP32‑S3 targets may work".

## Verification

Rendered via the `site` preview server: all sections present and correct, no console
errors, desktop grids and mobile (single column, no horizontal overflow) both confirmed.
