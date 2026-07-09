//! Native-Rust port of the ytmdesktop → board bridge (was bridge/src/*.js).
pub mod auth;
pub mod board_server;
pub mod bridge;
pub mod commands;
pub mod config;
pub mod cover;
pub mod discovery;
pub mod normalize;
pub mod state;
pub mod ytmd;

pub use bridge::{run, BridgeEvent};
pub use normalize::{NowPlayingVm, Playback};
pub use state::BridgeState;
