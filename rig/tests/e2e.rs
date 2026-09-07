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

const DEFAULT_ATOM_PORT: &str =
    "/dev/serial/by-id/usb-M5STACK_Inc._M5_Serial_Converter_9D529068B4-if00-port0";
/// The relay default: the mint's LAN bind — the atom must reach the
/// URL embedded in its tokens.
const DEFAULT_RELAY_MINT: &str = "http://192.168.13.221:3338";

fn atom_port() -> String {
    std::env::var("ATOM_PORT").unwrap_or_else(|_| DEFAULT_ATOM_PORT.into())
}

fn mint_url() -> String {
    std::env::var("MINT_URL").unwrap_or_else(|_| DEFAULT_RELAY_MINT.into())
}

/// Rust serial-driver smoke: open the atom console and round-trip a
/// status command.
#[test]
#[ignore = "hardware: atom on the serial port"]
fn atom_console_smoke() {
    let mut atom = AtomConsole::open(&atom_port()).expect("atom console");
    let status = atom.status().expect("status");
    eprintln!("{status}");
    assert!(status.contains("nucula>") || status.contains("nfc:"), "no status output");
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

    // ACR writes the relay card, then quiets its field for the atom.
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
    // Restores polling (0x8F) on drop — even on a failed assertion.
    let _polling_guard = nucula_rig::acr::PollingRestoreOnDrop;

    let mut atom = AtomConsole::open(&atom_port()).expect("atom console");
    let _ = atom.nfc_stop();
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
    let _polling_guard = nucula_rig::acr::PollingRestoreOnDrop;

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
        eprintln!("dtr={dtr} rts={rts} -> {} bytes: {:?}",
                  got.len(), String::from_utf8_lossy(&got));
        drop(p);
        std::thread::sleep(Duration::from_millis(300));
    }
}
