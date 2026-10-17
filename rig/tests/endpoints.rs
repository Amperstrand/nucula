//! Endpoint resolution tests (issue #14). Pure precedence/parsing logic —
//! no features, no hardware. The `show` fixtures are captured verbatim from
//! the live coordinator (ai-legion 192.168.13.208:20408) on 2026-10-17.

use nucula_rig::endpoints::{
    mint_from_comment, parse_place_comment, resolve_with_env, BenchEndpoints, Source,
    FALLBACK_COORDINATOR, FALLBACK_MINT,
};

/// `nucula-rig` today: created without a comment — the comment line is
/// absent entirely.
const SHOW_NO_COMMENT: &str = "\
Place 'nucula-rig':
  matches:
    ai-legion-nfc/m5stick-usb/SerialPort
    ai-legion-nfc/ref-acr1252/USBDevice
  acquired: None
  acquired resources:
  created: 2026-10-15 01:00:19.193310
  changed: 2026-10-16 05:04:58.748537
Matching resource 'SerialPort' (ai-legion-nfc/m5stick-usb/SerialPort/SerialPort):
  {'acquired': None,
   'avail': True,
";

/// A place with a single-line comment (ap-lan2 shape).
const SHOW_SINGLE_LINE: &str = "\
Place 'ap-lan2':
  comment: bench port lan2
  matches: [\"*/ap-lan2/*\"]
  acquired: None
";

/// The ccid-nucula shape: prose carrying an endpoint (the OTA push host)
/// WITHOUT a `mint=` directive — prose must not be mistaken for one.
const SHOW_CCID_NUCULA: &str = "\
Place 'ccid-nucula':
  comment: nucula (ESP32-C3 + PN7160) OTA seat: acquire to flash via 192.168.13.230:3232 (host-tools/ota_push.py); console token exported, never auto-matched (DTR resets board)
  matches: [\"*/ccid-nucula/*\"]
  acquired: None
";

#[test]
fn comment_parsing_shapes() {
    assert_eq!(parse_place_comment(SHOW_NO_COMMENT), None);
    assert_eq!(
        parse_place_comment(SHOW_SINGLE_LINE).as_deref(),
        Some("bench port lan2")
    );
    let ccid = parse_place_comment(SHOW_CCID_NUCULA).unwrap();
    assert!(ccid.contains("192.168.13.230:3232"));
}

#[test]
fn mint_directive_extraction() {
    assert_eq!(
        mint_from_comment("nucula rig seat mint=http://192.168.13.208:3338 ota=x"),
        Some("http://192.168.13.208:3338")
    );
    // prose endpoint, no directive
    assert_eq!(
        mint_from_comment("OTA seat: acquire to flash via 192.168.13.230:3232"),
        None
    );
    // directive without an absolute URL is not a mint endpoint
    assert_eq!(mint_from_comment("mint=localhost:3338"), None);
    assert_eq!(mint_from_comment(""), None);
}

#[test]
fn precedence_env_over_registry_over_fallback() {
    // nothing set: fallbacks
    let e = resolve_with_env(None, None, None);
    assert_eq!(
        e,
        BenchEndpoints {
            coordinator: FALLBACK_COORDINATOR.into(),
            coordinator_source: Source::Fallback,
            mint: FALLBACK_MINT.into(),
            mint_source: Source::Fallback,
        }
    );

    // registry beats fallback for the mint
    let e = resolve_with_env(Some("mint=http://10.9.9.9:1 x=y"), None, None);
    assert_eq!(e.mint, "http://10.9.9.9:1");
    assert_eq!(e.mint_source, Source::Registry);
    // ...but never for the coordinator (bootstrap endpoint)
    assert_eq!(e.coordinator_source, Source::Fallback);

    // env beats everything
    let e = resolve_with_env(
        Some("mint=http://10.9.9.9:1"),
        Some("192.0.2.1:20408"),
        Some("http://192.0.2.2:3338"),
    );
    assert_eq!(e.coordinator, "192.0.2.1:20408");
    assert_eq!(e.mint, "http://192.0.2.2:3338");
    assert!(matches!(e.coordinator_source, Source::Env(_)));
    assert!(matches!(e.mint_source, Source::Env(_)));

    // a comment without a directive is not an error: fallback
    let e = resolve_with_env(Some("just prose about the seat"), None, None);
    assert_eq!(e.mint_source, Source::Fallback);
}

/// Live, read-only: resolve against the real coordinator and print the
/// sources. Run explicitly: `cargo test --test endpoints live_registry -- --ignored --nocapture`.
#[test]
#[ignore = "touches the live coordinator (read-only show)"]
fn live_registry_resolve() {
    let probe = resolve_with_env(None, None, None);
    let comment = nucula_rig::endpoints::place_comment(&probe.coordinator, "nucula-rig");
    let e = nucula_rig::endpoints::resolve(comment.as_deref());
    println!("registry comment: {comment:?}");
    println!("coordinator: {} [{}]", e.coordinator, e.coordinator_source.as_str());
    println!("mint:         {} [{}]", e.mint, e.mint_source.as_str());
}
