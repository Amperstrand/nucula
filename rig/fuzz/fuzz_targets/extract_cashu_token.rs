#![no_main]
use libfuzzer_sys::fuzz_target;
use nucula_rig::ndef_t2t::extract_cashu_token;

fuzz_target!(|data: &[u8]| {
    if let Ok(s) = std::str::from_utf8(data) {
        let _ = extract_cashu_token(s);
    }
});
