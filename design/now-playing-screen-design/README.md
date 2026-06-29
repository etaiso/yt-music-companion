# CODING AGENTS: READ THIS FIRST

This is a **handoff bundle** from Claude Design (claude.ai/design).

A user mocked up designs in HTML/CSS/JS using an AI design tool, then exported this bundle so a coding agent can implement the designs for real.

## What you should do — IMPORTANT

**Read `now-playing-screen-design/project/Now Playing.dc.html` in full.** The user had this file open when they triggered the handoff, so it's almost certainly the primary design they want built. Read it top to bottom — don't skim. Then **follow its imports**: open every file it pulls in (shared components, CSS, scripts) so you understand how the pieces fit together before you start implementing.

**If anything is ambiguous, ask the user to confirm before you start implementing.** It's much cheaper to clarify scope up front than to build the wrong thing.

## About the design files

The design medium is **HTML/CSS/JS** — these are prototypes, not production code. Your job is to **recreate them pixel-perfectly** in whatever technology makes sense for the target codebase (React, Vue, native, whatever fits). Match the visual output; don't copy the prototype's internal structure unless it happens to fit.

**Don't render these files in a browser or take screenshots unless the user asks you to.** Everything you need — dimensions, colors, layout rules — is spelled out in the source. Read the HTML and CSS directly; a screenshot won't tell you anything they don't.

## Bundle contents

- `now-playing-screen-design/README.md` — this file
- `now-playing-screen-design/project/` — the `Now Playing screen design` project files (HTML prototypes, assets, components)

## Versions

The project ships two design revisions. **V2 is the current direction** — a dark theme.

- **V1** (light) — `project/Now Playing.dc.html` + `project/NowPlayingDevice.dc.html`. Light screen (`#FBF7F3`), 128px cover, purple `#A855F7` accent. Screenshot: `project/screenshots/canvas.png`.
- **V2** (dark) — `project/Now Playing v2.dc.html` + `project/NowPlayingDeviceV2.dc.html`. Near-black screen (`#070709`) with an album-art ambient glow, 172px cover, weight-900 type, red `#FF4458` accent. The V2 canvas imports the `NowPlayingDeviceV2` component. Screenshot: `project/screenshots/canvas-v2.png`.

Both share `project/support.js` (runtime only — do not port).
