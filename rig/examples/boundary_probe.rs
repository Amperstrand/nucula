//! Boundary probe: drive the wallet balance toward the persistence-
//! failure boundary (fork docs said ~790 sat; the 2026-10-15 soak saw a
//! wallet-state reset at ~714) with wstat checkpoints after every few
//! receives, then report the final state. Run with the rig held.
//!
//! Env: NUCULA_PROBE_COUNT / NUCULA_PROBE_AMOUNT (defaults 18 x 42 =
//! 756 sat, just past the observed boundary).

use std::time::Duration;

use nucula_rig::atom_console::AtomConsole;

fn env_or(name: &str, default: u64) -> u64 {
    std::env::var(name)
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(default)
}

#[tokio::main]
async fn main() {
    let mint = nucula_rig::rig::relay_mint();
    let count = env_or("NUCULA_PROBE_COUNT", 18);
    let amount = env_or("NUCULA_PROBE_AMOUNT", 42);
    let _rig = nucula_rig::rig::RigGuard::acquire().expect("rig");

    let mut c = AtomConsole::open(&nucula_rig::atom_default_port()).expect("console");
    let _ = c.mint_add(&mint);

    let mut last_total: Option<u64> = None;
    let mut last_ckpt: u64 = 0;
    for i in 1..=count {
        let token = nucula_rig::payer::mint_token(&mint, amount)
            .await
            .expect("mint");
        for attempt in 0..3 {
            if let Ok(out) = c.cmd(&format!("receive {token}")) {
                if out.contains("received") {
                    break;
                }
            }
            if attempt == 2 {
                eprintln!("receive {i} incomplete after retries");
            }
            std::thread::sleep(Duration::from_millis(1500));
        }
        if i % 3 == 0 || i == count {
            let bal = c.cmd("balance").unwrap_or_default();
            let ws = c.cmd("wstat").unwrap_or_default();
            let total_line = bal.lines().find(|l| l.contains("total:"));
            let total: Option<u64> =
                total_line.and_then(|l| l.split_whitespace().find_map(|w| w.parse().ok()));
            let b = total_line.unwrap_or("?").trim();
            eprintln!("== after {i}x{amount} == {b}");
            if let (Some(prev), Some(now)) = (last_total, total) {
                // Expected total after every receive since the last
                // checkpoint. A reset can hide behind three successful
                // receives (now == prev) — compare against the EXPECTED
                // value, not just the previous one (Codex P2).
                let expected = prev + (i - last_ckpt) * amount;
                if now != expected {
                    eprintln!("   *** BOUNDARY EVENT: total {prev} -> {now}, expected {expected} (delta {} sats) ***",
                        now as i64 - expected as i64);
                }
            }
            // Keep the last VALID baseline: a truncated checkpoint must
            // not erase it, or a later reset goes unmarked.
            if total.is_some() {
                last_total = total;
                last_ckpt = i;
            }
            for l in ws
                .lines()
                .filter(|l| l.contains("saves=") || l.contains("nvs entries"))
            {
                eprintln!("   {}", l.trim());
            }
        }
    }
    let bal = c.cmd("balance").unwrap_or_default();
    let ws = c.cmd("wstat").unwrap_or_default();
    eprintln!("== FINAL ==");
    for l in bal.lines().filter(|l| l.contains("total:")) {
        eprintln!("{}", l.trim());
    }
    for l in ws
        .lines()
        .filter(|l| l.contains("saves=") || l.contains("nvs entries"))
    {
        eprintln!("{}", l.trim());
    }
}
