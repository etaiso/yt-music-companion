//! Map ytmdesktop /state -> board view-model (ports normalize.js).
//! Cover art is NOT in this JSON: the board protocol sends it as a separate
//! binary frame; `cover_url` is only a hint the orchestrator renders from.
use serde::Serialize;
use serde_json::Value;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Playback {
    Paused,
    Playing,
    Buffering,
}

#[derive(Debug, Clone, Serialize)]
pub struct NowPlayingVm {
    pub source_name: String,
    pub is_live: bool,
    pub track_id: String,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub ad_playing: bool,
    pub cover_url: Option<String>,
    pub playback: Playback,
    pub is_favorite: bool,
    pub position_sec: i64,
    pub duration_sec: i64,
    pub level: i64,
    pub host_connected: bool,
}

fn best_thumbnail(thumbs: &Value) -> Option<String> {
    let arr = thumbs.as_array()?;
    arr.iter()
        .max_by_key(|t| {
            let w = t.get("width").and_then(Value::as_i64).unwrap_or(0);
            let h = t.get("height").and_then(Value::as_i64).unwrap_or(0);
            w * h
        })
        .and_then(|t| t.get("url").and_then(Value::as_str))
        .map(str::to_owned)
}

fn playback_from(track_state: i64) -> Playback {
    match track_state {
        0 => Playback::Paused,
        1 => Playback::Playing,
        2 => Playback::Buffering,
        _ => Playback::Buffering, // -1 unknown -> buffering
    }
}

/// Returns `None` to mean "skip this update" — used while the player has no
/// metadata yet, so the board doesn't flicker partial data.
pub fn normalize(state: &Value, connected: bool) -> Option<NowPlayingVm> {
    let video = state.get("video").cloned().unwrap_or(Value::Null);
    let player = state.get("player").cloned().unwrap_or(Value::Null);

    let ad_playing = player.get("adPlaying").and_then(Value::as_bool).unwrap_or(false);

    // Wait for complete metadata before showing a track — but ads have no
    // metadata by design, so don't gate ad frames on it.
    let metadata_filled = video.get("metadataFilled").and_then(Value::as_bool);
    if !ad_playing && metadata_filled == Some(false) {
        return None;
    }

    let is_live = video.get("isLive").and_then(Value::as_bool).unwrap_or(false);
    let track_state = player.get("trackState").and_then(Value::as_i64).unwrap_or(-1);
    let progress = player.get("videoProgress").and_then(Value::as_f64).unwrap_or(0.0);
    let duration = video.get("durationSeconds").and_then(Value::as_f64).unwrap_or(0.0);

    let s = |v: &Value, k: &str| v.get(k).and_then(Value::as_str).unwrap_or("").to_owned();

    Some(NowPlayingVm {
        source_name: "YouTube Music".into(),
        is_live,
        track_id: s(&video, "id"),
        title: s(&video, "title"),
        artist: s(&video, "author"),
        album: s(&video, "album"),
        ad_playing,
        cover_url: best_thumbnail(video.get("thumbnails").unwrap_or(&Value::Null)),
        playback: playback_from(track_state),
        is_favorite: video.get("likeStatus").and_then(Value::as_i64) == Some(2),
        position_sec: progress.round() as i64,
        duration_sec: if is_live { 0 } else { duration.round() as i64 },
        level: 0, // ytmdesktop exposes no audio energy; ring falls back to pulse
        host_connected: connected,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn skips_when_metadata_not_filled_and_not_ad() {
        let state = json!({ "video": { "metadataFilled": false }, "player": {} });
        assert!(normalize(&state, true).is_none());
    }

    #[test]
    fn ad_frame_is_not_gated_on_metadata() {
        let state = json!({
            "video": { "metadataFilled": false },
            "player": { "adPlaying": true, "trackState": 1 }
        });
        let vm = normalize(&state, true).expect("ad frame should pass");
        assert!(vm.ad_playing);
        assert_eq!(vm.playback, Playback::Playing);
    }

    #[test]
    fn maps_core_fields_and_playback() {
        let state = json!({
            "video": {
                "id": "abc123",
                "title": "Creep",
                "author": "Radiohead",
                "album": "Pablo Honey",
                "durationSeconds": 238,
                "isLive": false,
                "likeStatus": 2,
                "thumbnails": [
                    { "url": "small", "width": 60, "height": 60 },
                    { "url": "big", "width": 544, "height": 544 }
                ]
            },
            "player": { "trackState": 1, "videoProgress": 30.7 }
        });
        let vm = normalize(&state, true).unwrap();
        assert_eq!(vm.track_id, "abc123");
        assert_eq!(vm.title, "Creep");
        assert_eq!(vm.artist, "Radiohead");
        assert_eq!(vm.album, "Pablo Honey");
        assert_eq!(vm.playback, Playback::Playing);
        assert!(vm.is_favorite);
        assert_eq!(vm.position_sec, 31); // rounded
        assert_eq!(vm.duration_sec, 238);
        assert_eq!(vm.cover_url.as_deref(), Some("big")); // largest thumbnail
        assert_eq!(vm.level, 0);
        assert!(vm.host_connected);
    }

    #[test]
    fn live_zeroes_duration() {
        let state = json!({
            "video": { "title": "x", "author": "y", "isLive": true, "durationSeconds": 999 },
            "player": { "trackState": 1 }
        });
        let vm = normalize(&state, true).unwrap();
        assert!(vm.is_live);
        assert_eq!(vm.duration_sec, 0);
    }

    #[test]
    fn unknown_trackstate_falls_back_to_buffering() {
        let state = json!({
            "video": { "title": "x", "author": "y" },
            "player": { "trackState": -1 }
        });
        assert_eq!(normalize(&state, true).unwrap().playback, Playback::Buffering);
    }

    #[test]
    fn serializes_playback_as_string() {
        let s = serde_json::to_string(&Playback::Buffering).unwrap();
        assert_eq!(s, "\"buffering\"");
    }
}
