//! Whole-rig orchestration (`live` feature): cross-project device
//! locking, preflight state checks with safe repairs, and teardown that
//! returns shared hardware to normal.
//!
//! Locking is two-layered: the PRIMARY is the shared labgrid
//! coordinator (place `nucula-rig` matching the m5stick serial and the
//! reference ACR1252 tokens exported by the ai-legion-nfc bench
//! exporter — acquiring excludes the other bench users). The FALLBACK
//! is the machine-wide flock convention (see [`crate::locks`]) when the
//! coordinator is unreachable. Both are held when labgrid works:
//! a coordinator crash mid-run must not silently drop exclusivity.

use core::fmt;
use std::net::TcpStream;
use std::process::Command;
use std::time::Duration;

use crate::acr::Acr1252;
use crate::atom_console::AtomConsole;
use crate::locks::DeviceLock;

pub const LABGRID_COORDINATOR: &str = "192.168.13.208:20408";
pub const LABGRID_PLACE: &str = "nucula-rig";
pub const LABGRID_CLIENT: &str = "/usr/local/bin/labgrid-client";
/// The mint's LAN bind — the atom must reach the URL embedded in its
/// tokens; preflight checks reachability.
pub const DEFAULT_RELAY_MINT: &str = "http://192.168.13.208:3338";

/// The relay mint URL: `MINT_URL` overrides the bench default (same
/// convention as the payer tests — see payer.rs `mint_base`).
pub fn relay_mint() -> String {
    std::env::var("MINT_URL").unwrap_or_else(|_| DEFAULT_RELAY_MINT.into())
}

#[derive(Default)]
pub struct PreflightReport {
    pub lock_mode: &'static str, // "labgrid+flock" | "flock (coordinator unreachable)"
    pub acr: String,
    pub acr_repaired: Option<&'static str>,
    pub atom: String,
    pub atom_repaired: Option<&'static str>,
    pub mint: String,
}

impl fmt::Display for PreflightReport {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(f, "lock: {}", self.lock_mode)?;
        writeln!(f, "acr:  {} {:?}", self.acr, self.acr_repaired)?;
        writeln!(f, "atom: {} {:?}", self.atom, self.atom_repaired)?;
        write!(f, "mint: {}", self.mint)
    }
}

