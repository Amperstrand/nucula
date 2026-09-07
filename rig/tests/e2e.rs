//! Over-the-air e2e: a real token minted at a live micronuts-mint,
//! written onto a boltcard by the ACR1252U (reader mode), received by
//! the atom's Type 4 / ISO-DEP reader, redeemed back at the mint.
//! (Supersedes the Ultralight card-emulation e2e — the emulated area
//! caps at 256 addressable bytes and cannot carry DLEQ-bearing tokens;
//! see rig/README.md.)
//!
//! Hardware-gated (#[ignore]): the atom on a serial port, a WRITABLE
//! blank boltcard within reach of both antennas (on the atom's, ACR
//! adjacent), the ACR1252U freshly powered, pcscd running, and the
//! mint on its LAN bind (`/tmp/opencode/rig/start-mint.sh`).
//!
//!   cargo test --features live,payer --test e2e -- --ignored --nocapture
//!
//! Environment: ATOM_PORT (default: the lab atom's by-id path) and
//! MINT_URL (default: the mint's LAN bind — the atom must reach the
//! URL embedded in its tokens).

#![cfg(feature = "live")]

use std::time::Duration;

use nucula_rig::acr::Acr1252;
use nucula_rig::atom_console::AtomConsole;

fn atom_port() -> String {
    nucula_rig::atom_default_port()
}

fn mint_url() -> String {
    std::env::var("MINT_URL").unwrap_or_else(|_| nucula_rig::rig::DEFAULT_RELAY_MINT.into())
}

/// Rust serial-driver smoke: open the atom console and round-trip a
/// status command.
#[test]
#[ignore = "hardware: atom on the serial port"]
fn atom_console_smoke() {
    let mut atom = AtomConsole::open(&atom_port()).expect("atom console");
    let status = atom.status().expect("status");
    eprintln!("{status}");
    assert!(
        status.contains("nucula>") || status.contains("nfc:"),
        "no status output"
    );
}

/// Relay e2e: the payer mints 1 sat at the LAN mint, the ACR1252
/// writes the token's NDEF record onto the boltcard on the atom's
/// antenna, then — ACR field quieted — the atom reads it over Type 4 /
/// ISO-DEP and redeems.
#[cfg(feature = "payer")]
#[tokio::test]
#[ignore = "hardware: atom + writable boltcard + ACR1252U + LAN mint"]
async fn relay_token_over_the_air_via_boltcard() {
    let mint = mint_url();

    let token = nucula_rig::payer::mint_token(&mint, 1).await.expect("mint");
    assert!(token.starts_with("cashuA") || token.starts_with("cashuB"));
    // Cross-project rig lock + preflight (repairs state) + teardown.
    let _rig = nucula_rig::rig::RigGuard::acquire().expect("rig");
    eprintln!("{}", _rig.report);

    // ACR writes the relay card, then quiets its field for the atom.
    {
        let mut acr = Acr1252::open().expect("ACR direct");
        acr.set_auto_polling(0x8F).expect("polling on");
    }
    let mut card = nucula_rig::acr::Acr1252Card::connect().expect("card in field");
    let record = nucula_rig::ndef_t2t::build_ndef_text_record(&token);
    if let Err(e) = card.write_ndef(&record) {
        if matches!(e, nucula_rig::acr::CardError::Capacity { .. }) {
            panic!("{e} — swap in a bigger-NDEF boltcard (NTAG 424 DNA class)");
        }
        panic!("NDEF write failed: {e}");
    }
    card.disconnect();
    {
        let mut acr = Acr1252::open().expect("ACR direct");
        acr.set_auto_polling(0x00).expect("polling off");
    }
    let mut atom = AtomConsole::open(&atom_port()).expect("atom console");
    let _ = atom.nfc_stop();
    // The redeem needs the LAN mint reachable from the atom.
    let st = atom.status().expect("status");
    assert!(st.contains("connected"), "atom wifi down: {st}");
    let out = atom.mint_add(&mint).expect("mint add");
    assert!(!out.contains("error"), "mint add failed: {out}");
    atom.nfc_request(1).expect("nfc request");

    let log = atom
        .wait_for_log("redeemed", Duration::from_secs(60))
        .expect("no redeem log");
    eprintln!("{log}");
    let status = atom.status().expect("status");
    eprintln!("{status}");
}

