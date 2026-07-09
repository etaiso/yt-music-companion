// Guided status window for the YTM Board Bridge tray app.
// Renders the bridge state machine (BridgeEvent, forwarded from Rust as the
// "bridge-event" webview event) and its calls-to-action. Plain DOM, no
// bundler — `window.__TAURI__` is injected because `app.withGlobalTauri` is
// true in tauri.conf.json.

(() => {
  "use strict";

  const app = document.getElementById("app");
  // Cached so board-connected can redraw the track card immediately after a
  // state transition, even if the next "now-playing" event hasn't arrived yet.
  let lastNowPlaying = null;

  // --- tiny DOM builder -----------------------------------------------
  function el(tag, attrs = {}, children = []) {
    const node = document.createElement(tag);
    for (const [key, value] of Object.entries(attrs)) {
      if (value == null) continue;
      if (key === "class") node.className = value;
      else if (key === "text") node.textContent = value;
      else if (key.startsWith("on") && typeof value === "function") {
        node.addEventListener(key.slice(2), value);
      } else {
        node.setAttribute(key, value);
      }
    }
    for (const child of children) {
      if (child == null) continue;
      node.appendChild(typeof child === "string" ? document.createTextNode(child) : child);
    }
    return node;
  }

  // External links must go through the opener plugin — a raw <a target=_blank>
  // does nothing useful inside the webview.
  function openExternal(url) {
    const opener = window.__TAURI__ && window.__TAURI__.opener;
    if (!opener || typeof opener.openUrl !== "function") {
      console.warn("opener.openUrl unavailable; cannot open", url);
      return;
    }
    opener.openUrl(url).catch((err) => console.error("openUrl failed for", url, err));
  }

  function fmtTime(sec) {
    const s = Math.max(0, Math.floor(Number(sec) || 0));
    const m = Math.floor(s / 60);
    const ss = s % 60;
    return `${m}:${ss < 10 ? "0" : ""}${ss}`;
  }

  // --- generic "icon + title + body + action buttons" card -------------
  function view({ icon, title, body, actions }) {
    const card = el("section", { class: "card" }, [
      el("div", { class: "glyph", "aria-hidden": "true", text: icon }),
      el("h1", { text: title }),
      body ? el("p", { class: "body", text: body }) : null,
    ]);
    if (actions && actions.length) {
      const row = el("div", { class: "actions" });
      for (const action of actions) {
        row.appendChild(
          el("button", {
            class: "btn",
            type: "button",
            text: action.label,
            onclick: () => openExternal(action.url),
          }),
        );
      }
      card.appendChild(row);
    }
    return card;
  }

  // --- board-connected: now-playing card + settings ---------------------
  function renderNowPlaying(container, data) {
    container.replaceChildren();
    if (!data) {
      container.appendChild(el("p", { class: "muted", text: "Waiting for a track…" }));
      return;
    }

    const badgeText = data.ad_playing
      ? "Ad"
      : data.playback === "playing"
        ? "Playing"
        : data.playback === "buffering"
          ? "Buffering"
          : "Paused";

    container.appendChild(
      el("div", { class: "np-row" }, [
        el("div", { class: "np-cover", "aria-hidden": "true", text: "♪" }),
        el("div", { class: "np-meta" }, [
          el("div", { class: "np-badge", text: data.is_live ? "Live" : badgeText }),
          el("div", { class: "np-title", text: data.title || "Untitled" }),
          data.artist ? el("div", { class: "np-artist", text: data.artist }) : null,
          data.album ? el("div", { class: "np-album", text: data.album }) : null,
        ]),
        el("div", { class: "np-fav", "aria-hidden": "true", text: data.is_favorite ? "♥" : "" }),
      ]),
    );

    if (!data.is_live) {
      const durationSec = Number(data.duration_sec) || 0;
      const positionSec = Number(data.position_sec) || 0;
      const pct = durationSec > 0 ? Math.min(100, (positionSec / durationSec) * 100) : 0;
      container.appendChild(
        el("div", { class: "np-progress" }, [
          el("span", { class: "np-time", text: fmtTime(positionSec) }),
          el("div", { class: "np-bar" }, [el("div", { class: "np-fill", style: `width:${pct}%` })]),
          el("span", { class: "np-time", text: fmtTime(durationSec) }),
        ]),
      );
    }

    if (data.host_connected === false) {
      container.appendChild(
        el("div", { class: "np-warning", text: "Lost connection to YouTube Music Desktop…" }),
      );
    }
  }

  // TODO(Task 8): wire to invoke("get_autostart") / invoke("set_autostart", …).
  // The toggle is a disabled stub until that command exists.
  function autostartToggleStub() {
    return el("input", {
      type: "checkbox",
      id: "autostart-toggle",
      class: "toggle",
      disabled: "disabled",
      "aria-disabled": "true",
      title: "Coming soon",
    });
  }

  function connectedView() {
    const card = el("section", { class: "card connected" }, [
      el("div", { class: "glyph", "aria-hidden": "true", text: "\u{1F3A7}" }),
      el("h1", { text: "Board connected" }),
      el("p", { class: "body", text: "Streaming live playback to your board." }),
    ]);

    const nowPlaying = el("div", { class: "now-playing", id: "now-playing" });
    renderNowPlaying(nowPlaying, lastNowPlaying);
    card.appendChild(nowPlaying);

    card.appendChild(
      el("div", { class: "settings-row" }, [
        el("label", { class: "toggle-label", for: "autostart-toggle", text: "Launch at login" }),
        autostartToggleStub(),
      ]),
    );

    return card;
  }

  // --- one card per BridgeState -----------------------------------------
  function renderState(state) {
    switch (state) {
      case "ytmd-not-found":
        return view({
          icon: "\u{1F50C}",
          title: "YouTube Music Desktop isn’t running",
          body: "Start ytmdesktop and enable its Companion Server, then this window will pick it up automatically.",
          actions: [
            { label: "Download ytmdesktop", url: "https://ytmdesktop.app" },
            {
              label: "How to enable Companion Server",
              url: "https://github.com/ytmdesktop/ytmdesktop#companion-server",
            },
          ],
        });
      case "not-authorized":
        // The auth-code event (fired moments later by the same transition)
        // renders the real CTA via showAuthCode(); this is just the gap-filler
        // so the window never looks blank/broken while the code is requested.
        return view({
          icon: "\u{1F511}",
          title: "Authorizing…",
          body: "Requesting a pairing code from YouTube Music Desktop…",
        });
      case "waiting-for-board":
        return view({
          icon: "\u{1F6F0}️",
          title: "Waiting for your board…",
          body: "Authorized. Power on the board on the same Wi-Fi network.",
        });
      case "board-connected":
        return connectedView();
      case "ytmd-disconnected":
        return view({
          icon: "⚠️",
          title: "Lost YouTube Music Desktop",
          body: "Reconnecting…",
        });
      case "starting":
      default:
        return view({ icon: "⏳", title: "Starting…", body: "" });
    }
  }

  function showAuthCode(code) {
    // A repeat auth-code (e.g. the re-auth follow-up re-emitting one) is just
    // a refresh: rebuilding from scratch naturally shows the new code.
    const card = el("section", { class: "card auth" }, [
      el("div", { class: "glyph", "aria-hidden": "true", text: "\u{1F511}" }),
      el("h1", { text: "Authorize the board bridge" }),
      el("p", {
        class: "body",
        text: "Enter this code in YouTube Music Desktop → Settings → Integration → Companion Server.",
      }),
      el("div", { class: "auth-code", text: code }),
      el("div", { class: "actions" }, [
        el("button", {
          class: "btn",
          type: "button",
          text: "How to enable Companion Server",
          onclick: () => openExternal("https://github.com/ytmdesktop/ytmdesktop#companion-server"),
        }),
      ]),
    ]);
    app.replaceChildren(card);
  }

  function updateNowPlaying(data) {
    lastNowPlaying = data;
    const mount = document.getElementById("now-playing");
    if (mount) renderNowPlaying(mount, data);
  }

  function handleBridgeEvent(payload) {
    if (!payload || typeof payload !== "object") return;
    switch (payload.type) {
      case "state":
        app.replaceChildren(renderState(payload.data));
        break;
      case "auth-code":
        showAuthCode(payload.code);
        break;
      case "now-playing":
        updateNowPlaying(payload.data);
        break;
      case "log":
        console.debug("[bridge]", payload.data);
        break;
      default:
        console.warn("Unknown bridge-event payload", payload);
    }
  }

  function init() {
    const tauri = window.__TAURI__;
    if (!tauri || !tauri.event || typeof tauri.event.listen !== "function") {
      console.error("window.__TAURI__.event.listen is unavailable; is this running inside Tauri?");
      return;
    }
    tauri.event
      .listen("bridge-event", ({ payload }) => handleBridgeEvent(payload))
      .catch((err) => console.error("Failed to attach bridge-event listener", err));
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
