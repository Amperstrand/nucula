fn main() {
    for (i, t) in ["cashuAeyJ2IjoxfQ==", "hello world", ""].iter().enumerate() {
        if let Ok(img) = nucula_rig::ndef_t2t::build_ndef_text_image(t, 512) {
            std::fs::write(format!("/tmp/nucula-fz/rig/fuzz/corpus/fuzz_parse_ndef_text/seed{}", i), img).unwrap();
        }
    }
}
