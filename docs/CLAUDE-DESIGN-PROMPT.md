# Claude Design prompt — Now Playing screen

Based on §6 ("THE TASK — Now Playing screen") of `SPEC-ytmusic-now-playing.md`.
Paste the block below into Claude Design. It's self-contained, but attaching
`SPEC-ytmusic-now-playing.md` gives even better fidelity.

---

Design the **Now Playing** screen for a YouTube Music hardware companion — a small
desktop dock/remote with a **2.16″ square AMOLED touchscreen, exactly 480×480 px**.
Make it a **single full-screen 480×480 artboard** with round-cornered framing. It's a
thin client: audio plays on the user's Mac; this screen only displays the current
track and sends touch commands. Used at arm's length, so type and controls are large;
touch targets min ~44px.

**Design language (use exactly):**
- Background: warm cream `#FBF7F3`. Cards/raised surfaces white `#FFFFFF`, ~16px
  radius, soft shadows. Pills fully rounded (radius 999).
- Ink scale: primary text `#1A1410`, secondary `#4A3F37`, muted `#8A7A6E`,
  faint/placeholder `#B8A99A`. Borders `#ECE2D8`. Error only `#C0392B`.
- Brand gradient 135°: indigo `#6366F1` → purple `#A855F7` → pink `#EC4899`. Use it
  **sparingly** — only the rings, the play button, and active/"on" states. Pink =
  primary accent; purple = play control, status dots, section labels.
- Typography: **Inter**. Display title ~22–28px / 800 / tight tracking; section labels
  11px / 800 / UPPERCASE / ~1.5px tracking; body ~15px; meta 11–13px (muted ink).
- Feel: warm, editorial, calm — one bold gradient moment, never busy.

**Layout (top → bottom), matching this wireframe:**
```
┌─────────────────────────────────────────┐
│  YouTube Music                  ▷ PLAYING │  status bar: source label (l) · state (r)
│                                          │
│            ╭───────────────╮             │
│         (   concentric rings  )          │  HERO: audio-reactive gradient rings
│        (    ┌─────────┐       )           │  with cover art centered (rounded ~20px)
│         (   │  cover  │      )            │
│            ╰───────────────╯             │
│                                          │
│             Midnight Drive               │  title (display, 800)
│             The Reverb Club              │  artist (ink2)
│             Neon Nights · Album          │  album/context (ink3, optional)
│                                          │
│   1:12  ▓▓▓▓▓▓▓░░░░░░░  3:48              │  elapsed / total seekable bar
│                                          │
│     ♡    ⏮     (❚❚)     ⏭     ♥          │  transport row
└─────────────────────────────────────────┘
```

**The ring visualizer (the one bold thing — spend the most effort here):**
3–4 concentric arcs/circles in the brand gradient stroke, behind and around the
centered album cover. It's an audio-reactive visualizer — rings ripple outward, inner
rings stronger; draw it mid-pulse. This is also the product's signature/brand motif.

**Transport row controls (left → right):** like ♡, previous ⏮, large round **purple
play/pause** in the center, next ⏭, like ♥ (filled when liked).

**Progress:** a finite **seekable** bar — elapsed time, gradient-filled track, total
time (e.g. `1:12 ▓▓▓▓▓░░░ 3:48`). Not a radio/live timeline.

**Show these state variants** (as small alternate frames beside the main one), plain
active sentence-case copy:
- **Playing** — full layout (the main frame).
- **Paused** — play glyph showing; rings settle to a gentle pulse.
- **Buffering** — shimmer on the title line; rings idle.
- **Nothing playing right now** — that exact copy in the title area; no artist/album;
  cover falls back to a gradient block.
- **Advertisement** — "Advertisement" in the title area; gradient-block cover;
  controls dimmed.
- **Disconnected** — small banner: "Can't reach the Mac — check it's on and on the
  same network."

Deliver clean, production-leaning mockups, consistent spacing, tokens above, all
within the 480×480 round-cornered AMOLED frame.

---

**Iterating in Claude Design:**
- Generate the **Playing** state first; refine the rings + gradient until they sing,
  then ask for the other five state variants in the same style.
- If the gradient creeps in everywhere: "use the gradient only on the rings, the play
  button, and active states — everything else is ink on cream."
- Ask for a **dark / AMOLED variant** (true-black background) afterward — the token
  set is built to swap to a dark table later.
