//! mDNS advertisement so the board finds the host (ports discovery.js).
//! Advertises `_ytmboard._tcp` on the board port; the board browses for this
//! service and connects to the advertised host:port — no hard-coded IP.
use crate::config::board_port;
use anyhow::{Context, Result};
use mdns_sd::{ServiceDaemon, ServiceInfo};

const SERVICE_TYPE: &str = "_ytmboard._tcp.local.";
const SERVICE_NAME: &str = "YT Music board bridge";

pub struct Discovery {
    daemon: ServiceDaemon,
}

pub fn start() -> Result<Discovery> {
    let daemon = ServiceDaemon::new().context("mdns ServiceDaemon::new")?;
    let service = ServiceInfo::new(
        SERVICE_TYPE,
        SERVICE_NAME,
        "ytmboard.local.",
        "",
        board_port(),
        &[("proto", "ws"), ("path", "/"), ("v", "1")][..],
    )
    .context("mdns ServiceInfo::new")?
    .enable_addr_auto();
    daemon.register(service).context("mdns register")?;
    tracing::info!("[mdns] advertising {SERVICE_TYPE} on :{}", board_port());
    Ok(Discovery { daemon })
}

impl Discovery {
    pub fn stop(self) {
        // Send goodbye packets so boards drop us promptly, then shut down.
        let _ = self.daemon.unregister(&format!("{SERVICE_NAME}.{SERVICE_TYPE}"));
        let _ = self.daemon.shutdown();
    }
}
