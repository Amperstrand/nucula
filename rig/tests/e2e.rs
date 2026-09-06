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

const DEFAULT_ATOM_PORT: &str =
    "/dev/serial/by-id/usb-M5STACK_Inc._M5_Serial_Converter_9D529068B4-if00-port0";
const DEFAULT_MINT_URL: &str = "http://127.0.0.1:3338";

fn atom_port() -> String {
    std::env::var("ATOM_PORT").unwrap_or_else(|_| DEFAULT_ATOM_PORT.into())
}

fn mint_url() -> String {
    std::env::var("MINT_URL").unwrap_or_else(|_| DEFAULT_MINT_URL.into())
}

#[cfg(feature = "payer")]
#[tokio::test]
#[ignore = "hardware: atom + freshly replugged ACR1252U + running micronuts-mint"]
async fn receives_a_real_token_over_the_air_and_redeems_it() {
    let mint = mint_url();

    // Payer: real ecash from the running mint.
    let token = nucula_rig::payer::mint_token(&mint, 21).await.expect("mint");
    assert!(token.starts_with("cashuA") || token.starts_with("cashuB"));

    // Atom: add the mint and start an NFC reader session.
    let mut atom = AtomConsole::open(&atom_port()).expect("atom console");
    let _ = atom.nfc_stop();
    let out = atom.mint_add(&mint).expect("mint add");
    assert!(!out.contains("error"), "mint add failed: {out}");
    atom.nfc_request(21).expect("nfc request");

    // ACR: preload the token as a Type 2 NDEF tag and start emulating.
    // ONE-WAY per power cycle — keep last so a failing earlier step
    // does not burn the reader.
    let image = build_ndef_text_image(&token, NDEF_AREA_512).expect("image");
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
    let token = nucula_rig::payer::mint_token(&mint, 21).await.expect("mint");

    let mut atom = AtomConsole::open(&atom_port()).expect("atom console");
    let _ = atom.nfc_stop();
    atom.nfc_request(21).expect("nfc request");

    let image = build_ndef_text_image(&token, NDEF_AREA_512).expect("image");
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
