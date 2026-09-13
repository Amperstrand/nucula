//! Boundary probe: drive the wallet balance toward the ~790-sat
//! persistence-failure boundary with wstat checkpoints after every few
//! receives, then report the final state. Run with the rig held.

use std::time::Duration;

use nucula_rig::atom_console::AtomConsole;

#[tokio::main]
async fn main() {
    let mint = "http://192.168.13.221:3338";
    let _rig = nucula_rig::rig::RigGuard::acquire().expect("rig");

    let mut c = AtomConsole::open(&nucula_rig::atom_default_port()).expect("console");
    let _ = c.mint_add(mint);

    for i in 1..=10u32 {
        let token = nucula_rig::payer::mint_token(mint, 42).await.expect("mint");
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
        if i % 3 == 0 {
            let bal = c.cmd("balance").unwrap_or_default();
            let ws = c.cmd("wstat").unwrap_or_default();
            let b = bal.lines().find(|l| l.contains("total:")).unwrap_or("?").trim();
            eprintln!("== after {i}x42 == {b}");
            for l in ws.lines().filter(|l| l.contains("saves=") || l.contains("nvs entries")) {
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
    for l in ws.lines().filter(|l| l.contains("saves=") || l.contains("nvs entries")) {
        eprintln!("{}", l.trim());
    }
}
