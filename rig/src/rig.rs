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

/// COMPILED FALLBACKS ONLY (issue #14): the live values come from the lab
/// registry when it is reachable — see [`resolve_endpoints`]. These are the
/// last bench layout (ai-legion .208) and go stale on every migration
/// (.221→.208 was hand-edited); treat a run on them as "registry missing".
pub const LABGRID_COORDINATOR: &str = "192.168.13.208:20408";
pub const LABGRID_PLACE: &str = "nucula-rig";
pub const LABGRID_CLIENT: &str = "/usr/local/bin/labgrid-client";
/// The mint's LAN bind — the atom must reach the URL embedded in its
/// tokens; preflight checks reachability.
pub const DEFAULT_RELAY_MINT: &str = "http://192.168.13.208:3338";

/// Where an endpoint value came from — preflight reports it so a run on
/// stale compiled fallbacks is visible, not silent.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum EndpointSource {
    EnvOverride,
    Registry,
    CompiledFallback,
}

impl EndpointSource {
    fn as_str(self) -> &'static str {
        match self {
            EndpointSource::EnvOverride => "env override",
            EndpointSource::Registry => "lab registry",
            EndpointSource::CompiledFallback => "COMPILED FALLBACK (registry unreachable)",
        }
    }
}

/// One resolved endpoint and its provenance.
pub struct Endpoint {
    pub value: String,
    pub source: EndpointSource,
}

/// Bench endpoints for a run (#14): coordinator + mint, resolved with a
/// strict precedence — direct env override (`LABGRID_COORDINATOR` /
/// `MINT_URL`), then the conwrt lab registry
/// (`$CONWRT_BENCH/scripts/lab_registry.py endpoint <role>`, one
/// `host[:port]`-shaped line on stdout; `mint` carries the full URL),
/// then the compiled fallbacks above. Resolution failures fall back —
/// a rig run must not be blocked by a missing registry — but the
/// preflight report names the source so the stale case is loud.
pub fn resolve_endpoints() -> (Endpoint, Endpoint) {
    let bench = std::env::var("CONWRT_BENCH").ok().or_else(|| {
        std::env::var("CONWRT_LAB")
            .ok()
            .map(|lab| format!("{lab}/../conwrt-bench"))
    });
    let from_registry = |role: &str| -> Option<String> {
        let bench = bench.as_ref()?;
        let out = Command::new("python3")
            .arg(format!("{bench}/scripts/lab_registry.py"))
            .args(["endpoint", role])
            .output()
            .ok()?;
        if !out.status.success() {
            return None;
        }
        String::from_utf8(out.stdout)
            .ok()?
            .lines()
            .map(str::trim)
            .find(|l| !l.is_empty())
            .map(str::to_string)
    };
    let endpoint = |env_var: &str, role: &str, fallback: &str| {
        if let Some(v) = std::env::var(env_var).ok().filter(|v| !v.trim().is_empty()) {
            return Endpoint { value: v.trim().to_string(), source: EndpointSource::EnvOverride };
        }
        if let Some(v) = from_registry(role) {
            return Endpoint { value: v, source: EndpointSource::Registry };
        }
        Endpoint { value: fallback.to_string(), source: EndpointSource::CompiledFallback }
    };
    (
        endpoint("LABGRID_COORDINATOR", "coordinator", LABGRID_COORDINATOR),
        endpoint("MINT_URL", "mint", DEFAULT_RELAY_MINT),
    )
}

/// The relay mint URL (see [`resolve_endpoints`]; kept for callers that
/// only need the value — `MINT_URL` > registry > compiled fallback).
pub fn relay_mint() -> String {
    resolve_endpoints().1.value
}

#[derive(Default)]
pub struct PreflightReport {
    pub lock_mode: &'static str, // "labgrid+flock" | "flock (coordinator unreachable)"
    pub acr: String,
    pub acr_repaired: Option<&'static str>,
    pub atom: String,
    pub atom_repaired: Option<&'static str>,
    pub mint: String,
    /// #14: which source the coordinator/mint came from — a run on the
    /// compiled fallbacks must be visible, not silent.
    pub endpoints: String,
}

impl fmt::Display for PreflightReport {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(f, "lock: {}", self.lock_mode)?;
        writeln!(f, "acr:  {} {:?}", self.acr, self.acr_repaired)?;
        writeln!(f, "atom: {} {:?}", self.atom, self.atom_repaired)?;
        writeln!(f, "mint: {}", self.mint)?;
        write!(f, "endpoints: {}", self.endpoints)
    }
}

