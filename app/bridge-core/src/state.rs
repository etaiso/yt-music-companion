//! The single source of truth the UI renders. Ports the state diagram in the
//! design doc: starting -> ytmd-not-found -> not-authorized -> waiting-for-board
//! -> board-connected, with ytmd-disconnected as a recoverable fallback.
//!
//! The catch the old linear `next_state(cur, sig)` got wrong: "is YouTube Music
//! Desktop's socket up" and "is a board attached" are ORTHOGONAL facts, but a
//! single linear state can only remember one at a time. So `Authorized` (which
//! fires on every socket connect *and reconnect*) reset an already-attached
//! board back to `WaitingForBoard`, and the board-attach vs socket-connect
//! events raced at startup (`tokio::select!` polls its arms in random order).
//! We now track the two facts independently and DERIVE the rendered state, so
//! one signal can never clobber the other.
use serde::Serialize;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum BridgeState {
    Starting,
    YtmdNotFound,
    NotAuthorized,
    WaitingForBoard,
    BoardConnected,
    YtmdDisconnected,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Signal {
    YtmdProbeFailed,
    ServerUpNoToken,
    Authorized,
    BoardAttached,
    BoardDetached,
    YtmdDropped,
}

/// How far the initial handshake has progressed. Only meaningful before we've
/// ever authorized; once `Authorized` lands we stay in `Authorized` and let the
/// two connection booleans decide the rendered state.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Phase {
    Starting,
    YtmdMissing,
    ServerUp,
    Authorized,
}

/// The two orthogonal connection facts plus handshake progress. `state()`
/// collapses them into the one `BridgeState` the UI renders.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Machine {
    phase: Phase,
    ytmd_up: bool,
    board_up: bool,
}

impl Default for Machine {
    fn default() -> Self {
        Self { phase: Phase::Starting, ytmd_up: false, board_up: false }
    }
}

impl Machine {
    pub fn new() -> Self {
        Self::default()
    }

    /// Fold a signal into the tracked facts. Board attach/detach is remembered
    /// across ytmd socket blips, and a socket reconnect (`Authorized`) never
    /// disturbs `board_up`.
    pub fn apply(&mut self, sig: Signal) {
        use Signal::*;
        match sig {
            // ytmd unreachable during the pre-auth probe loop.
            YtmdProbeFailed => {
                self.phase = Phase::YtmdMissing;
                self.ytmd_up = false;
            }
            ServerUpNoToken => self.phase = Phase::ServerUp,
            // Socket connected (initial or reconnect): we're authorized and the
            // host feed is live again. board_up is deliberately untouched.
            Authorized => {
                self.phase = Phase::Authorized;
                self.ytmd_up = true;
            }
            // Socket dropped mid-run: keep the authorized phase so recovery is a
            // plain `Authorized` again, but mark the feed down.
            YtmdDropped => self.ytmd_up = false,
            BoardAttached => self.board_up = true,
            BoardDetached => self.board_up = false,
        }
    }

    /// The linear state the frontend renders.
    pub fn state(&self) -> BridgeState {
        use BridgeState::*;
        match self.phase {
            Phase::Starting => Starting,
            Phase::YtmdMissing => YtmdNotFound,
            Phase::ServerUp => NotAuthorized,
            // Post-auth: the host feed being down outranks the board, since a
            // stale track with no live source is worse than "waiting".
            Phase::Authorized => {
                if !self.ytmd_up {
                    YtmdDisconnected
                } else if self.board_up {
                    BoardConnected
                } else {
                    WaitingForBoard
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{BridgeState::*, Signal::*, *};

    fn drive(sigs: &[Signal]) -> BridgeState {
        let mut m = Machine::new();
        for s in sigs {
            m.apply(*s);
        }
        m.state()
    }

    #[test]
    fn starts_in_starting() {
        assert_eq!(Machine::new().state(), Starting);
    }

    #[test]
    fn happy_path() {
        assert_eq!(drive(&[ServerUpNoToken]), NotAuthorized);
        assert_eq!(drive(&[ServerUpNoToken, Authorized]), WaitingForBoard);
        assert_eq!(drive(&[ServerUpNoToken, Authorized, BoardAttached]), BoardConnected);
    }

    #[test]
    fn probe_fail_goes_not_found_from_any_pre_auth_state() {
        assert_eq!(drive(&[YtmdProbeFailed]), YtmdNotFound);
        assert_eq!(drive(&[ServerUpNoToken, YtmdProbeFailed]), YtmdNotFound);
    }

    #[test]
    fn board_detach_returns_to_waiting() {
        assert_eq!(drive(&[Authorized, BoardAttached, BoardDetached]), WaitingForBoard);
    }

    #[test]
    fn ytmd_drop_is_recoverable_fallback() {
        assert_eq!(drive(&[Authorized, BoardAttached, YtmdDropped]), YtmdDisconnected);
        assert_eq!(drive(&[Authorized, YtmdDropped]), YtmdDisconnected);
        // recovery: socket comes back -> resume, board still attached
        assert_eq!(
            drive(&[Authorized, BoardAttached, YtmdDropped, Authorized]),
            BoardConnected
        );
    }

    // Regression: `Authorized` (fired on every socket reconnect) must NOT reset
    // an already-attached board back to WaitingForBoard. This is the #1 bug.
    #[test]
    fn reconnect_does_not_clobber_attached_board() {
        assert_eq!(
            drive(&[Authorized, BoardAttached, Authorized, Authorized]),
            BoardConnected
        );
    }

    // Regression: the startup order of the board-attach and socket-connect
    // signals is nondeterministic (tokio::select!). Either order must land on
    // BoardConnected — the crux of the #1 race.
    #[test]
    fn attach_then_authorize_and_reverse_agree() {
        assert_eq!(drive(&[BoardAttached, Authorized]), BoardConnected);
        assert_eq!(drive(&[Authorized, BoardAttached]), BoardConnected);
    }

    // Regression: ytmd going away must surface as disconnected even with a
    // board still attached — so the UI stops showing a stale track (#2).
    #[test]
    fn ytmd_down_outranks_attached_board() {
        assert_eq!(drive(&[Authorized, BoardAttached, YtmdDropped]), YtmdDisconnected);
    }

    #[test]
    fn serializes_kebab_case() {
        assert_eq!(serde_json::to_string(&YtmdNotFound).unwrap(), "\"ytmd-not-found\"");
        assert_eq!(serde_json::to_string(&WaitingForBoard).unwrap(), "\"waiting-for-board\"");
    }
}