/// RF smoke: the atom's NFC front-end against the card in the rig.
/// Asserts ISO14443-3 up to anticollision within one reader session —
/// REQA answered and at least one anticollision round completed (the
/// observed rates: REQA near-100% after the conductance boost,
/// anticollision ~25/120s; the 9-byte SELECT is coupling-limited by the
/// sandwich layout, tracked separately). Proves the I2C driver, field
/// gating, REQA/WUPA, and bit-framed anticollision live on hardware.
#[test]
#[ignore = "hardware: atom + card on its antenna"]
fn atom_rf_iso14443_smoke() {
    let mut atom = AtomConsole::open(&atom_port()).expect("atom console");
    let _ = atom.nfc_stop();
    let _ = atom.cmd("log d rc522");
    atom.nfc_request(1).expect("nfc request");

    let log = atom
        .wait_for_log("REQA: ATQA=", Duration::from_secs(30))
        .expect("no REQA answer in 30s");
    eprintln!("REQA up: {}", log.lines().last().unwrap_or(""));

    let log2 = atom
        .wait_for_log("anticoll lvl=0 ok=1", Duration::from_secs(120))
        .expect("no completed anticollision in 120s");
    eprintln!("anticollision completed");
    assert!(log2.contains("anticoll lvl=0 ok=1"));
    let _ = atom.nfc_stop();
}

/// Console-relay e2e: the full money loop minus the RF hop. The payer
/// mints a real token at the LAN mint and the atom receives it via its
/// serial console (`receive`) — exercising everything the over-the-air
/// relay would: V4 token parse, keyset fetch over WiFi, NUT-12-verified
/// swap at the mint, balance. Run whenever the atom is on serial and
/// the mint is up; isolates the (physical) RF coupling question from
/// the wallet correctness question.
#[cfg(feature = "payer")]
#[tokio::test]
#[ignore = "hardware: atom on serial + mint on the LAN bind"]
async fn console_relay_money_loop() {
    use nucula_rig::atom_console::AtomConsole;
    let mint = mint_url();
    let token = nucula_rig::payer::mint_token(&mint, 1).await.expect("mint");

    let _rig = nucula_rig::rig::RigGuard::acquire().expect("rig");
    let mut atom = AtomConsole::open(&atom_port()).expect("atom console");
    let _ = atom.nfc_stop();
    let out = atom.mint_add(&mint).expect("mint add");
    assert!(!out.contains("error"), "mint add failed: {out}");

    let recv = atom.cmd(&format!("receive {token}")).expect("receive");
    eprintln!("{recv}");
    assert!(
        recv.contains("swapping"),
        "receive did not reach the swap: {recv}"
    );
    assert!(!recv.contains("warning"), "receive warned: {recv}");

    let balance = atom.cmd("balance").expect("balance");
    eprintln!("{balance}");
    assert!(
        balance.contains("1 sat"),
        "balance missing the received sat: {balance}"
    );
}

/// ACR-as-tag relay e2e: the payer mints 1 sat at the LAN mint, the
/// token is DLEQ-stripped to fit the 256-byte emulated NDEF area, and
/// the ACR1252 presents it as an emulated Type 2 tag (polling quieted
/// before the mode switch, CE exited, image written + verified,
/// emulation re-entered). The nucula reader board then reads the
/// emulated tag over RF and redeems the token — the full money loop
/// with no physical card in the loop.
#[cfg(feature = "payer")]
#[tokio::test]
#[ignore = "hardware: ACR1252U + nucula reader board + LAN mint"]
async fn relay_acr_emulated() {
    let mint = mint_url();
    let token = nucula_rig::payer::mint_token(&mint, 1).await.expect("mint");
    let stripped = nucula_rig::strip::strip_dleq(&token).expect("dleq strip");
    eprintln!(
        "token: {} chars -> {} after dleq strip",
        token.len(),
        stripped.len()
    );
    let image = nucula_rig::ndef_t2t::build_ndef_text_image(&stripped, 256).expect("ndef image");
    eprintln!("ndef image: {} bytes", image.len());

    let _rig = nucula_rig::rig::RigGuard::acquire().expect("rig");
    eprintln!("{}", _rig.report);

    {
        let mut acr = Acr1252::open().expect("ACR direct");
        acr.present_ndef_image(&image).expect("present NDEF image");
    }

    let mut atom = AtomConsole::open(&atom_port()).expect("console");
    let _ = atom.nfc_stop();
    let st = atom.status().expect("status");
    assert!(st.contains("connected"), "wifi down: {st}");
    let out = atom.mint_add(&mint).expect("mint add");
    assert!(!out.contains("error"), "mint add failed: {out}");

    let before = sat_total(&atom.cmd("balance").expect("balance before"));
    atom.nfc_request(1).expect("nfc request");
    let log = atom
        .wait_for_log("received", Duration::from_secs(60))
        .expect("no receive log");
    eprintln!("{log}");
    let _ = atom.nfc_stop();

    let balance = atom.cmd("balance").expect("balance after");
    eprintln!("{balance}");
    let after = sat_total(&balance);
    assert_eq!(
        after,
        before + 1,
        "balance did not gain the relayed sat: {balance}"
    );
}

