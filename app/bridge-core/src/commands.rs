//! Board command -> ytmdesktop command mapping (ports index.js mapCommand).
use crate::normalize::NowPlayingVm;
use serde_json::{json, Value};

#[derive(Debug, Clone, PartialEq)]
pub struct YtmdCommand {
    pub command: &'static str,
    pub data: Option<Value>,
}

pub fn map_command(cmd: &str, arg: Option<f64>, last_vm: Option<&NowPlayingVm>) -> Option<YtmdCommand> {
    let simple = |command| Some(YtmdCommand { command, data: None });
    match cmd {
        "toggle_play" => simple("playPause"),
        "next" => simple("next"),
        "prev" => simple("previous"),
        "toggle_favorite" => simple("toggleLike"),
        "volume_up" => simple("volumeUp"),
        "volume_down" => simple("volumeDown"),
        "seek" => {
            let raw = arg.unwrap_or(0.0);
            let dur = last_vm.map(|v| v.duration_sec).unwrap_or(0);
            let sec = if dur > 0 {
                raw.max(0.0).min(dur as f64)
            } else {
                raw.max(0.0)
            };
            Some(YtmdCommand { command: "seekTo", data: Some(json!(sec as i64)) })
        }
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::normalize::Playback;

    fn vm(duration: i64) -> NowPlayingVm {
        NowPlayingVm {
            source_name: "YouTube Music".into(), is_live: false, track_id: String::new(),
            title: String::new(), artist: String::new(), album: String::new(), ad_playing: false,
            cover_url: None, playback: Playback::Playing, is_favorite: false,
            position_sec: 0, duration_sec: duration, level: 0, host_connected: true,
        }
    }

    #[test]
    fn simple_commands_map_by_name() {
        assert_eq!(map_command("toggle_play", None, None).unwrap().command, "playPause");
        assert_eq!(map_command("next", None, None).unwrap().command, "next");
        assert_eq!(map_command("prev", None, None).unwrap().command, "previous");
        assert_eq!(map_command("toggle_favorite", None, None).unwrap().command, "toggleLike");
        assert_eq!(map_command("volume_up", None, None).unwrap().command, "volumeUp");
        assert_eq!(map_command("volume_down", None, None).unwrap().command, "volumeDown");
    }

    #[test]
    fn seek_clamps_to_duration() {
        let m = map_command("seek", Some(500.0), Some(&vm(238))).unwrap();
        assert_eq!(m.command, "seekTo");
        assert_eq!(m.data, Some(json!(238)));
    }

    #[test]
    fn seek_floors_at_zero() {
        let m = map_command("seek", Some(-5.0), Some(&vm(238))).unwrap();
        assert_eq!(m.data, Some(json!(0)));
    }

    #[test]
    fn seek_without_duration_passes_through_nonnegative() {
        let m = map_command("seek", Some(42.0), None).unwrap();
        assert_eq!(m.data, Some(json!(42)));
    }

    #[test]
    fn unknown_command_is_none() {
        assert!(map_command("launch_rockets", None, None).is_none());
    }
}