fn labgrid(action: &str, coordinator: &str) -> bool {
    Command::new(LABGRID_CLIENT)
        .args(["-x", coordinator, "-p", LABGRID_PLACE, action])
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
        (80u16, r)
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
    let (coord, mint_ep) = resolve_endpoints();
    r.endpoints = format!(
        "coordinator={} [{}], mint={} [{}]",
        coord.value,
        coord.source.as_str(),
        mint_ep.value,
        mint_ep.source.as_str()
    );

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
    coordinator: String,
    labgrid_held: bool,
    _acr_lock: DeviceLock,
    _port_lock: DeviceLock,
    /// Legacy board-named lock, held ALONGSIDE the port-derived one so a
    /// rig of the previous generation overlapping this one still excludes
    /// on the shared name (Codex P1: identity drift between versions).
    _legacy_board_lock: DeviceLock,
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

/// The pre-port-identity lock name (e.g. `atom-9d529068b4`), kept so
/// overlapping rig generations still exclude each other.
fn legacy_board_flock_name() -> String {
    let board = super::detect_board();
    format!("{}-{}", board.name(), board.lock_id())
}

impl RigGuard {
    pub fn acquire() -> Result<Self, String> {
        let (coord, mint_ep) = resolve_endpoints();
        let labgrid_held = labgrid("acquire", &coord.value);
        // Locks: always. Machine-wide convention covers non-labgrid
        // users; labgrid covers cross-host users of the coordinator.
        let acr_lock = DeviceLock::acquire("acr1252").map_err(|e| format!("preflight: {e}"))?;
        let port_lock =
            DeviceLock::acquire(&board_flock_name()).map_err(|e| format!("preflight: {e}"))?;
        let legacy_board_lock =
            DeviceLock::acquire(&legacy_board_flock_name()).map_err(|e| format!("preflight: {e}"))?;
        let mut report = preflight(true);
        report.lock_mode = if labgrid_held {
            "labgrid place + flock"
        } else {
            "flock only (labgrid coordinator unreachable)"
        };
        Ok(RigGuard {
            coordinator: coord.value.clone(),
            labgrid_held,
            _acr_lock: acr_lock,
            _port_lock: port_lock,
            _legacy_board_lock: legacy_board_lock,
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
            let _ = labgrid("release", &self.coordinator);
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

#[cfg(test)]
mod endpoint_tests {
    use super::*;
    use std::fs;
    use std::path::PathBuf;

    /// #14's contract: env override wins, the registry (a fixture
    /// lab_registry.py standing in for CONWRT_BENCH) is consulted next,
    /// and with neither the compiled fallbacks are used — each with its
    /// provenance. One test, sequential phases: the phases mutate process
    /// env, so they must not run in parallel with themselves.
    #[test]
    fn endpoints_env_registry_fallback_precedence() {
        let tmp = std::env::temp_dir().join(format!("nucula-registry-{}", std::process::id()));
        let scripts = tmp.join("scripts");
        fs::create_dir_all(&scripts).unwrap();
        let reg = scripts.join("lab_registry.py");
        fs::write(
            &reg,
            "#!/usr/bin/env python3\nimport sys\nprint({'coordinator': '10.9.9.9:20408', 'mint': 'http://10.9.9.9:3338'}[sys.argv[2]])\n",
        )
        .unwrap();

        // phase 1: registry only
        std::env::set_var("CONWRT_BENCH", &tmp);
        std::env::remove_var("LABGRID_COORDINATOR");
        std::env::remove_var("MINT_URL");
        let (c, m) = resolve_endpoints();
        assert_eq!(c.value, "10.9.9.9:20408");
        assert_eq!(c.source, EndpointSource::Registry);
        assert_eq!(m.value, "http://10.9.9.9:3338");
        assert_eq!(m.source, EndpointSource::Registry);

        // phase 2: env beats the registry
        std::env::set_var("LABGRID_COORDINATOR", "10.1.1.1:20408");
        std::env::set_var("MINT_URL", "http://10.1.1.1:3338");
        let (c, m) = resolve_endpoints();
        assert_eq!(c.value, "10.1.1.1:20408");
        assert_eq!(c.source, EndpointSource::EnvOverride);
        assert_eq!(m.value, "http://10.1.1.1:3338");
        assert_eq!(m.source, EndpointSource::EnvOverride);

        // phase 3: broken registry + no env -> compiled fallback, loudly labelled
        std::env::remove_var("LABGRID_COORDINATOR");
        std::env::remove_var("MINT_URL");
        std::env::set_var("CONWRT_BENCH", tmp.join("nonexistent"));
        let (c, m) = resolve_endpoints();
        assert_eq!(c.value, LABGRID_COORDINATOR);
        assert_eq!(c.source, EndpointSource::CompiledFallback);
        assert_eq!(m.value, DEFAULT_RELAY_MINT);
        assert_eq!(m.source, EndpointSource::CompiledFallback);
        assert!(c.source.as_str().contains("registry unreachable"));

        std::env::remove_var("CONWRT_BENCH");
        let _ = fs::remove_dir_all(&tmp);
        let _ = PathBuf::new();
    }
}