/// Sum the "N sat" amounts in a `balance` command's output.
fn sat_total(balance: &str) -> u64 {
    balance
        .lines()
        .filter_map(|l| l.trim().strip_suffix(" sat"))
        .filter_map(|l| l.split_whitespace().next())
        .filter_map(|n| n.parse::<u64>().ok())
        .sum()
}

/// Offline relay variant: same hand-off, but the atom is out of AP
/// range (or the AP is down) after having once fetched the mint's
/// keysets — the token is stashed, then drained automatically once
/// the link returns.
#[cfg(feature = "payer")]
#[tokio::test]
#[ignore = "hardware + manual: atom offline after keyset fetch; reconnect after the tap"]
async fn relay_stashes_offline_then_drains_on_reconnect() {
    let mint = mint_url();
    let token = nucula_rig::payer::mint_token(&mint, 1).await.expect("mint");
    // Cross-project rig lock + preflight (repairs state) + teardown.
    let _rig = nucula_rig::rig::RigGuard::acquire().expect("rig");
    eprintln!("{}", _rig.report);

    {
        let mut acr = Acr1252::open().expect("ACR direct");
        acr.set_auto_polling(0x8F).expect("polling on");
    }
    let mut card = nucula_rig::acr::Acr1252Card::connect().expect("card in field");
    let record = nucula_rig::ndef_t2t::build_ndef_text_record(&token);
    card.write_ndef(&record).expect("NDEF write");
    card.disconnect();
    {
        let mut acr = Acr1252::open().expect("ACR direct");
        acr.set_auto_polling(0x00).expect("polling off");
    }
    let mut atom = AtomConsole::open(&atom_port()).expect("atom console");
    let _ = atom.nfc_stop();
    atom.nfc_request(1).expect("nfc request");
    let log = atom
        .wait_for_log("stashed", Duration::from_secs(60))
        .expect("no stash log");
    eprintln!("{log}");

    // Bring the atom back to the AP (manual); the drain task redeems.
    let drained = atom
        .wait_for_log("drain", Duration::from_secs(180))
        .expect("no drain log");
    eprintln!("{drained}");
}

/// Diagnostic: which CP210x control-line state lets the console talk?
/// Tries (dtr, rts) combinations, writing CR LF and reading raw.
#[test]
#[ignore = "hardware: atom on the serial port"]
fn console_line_probe() {
    use std::io::{Read, Write};
    let path = atom_port();
    for (dtr, rts) in [(true, true), (true, false), (false, true), (false, false)] {
        let mut p = serialport::new(&path, 115_200)
            .timeout(Duration::from_millis(200))
            .open()
            .expect("open");
        let _ = p.write_data_terminal_ready(dtr);
        let _ = p.write_request_to_send(rts);
        std::thread::sleep(Duration::from_millis(300));
        let _ = p.write_all(b"\r\n");
        let _ = p.flush();
        let mut got = Vec::new();
        let mut buf = [0u8; 256];
        let end = std::time::Instant::now() + Duration::from_secs(2);
        while std::time::Instant::now() < end {
            match p.read(&mut buf) {
                Ok(n) => got.extend_from_slice(&buf[..n]),
                Err(e) if e.kind() == std::io::ErrorKind::TimedOut => {}
                Err(_) => break,
            }
        }
        eprintln!(
            "dtr={dtr} rts={rts} -> {} bytes: {:?}",
            got.len(),
            String::from_utf8_lossy(&got)
        );
        drop(p);
        std::thread::sleep(Duration::from_millis(300));
    }
}
