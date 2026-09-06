//! Over-the-air e2e: real token, minted at a live micronuts-mint,
//! presented to the atom through the ACR1252U's Ultralight emulation,
//! received by the ported reader-mode wallet, redeemed back at the mint.
//!
//! Hardware-gated (#[ignore]): needs the atom on a serial port, the
//! ACR1252U freshly powered (card emulation is one-way per power
//! cycle — replug between runs), pcscd running, and the mint:
//!
//!   cd micronuts && MICRONUTS_ADAPTER_PORT=3338 \
//!     setsid ./target/debug/micronuts-audit-adapter </dev/null &
//!
//! Then:
//!
//!   cargo test --features live,payer --test e2e -- --ignored --nocapture
//!
//! Environment: ATOM_PORT (default: the lab atom's by-id path) and
//! MINT_URL (default http://127.0.0.1:3338).

#![cfg(feature = "live")]

use std::time::Duration;

use nucula_rig::acr::Acr1252;
use nucula_rig::atom_console::AtomConsole;
use nucula_rig::ndef_t2t::{build_ndef_text_image, NDEF_AREA_512};

/// The ACR1252U's Write Card Emulation Data takes a one-byte StartOffset,
/// capping the addressable emulated image at 256 bytes. A 1-sat cashuB
/// token (single proof, CBOR+base64) lands around 200 bytes and fits;
/// anything multi-proof does not. Verify before burning the one CE
/// entry this reader has per power cycle.
const CE_IMAGE_LIMIT: usize = 256;

const DEFAULT_ATOM_PORT: &str =
    "/dev/serial/by-id/usb-M5STACK_Inc._M5_Serial_Converter_9D529068B4-if00-port0";
const DEFAULT_MINT_URL: &str = "http://127.0.0.1:3338";

fn atom_port() -> String {
    std::env::var("ATOM_PORT").unwrap_or_else(|_| DEFAULT_ATOM_PORT.into())
}

fn mint_url() -> String {
    std::env::var("MINT_URL").unwrap_or_else(|_| DEFAULT_MINT_URL.into())
}

/// Rust serial-driver smoke: open the atom console and round-trip a
/// status command (the python prototypes drove it so far; this
/// exercises AtomConsole itself).
#[test]
#[ignore = "hardware: atom on the serial port"]
fn atom_console_smoke() {
    std::thread::sleep(Duration::from_millis(300));
    let mut atom = AtomConsole::open(&atom_port()).expect("atom console");
    let status = atom.status().expect("status");
    eprintln!("{status}");
    assert!(status.contains("nucula>") || status.contains("nfc:"), "no status output");
}

#[cfg(feature = "payer")]
#[tokio::test]
#[ignore = "hardware: atom + freshly replugged ACR1252U + running micronuts-mint"]
async fn receives_a_real_token_over_the_air_and_redeems_it() {
    let mint = mint_url();

    // Payer: real ecash from the running mint. Single proof, 1 sat —
    // must fit the emulated tag (see CE_IMAGE_LIMIT).
    let token = nucula_rig::payer::mint_token(&mint, 1).await.expect("mint");
    assert!(token.starts_with("cashuA") || token.starts_with("cashuB"));

    // Atom: add the mint and start an NFC reader session.
    let mut atom = AtomConsole::open(&atom_port()).expect("atom console");
    let _ = atom.nfc_stop();
    let out = atom.mint_add(&mint).expect("mint add");
    assert!(!out.contains("error"), "mint add failed: {out}");
    atom.nfc_request(1).expect("nfc request");

    // ACR: preload the token as a Type 2 NDEF tag and start emulating.
    // ONE-WAY per power cycle — keep last so a failing earlier step
    // does not burn the reader.
    let image = build_ndef_text_image(&token, NDEF_AREA_512).expect("image");
    assert!(image.len() <= CE_IMAGE_LIMIT,
            "token image {} B exceeds the CE addressable area", image.len());
    let mut acr = Acr1252::open().expect("ACR1252");
    acr.present_ndef_image(&image).expect("CE preload");

    // Atom: token read over the air, swapped at the mint, balance up.
    let log = atom
        .wait_for_log("redeemed", Duration::from_secs(45))
        .expect("no redeem log");
    eprintln!("{}", log);
    let status = atom.status().expect("status");
    eprintln!("{status}");
    assert!(status.contains("connected"), "atom offline: {status}");
}

#[cfg(feature = "payer")]
#[tokio::test]
#[ignore = "hardware + manual: boot the atom OUT of WiFi range (or with the AP down) after it has once fetched this mint's keysets"]
async fn stashes_a_token_offline_then_drains_on_reconnect() {
    let mint = mint_url();

    // Precondition (manual): the atom has been online with this mint
    // before, so it holds keysets and accepts offline tokens from it.
    let token = nucula_rig::payer::mint_token(&mint, 1).await.expect("mint");

    let mut atom = AtomConsole::open(&atom_port()).expect("atom console");
    let _ = atom.nfc_stop();
    atom.nfc_request(1).expect("nfc request");

    let image = build_ndef_text_image(&token, NDEF_AREA_512).expect("image");
    assert!(image.len() <= CE_IMAGE_LIMIT, "token image too big for CE");
    let mut acr = Acr1252::open().expect("ACR1252");
    acr.present_ndef_image(&image).expect("CE preload");

    let log = atom
        .wait_for_log("stashed", Duration::from_secs(45))
        .expect("no stash log");
    eprintln!("{}", log);
    assert!(log.contains("offline: stashed") || log.contains("stashed"));

    // Reconnect: bring the atom back to the AP (manual), then the
    // drain task redeems automatically; balance reflects it.
    let drained = atom
        .wait_for_log("drain", Duration::from_secs(120))
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
        eprintln!("dtr={dtr} rts={rts} -> {} bytes: {:?}", got.len(),
                  String::from_utf8_lossy(&got));
        drop(p);
        std::thread::sleep(Duration::from_millis(300));
    }
}