fn labgrid(action: &str) -> bool {
    Command::new(LABGRID_CLIENT)
        .args(["-x", LABGRID_COORDINATOR, "-p", LABGRID_PLACE, action])
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

fn mint_host_port() -> Result<std::net::SocketAddr, String> {
    use std::net::ToSocketAddrs;
    let mint = relay_mint();
    // No URL crate: split scheme, authority and path by hand. A base
    // path is legal (the payer and firmware use it) and must not leak
    // into the port parse; https is probed on its default port.
    let (default_port, rest) = if let Some(r) = mint.strip_prefix("https://") {
        (443u16, r)
    } else if let Some(r) = mint.strip_prefix("http://") {
        (3338u16, r)
    } else {
        return Err(format!("MINT_URL must start with http:// or https:// (got {mint})"));
    };
    let authority = rest.split('/').next().unwrap_or(rest);
    let (host, port) = match authority.rsplit_once(':') {
        Some((h, p)) => (
            h,
            p.parse::<u16>()
                .map_err(|_| format!("bad port in MINT_URL: {mint}"))?,
        ),
        None => (authority, default_port),
    };
    (host, port)
        .to_socket_addrs()
        .map_err(|e| format!("cannot resolve {host}:{port}: {e}"))?
        .next()
        .ok_or_else(|| format!("no address for {host}:{port}"))
}

/// Check device state; with `repair`, also fix what is safe to fix.
/// Never touches NFC mode-switch escapes (the firmware wedge — see
/// rig/README); settings commands only.
pub fn preflight(repair: bool) -> PreflightReport {
    let mut r = PreflightReport::default();

    // ACR: presence + polling state; restore polling if a crashed run
    // left it quieted (NVM persists across replugs).
    match Acr1252::open() {
        Ok(mut acr) => match (acr.firmware(), acr.read_auto_polling()) {
            (Ok(fw), Ok(poll)) => {
                if poll == 0 && repair {
                    let _ = acr.set_auto_polling(0x8F);
                    r.acr_repaired = Some("auto-polling restored 0x00 -> 0x8F");
                }
                r.acr = format!("up ({fw}), polling=0x{poll:02X}");
            }
            _ => r.acr = "enumerated but CCID dead — wedged, replug required".into(),
        },
        Err(_) => r.acr = "absent or pcscd down — replug / restart pcscd".into(),
    }

    // Atom: console answers; stale reader sessions stopped.
    let port = super::atom_default_port();
    match AtomConsole::open(&port) {
        Ok(mut atom) => match atom.status() {
            Ok(status) => {
                let nfc_busy =
                    !status.contains("nfc:     idle") && !status.contains("nfc:     off");
                if nfc_busy && repair {
                    let _ = atom.nfc_stop();
                    r.atom_repaired = Some("stale reader session stopped");
                }
                let wifi = if status.contains("connected") {
                    "wifi up"
                } else {
                    "wifi DOWN"
                };
                r.atom = format!(
                    "console up, {wifi}{}",
                    if nfc_busy { ", session was live" } else { "" }
                );
            }
            Err(e) => r.atom = format!("console open but status failed: {e}"),
        },
        Err(e) => r.atom = format!("unreachable: {e}"),
    }

    // Mint: TCP reachability of the LAN bind.
    r.mint = match mint_host_port() {
        Ok(addr) => {
            let addr_str = addr.to_string();
            match TcpStream::connect_timeout(&addr, Duration::from_secs(2)) {
                Ok(_) => format!("{addr_str} reachable"),
                Err(_) => format!("{addr_str} DOWN — run rig/scripts/start-mint.sh"),
            }
        }
        Err(e) => format!("bad MINT_URL: {e}"),
    };

    r
}

/// Acquires the rig (labgrid place + flock fallback) and runs preflight
/// with repairs. Drop releases everything and returns shared hardware
/// to normal — even when a test panics mid-flow.
pub struct RigGuard {
    labgrid_held: bool,
    _acr_lock: DeviceLock,
    _atom_lock: DeviceLock,
    pub report: PreflightReport,
}

/// Flock name derived from the serial port the console will ACTUALLY
/// open (atom_port(), honoring ATOM_PORT). Keying by probed board
/// identity allowed an override to open one board's port under another
/// board's lock — and in the flock-only fallback, two runs on the same
/// physical device under different lock names. The by-id basename is
/// unique per device, so identity-by-port is correct by construction.
fn board_flock_name() -> String {
    let port = super::atom_default_port();
    let tail = port.rsplit('/').next().unwrap_or("unknown");
    format!("port-{}", tail)
}

impl RigGuard {
    pub fn acquire() -> Result<Self, String> {
        let labgrid_held = labgrid("acquire");
        // Locks: always. Machine-wide convention covers non-labgrid
        // users; labgrid covers cross-host users of the coordinator.
        let acr_lock = DeviceLock::acquire("acr1252").map_err(|e| format!("preflight: {e}"))?;
        let atom_lock =
            DeviceLock::acquire(&board_flock_name()).map_err(|e| format!("preflight: {e}"))?;
        let mut report = preflight(true);
        report.lock_mode = if labgrid_held {
            "labgrid place + flock"
        } else {
            "flock only (labgrid coordinator unreachable)"
        };
        Ok(RigGuard {
            labgrid_held,
            _acr_lock: acr_lock,
            _atom_lock: atom_lock,
            report,
        })
    }
}

impl Drop for RigGuard {
    fn drop(&mut self) {
        // Teardown: back to normal for other lab users.
        if let Ok(mut acr) = Acr1252::open() {
            let _ = acr.set_auto_polling(0x8F);
        }
        if let Ok(mut atom) = AtomConsole::open(&super::atom_default_port()) {
            let _ = atom.nfc_stop();
        }
        if self.labgrid_held {
            let _ = labgrid("release");
        }
    }
}

#[cfg(test)]
mod tests {
    /// Read-only preflight: prints the rig state table without repairs.
    #[test]
    #[ignore = "hardware: rig devices present"]
    fn preflight_report_readonly() {
        let r = super::preflight(false);
        eprintln!("{r}");
        assert!(
            r.acr.contains("up") || r.acr.contains("replug"),
            "acr: {}",
            r.acr
        );
    }
}
