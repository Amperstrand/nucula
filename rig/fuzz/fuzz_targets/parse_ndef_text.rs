#![no_main]
//! Fuzz the NDEF Type-2 text-record parser over raw tag images — the exact
//! surface where nucula's T2T page-cache bug lived (found only when the
//! first real tag was read; nfcdump bypassed it). Hostile CC values, lying
//! TLV lengths, 0xFF extended forms, terminator-first layouts.
use libfuzzer_sys::fuzz_target;
use nucula_rig::ndef_t2t::parse_ndef_text;

fuzz_target!(|data: &[u8]| {
    let _ = parse_ndef_text(data);
});
