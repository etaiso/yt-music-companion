//! The single source of truth the UI renders. Ports the state diagram in the
//! design doc: starting -> ytmd-not-found -> not-authorized -> waiting-for-board
//! -> board-connected, with ytmd-disconnected as a recoverable fallback.
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

pub fn next_state(cur: BridgeState, sig: Signal) -> BridgeState {
    use BridgeState::*;
    use Signal::*;
    match sig {
        // ytmd server unreachable takes priority from any pre-connected state.
        YtmdProbeFailed => YtmdNotFound,
        ServerUpNoToken => NotAuthorized,
        // Authorized resumes the pipeline whether starting fresh or recovering.
        Authorized => WaitingForBoard,
        BoardAttached => BoardConnected,
        BoardDetached => WaitingForBoard,
        // Server vanished mid-run: fall back so the UI shows disconnected while
        // the client retries. Preserve BoardConnected-vs-Waiting is irrelevant —
        // both collapse to the recoverable fallback.
        YtmdDropped => {
            let _ = cur;
            YtmdDisconnected
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{BridgeState::*, Signal::*, *};

    #[test]
    fn happy_path() {
        assert_eq!(next_state(Starting, ServerUpNoToken), NotAuthorized);
        assert_eq!(next_state(NotAuthorized, Authorized), WaitingForBoard);
        assert_eq!(next_state(WaitingForBoard, BoardAttached), BoardConnected);
    }

    #[test]
    fn probe_fail_goes_not_found_from_any_pre_auth_state() {
        assert_eq!(next_state(Starting, YtmdProbeFailed), YtmdNotFound);
        assert_eq!(next_state(NotAuthorized, YtmdProbeFailed), YtmdNotFound);
    }

    #[test]
    fn board_detach_returns_to_waiting() {
        assert_eq!(next_state(BoardConnected, BoardDetached), WaitingForBoard);
    }

    #[test]
    fn ytmd_drop_is_recoverable_fallback() {
        assert_eq!(next_state(BoardConnected, YtmdDropped), YtmdDisconnected);
        assert_eq!(next_state(WaitingForBoard, YtmdDropped), YtmdDisconnected);
        // recovery: server comes back authorized -> resume waiting-for-board
        assert_eq!(next_state(YtmdDisconnected, Authorized), WaitingForBoard);
    }

    #[test]
    fn serializes_kebab_case() {
        assert_eq!(serde_json::to_string(&YtmdNotFound).unwrap(), "\"ytmd-not-found\"");
        assert_eq!(serde_json::to_string(&WaitingForBoard).unwrap(), "\"waiting-for-board\"");
    }
}
