//! Overnight soak: full-token money loops against the LAN mint, larger
//! amounts included, with reboot-persistence checks and heap telemetry.
//!
//!   cargo run --features live,payer --example soak -- [--hours 8]
//!
//! Holds the rig (BenchLock + labgrid place + device flocks) for the
//! whole run. Every cycle: mint <amount> at the LAN mint (FULL token,
//! NUT-12 intact — no stripping), `receive` it on the device, verify the
//! balance delta. Every 10th cycle: heap/tasks snapshot. Every 25th:
//! reboot the device and verify NVS balance persistence. Failures are
//! logged and recovered from (nfc_stop + reboot); five consecutive
//! failures abort. Results: rig/results/soak-<ts>.jsonl.

use std::io::Write;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use nucula_rig::atom_console::AtomConsole;
use nucula_rig::rig::RigGuard;

// Console-transport ceiling (measured 2026-09-13): tokens for 100+ sats
// (~1.5 KB lines) complete the swap on-device but break the console
// output stream deterministically — see nucula issue tracker. The loop
// stays at console-proven amounts; every 50th cycle probes 100 sats to
// track that bug and exercise the reboot-recovery path.
const AMOUNTS: [u64; 5] = [1, 2, 5, 21, 42];
const LARGE_PROBE_EVERY: u32 = 50;
const LARGE_PROBE_AMOUNT: u64 = 100;
const SNAPSHOT_EVERY: u32 = 10;
const REBOOT_EVERY: u32 = 25;
const MAX_CONSECUTIVE_FAILURES: u32 = 5;

fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_secs()
}

fn sat_total(balance: &str) -> u64 {
    balance
        .lines()
        .find(|l| l.trim_start_matches(' ').starts_with("total: "))
        .and_then(|l| l.split_whitespace().nth(1))
        .and_then(|n| n.parse().ok())
        .unwrap_or(0)
}

struct Soak {
    log: std::fs::File,
    cycles: u32,
    failures: u32,
    consecutive: u32,
    recovered: u32,
    reboot_checks: u32,
}

