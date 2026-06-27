// normalize.js — map ytmdesktop /state -> board view-model (SPEC §5).
//
// Output keys mirror now_playing_vm_t (ui/now_playing_vm.h). `playback` is a
// string token the board maps to playback_t. Cover art is NOT in this JSON: the
// board protocol (§6) sends the cover as a separate binary frame; we expose
// `cover_url` only as a hint for the (deferred) cover-art pass / diagnostics.

const PLAYBACK = {
  0: "paused", // PB_PAUSED
  1: "playing", // PB_PLAYING
  2: "buffering", // PB_BUFFERING
  // -1 unknown -> buffering (SPEC §5)
};

// Pick the largest thumbnail (closest to the cover slot, downscaled later).
function bestThumbnail(thumbs) {
  if (!Array.isArray(thumbs) || thumbs.length === 0) return null;
  return thumbs.reduce((best, t) =>
    (t?.width ?? 0) * (t?.height ?? 0) > (best?.width ?? 0) * (best?.height ?? 0)
      ? t
      : best,
  )?.url ?? null;
}

// Returns a vm object, or `null` to mean "skip this update" — used while the
// player has no metadata yet (SPEC §4.3, §8: don't flicker partial data).
export function normalize(state, { connected = true } = {}) {
  const player = state?.player ?? {};
  const video = state?.video ?? {};

  const adPlaying = Boolean(player.adPlaying);

  // Wait for complete metadata before showing a track — but ads have no metadata
  // by design, so don't gate ad frames on it.
  if (!adPlaying && video.metadataFilled === false) return null;

  const isLive = Boolean(video.isLive);

  return {
    source_name: "YouTube Music",
    is_live: isLive,

    title: video.title ?? "",
    artist: video.author ?? "",
    album: video.album ?? "",
    ad_playing: adPlaying,

    cover_url: bestThumbnail(video.thumbnails),

    playback: PLAYBACK[player.trackState] ?? "buffering",
    is_favorite: video.likeStatus === 2,

    position_sec: Math.round(player.videoProgress ?? 0),
    duration_sec: isLive ? 0 : Math.round(video.durationSeconds ?? 0),

    level: 0, // ytmdesktop exposes no audio energy; ring falls back to pulse (SPEC §5)

    host_connected: connected,
  };
}