impl Soak {
    fn event(&mut self, kind: &str, fields: &[(&str, String)]) {
        let mut line = format!(
            r#"{{"ts":{},"cycle":{},"kind":"{}""#,
            now(),
            self.cycles,
            kind
        );
        for (k, v) in fields {
            line.push_str(&format!(r#","{k}":"{}""#, json_escape(v)));
        }
        line.push_str("}\n");
        let _ = self.log.write_all(line.as_bytes());
        let _ = self.log.flush();
        eprintln!("{line}");
    }
}

fn json_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for ch in s.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out
}

/// Recover the console via the proven pyserial path (hardware boot-line
/// pulse + banner + wifi-joined status). The serialport crate's RTS
/// toggle does not reach this bridge — do NOT inline this in Rust.
fn recover_console(timeout_secs: u64) -> Option<AtomConsole> {
    let script =
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("scripts/recover_console.py");
    let ok = std::process::Command::new("python3")
        .args([script.to_str().unwrap(), &timeout_secs.to_string()])
        .status()
        .map(|s| s.success())
        .unwrap_or(false);
    if ok {
        AtomConsole::open(&nucula_rig::atom_default_port()).ok()
    } else {
        None
    }
}

fn wait_console(timeout: Duration) -> Option<AtomConsole> {
    let end = Instant::now() + timeout;
    while Instant::now() < end {
        if let Ok(mut c) = AtomConsole::open(&nucula_rig::atom_default_port()) {
            if let Ok(st) = c.status() {
                if st.contains("connected") {
                    return Some(c);
                }
            }
        }
        std::thread::sleep(Duration::from_secs(3));
    }
    None
}

/// Run a console command with retries — the Hades2001 bridge sporadically
/// drops a read chunk (readiness-but-no-data), which must not fail a cycle.
fn cmd_retry(c: &mut AtomConsole, line: &str) -> Result<String, String> {
    let mut last = String::new();
    for attempt in 0..3 {
        match c.cmd(line) {
            Ok(out) => return Ok(out),
            Err(e) => {
                last = format!("{e}");
                std::thread::sleep(Duration::from_millis(1500 + 500 * attempt as u64));
            }
        }
    }
    Err(format!("cmd {line:?} failed 3x: {last}"))
}

fn read_balance(c: &mut AtomConsole) -> Result<u64, String> {
    for _ in 0..3 {
        if let Ok(b) = cmd_retry(c, "balance") {
            if b.contains("total:") {
                return Ok(sat_total(&b));
            }
        }
        std::thread::sleep(Duration::from_millis(1000));
    }
    Err("balance output unreadable after retries".into())
}

fn reboot_and_verify(expected: u64) -> Result<AtomConsole, String> {
    std::thread::sleep(Duration::from_secs(6));
    let mut c2 = wait_console(Duration::from_secs(90)).ok_or("console never came back")?;
    let got = read_balance(&mut c2)?;
    if got != expected {
        return Err(format!("balance not persistent: {got} != {expected}"));
    }
    Ok(c2)
}

#[tokio::main]
async fn main() {
    let hours: u64 = std::env::args()
        .nth(1)
        .or_else(|| std::env::var("NUCULA_SOAK_HOURS").ok())
        .and_then(|s| s.trim_start_matches("--hours ").parse().ok())
        .unwrap_or(8);
    let mint =
        std::env::var("MINT_URL").unwrap_or_else(|_| nucula_rig::rig::DEFAULT_RELAY_MINT.into());

    let results = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("results");
    std::fs::create_dir_all(&results).expect("results dir");
    let log_path = results.join(format!("soak-{}.jsonl", time_stamp()));
    let mut soak = Soak {
        log: std::fs::File::create(&log_path).expect("log file"),
        cycles: 0,
        failures: 0,
        consecutive: 0,
        recovered: 0,
        reboot_checks: 0,
    };
    soak.event(
        "start",
        &[
            ("hours", hours.to_string()),
            ("log", format!("{log_path:?}")),
        ],
    );

    let _rig = RigGuard::acquire().expect("rig");
    soak.event("rig", &[("report", _rig.report.to_string())]);

    let deadline = Instant::now() + Duration::from_secs(hours * 3600);
    let mut console = wait_console(Duration::from_secs(60)).expect("console at soak start");
    let _ = console.mint_add(&mint);
    // Crash-consistency forensics: capture save_proofs success/failure
    // lines (INFO/ERROR on wallet_nvs) that the WARN default hides.
    let _ = console.cmd("log i wallet_nvs");

    while Instant::now() < deadline {
        soak.cycles += 1;
        let amount = if soak.cycles % LARGE_PROBE_EVERY == 0 {
            LARGE_PROBE_AMOUNT
        } else {
            AMOUNTS[(soak.cycles as usize - 1) % AMOUNTS.len()]
        };
        let t0 = Instant::now();

        let result = run_cycle(&mut console, &mint, amount).await;
        match result {
            Ok(()) => {
                soak.consecutive = 0;
                // Balance per cycle: dense samples make wallet-reset
                // boundaries (crash-consistency forensics) unambiguous.
                let bal = read_balance(&mut console).unwrap_or(0);
                soak.event(
                    "cycle",
                    &[
                        ("amount", amount.to_string()),
                        ("balance", bal.to_string()),
                        ("secs", format!("{:.2}", t0.elapsed().as_secs_f32())),
                    ],
                );
            }
            Err(err) => {
                soak.failures += 1;
                soak.consecutive += 1;
                soak.event(
                    "fail",
                    &[("amount", amount.to_string()), ("error", err.clone())],
                );
                if soak.consecutive >= MAX_CONSECUTIVE_FAILURES {
                    soak.event("abort", &[("reason", "consecutive failures".into())]);
                    drop(_rig);
                    std::process::exit(1);
                }
                // Recovery: hardware reset + health check via the proven
                // pyserial path (the in-Rust pulse does not reach the bridge).
                match recover_console(150).or_else(|| wait_console(Duration::from_secs(60))) {
                    Some(c) => {
                        console = c;
                        let _ = console.mint_add(&mint);
                        soak.recovered += 1;
                        soak.event("recovered", &[]);
                    }
                    None => {
                        soak.event("abort", &[("reason", "console lost after reboot".into())]);
                        drop(_rig);
                        std::process::exit(1);
                    }
                }
            }
        }

        // Pacing: the flaky bridge degrades under dense console traffic.
        std::thread::sleep(Duration::from_secs(5));

        if soak.cycles % SNAPSHOT_EVERY == 0 {
            if let (Ok(heap), Ok(tasks)) = (console.cmd("heap"), console.cmd("tasks")) {
                soak.event(
                    "snapshot",
                    &[
                        ("heap", heap.trim().to_string()),
                        ("tasks", tasks.trim().to_string()),
                    ],
                );
            }
        }

        if soak.cycles % REBOOT_EVERY == 0 {
            if let Ok(b) = console.cmd("balance") {
                let expected = sat_total(&b);
                let _ = console.cmd("reboot");
                match reboot_and_verify(expected) {
                    Ok(c) => {
                        console = c;
                        let _ = console.mint_add(&mint);
                        soak.reboot_checks += 1;
                        soak.event(
                            "reboot_check",
                            &[("balance", expected.to_string()), ("ok", "true".into())],
                        );
                    }
                    Err(e) => {
                        soak.failures += 1;
                        soak.event("reboot_check", &[("ok", "false".into()), ("error", e)]);
                        if let Some(c) =
                            recover_console(150).or_else(|| wait_console(Duration::from_secs(60)))
                        {
                            console = c;
                            let _ = console.mint_add(&mint);
                        }
                    }
                }
            }
        }
    }

    soak.event(
        "done",
        &[
            ("cycles", soak.cycles.to_string()),
            ("failures", soak.failures.to_string()),
            ("recovered", soak.recovered.to_string()),
            ("reboot_checks", soak.reboot_checks.to_string()),
        ],
    );
}

async fn run_cycle(c: &mut AtomConsole, mint: &str, amount: u64) -> Result<(), String> {
    let before = read_balance(c)?;

    let token = nucula_rig::payer::mint_token(mint, amount)
        .await
        .map_err(|e| format!("mint {amount}: {e}"))?;

    let mut recv = String::new();
    let mut recv_err = String::new();
    for _ in 0..2 {
        recv = cmd_retry(c, &format!("receive {token}"))?;
        if recv.contains("received") {
            break;
        }
        // The bridge can drop the body while the prompt survives — retry
        // once before declaring the receive itself broken.
        recv_err = recv.lines().last().unwrap_or("").to_string();
        recv.clear();
        std::thread::sleep(Duration::from_millis(2000));
    }
    if !recv.contains("received") {
        return Err(format!("receive did not complete: {recv_err}"));
    }

    let after = read_balance(c)?;
    if after != before + amount {
        return Err(format!("balance {after} != {before}+{amount}"));
    }
    Ok(())
}

fn time_stamp() -> String {
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_secs();
    let (h, m, s) = ((now / 3600) % 24, (now / 60) % 60, now % 60);
    format!("{:02}{:02}{:02}", h, m, s)
}
