//! NDEF Type 2 tag image building and parsing.
//!
//! Mirrors what the atom's reader-mode frontend expects and what the
//! ACR1252U card emulation actually serves (validated on hardware): the
//! reader keeps its own UID in pages 0-2 and serves the preloaded data
//! area from page 3 — so an image is exactly a T2T image minus the
//! UID/lock pages: Capability Container first, then the NDEF TLV.

use core::fmt;

pub const NDEF_AREA_512: usize = 512;
const CC_LEN: usize = 4;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ImageError {
    /// The text record does not fit the NDEF area after CC + TLV header.
    RecordTooLong { needed: usize, area: usize },
}

impl fmt::Display for ImageError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ImageError::RecordTooLong { needed, area } => {
                write!(f, "NDEF record needs {needed} bytes, area is {area}")
            }
        }
    }
}

impl std::error::Error for ImageError {}

/// One NFC Forum Text record ("en", UTF-8) carrying `text`. MB|ME set;
/// short form (1-byte length) under 256 payload bytes, 4-byte length
/// above. Shared by the Type 2 image builder and the Type 4 file
/// writer.
pub fn build_ndef_text_record(text: &str) -> Vec<u8> {
    let mut payload = Vec::with_capacity(text.len() + 3);
    payload.push(0x02); // status: UTF-8, 2-byte language code
    payload.extend_from_slice(b"en");
    payload.extend_from_slice(text.as_bytes());

    let mut record = Vec::with_capacity(payload.len() + 7);
    if payload.len() < 0x100 {
        record.extend_from_slice(&[0xD1, 0x01, payload.len() as u8, 0x54]);
    } else {
        record.extend_from_slice(&[0x51, 0x01]);
        record.extend_from_slice(&(payload.len() as u32).to_be_bytes());
        record.push(0x54);
    }
    record.extend_from_slice(&payload);
    record
}

/// Build a Type 2 NDEF image carrying `text` as an NFC Forum Text
/// record, padded to `area` bytes total (CC included).
///
/// The CC advertises the full area as read/write NDEF memory. TLV
/// lengths of 254+ use the three-byte form (`03 FF hi lo`).
pub fn build_ndef_text_image(text: &str, area: usize) -> Result<Vec<u8>, ImageError> {
    assert!(area >= 8 && area <= 0xFF * 8, "area must be 8..2040 bytes");

    let record = build_ndef_text_record(text);

    let mut tlv = Vec::with_capacity(record.len() + 4);
    tlv.push(0x03); // NDEF message TLV
    if record.len() >= 0xFF {
        tlv.extend_from_slice(&[0xFF, (record.len() >> 8) as u8, record.len() as u8]);
    } else {
        tlv.push(record.len() as u8);
    }
    tlv.extend_from_slice(&record);
    tlv.push(0xFE); // terminator TLV

    if CC_LEN + tlv.len() > area {
        return Err(ImageError::RecordTooLong {
            needed: CC_LEN + tlv.len(),
            area,
        });
    }

    let mut image = Vec::with_capacity(area);
    image.extend_from_slice(&[0xE1, 0x40, (area / 8) as u8, 0x00]);
    image.extend_from_slice(&tlv);
    image.resize(area, 0x00);
    Ok(image)
}

/// Parse a Type 2 NDEF image back into its Text record payload.
pub fn parse_ndef_text(image: &[u8]) -> Option<String> {
    if image.len() < CC_LEN || image[0] != 0xE1 {
        return None;
    }
    let mut i = CC_LEN;
    loop {
        let t = *image.get(i)?;
        match t {
            0x00 => i += 1, // padding
            0xFE => return None,
            0x03 => break,
            _ => return None,
        }
    }
    i += 1;
    let len = match *image.get(i)? {
        0xFF => {
            let hi = *image.get(i + 1)? as usize;
            let lo = *image.get(i + 2)? as usize;
            i += 3;
            (hi << 8) | lo
        }
        l => {
            i += 1;
            l as usize
        }
    };
    let msg = image.get(i..i + len)?;
    parse_text_record(msg)
}

fn parse_text_record(msg: &[u8]) -> Option<String> {
    // Well-known 'T' with MB|ME; short form has a 4-byte header,
    // the long form a 7-byte header (4-byte big-endian length).
    let (hdr, plen) = match (msg.first()?, msg.get(1)?) {
        (0xD1, 0x01) => (4usize, *msg.get(2)? as usize),
        (0x51, 0x01) => {
            let b = [*msg.get(2)?, *msg.get(3)?, *msg.get(4)?, *msg.get(5)?];
            (7usize, u32::from_be_bytes(b) as usize)
        }
        _ => return None,
    };
    if msg.get(hdr - 1) != Some(&0x54) {
        return None;
    }
    let payload = msg.get(hdr..hdr + plen)?;
    let lang_len = (payload[0] & 0x3F) as usize;
    let text = payload.get(1 + lang_len..)?;
    Some(String::from_utf8_lossy(text).into_owned())
}

/// Extract a cashu token from free text, mirroring the device-side
/// extraction: a bare leading `cashuA`/`cashuB`, a `#token=` URL
/// fragment, a `token=` parameter, or the first embedded run.
pub fn extract_cashu_token(text: &str) -> Option<&str> {
    for prefix in ["cashuA", "cashuB"] {
        if let Some(rest) = text.strip_prefix(prefix) {
            let end = rest
                .find(|c: char| !c.is_ascii_alphanumeric() && c != '-' && c != '_' && c != '=')
                .unwrap_or(rest.len());
            return Some(&text[..prefix.len() + end]);
        }
        if let Some(frag) = text.find(&format!("#token={prefix}")) {
            return extract_cashu_token(&text[frag + 7..]);
        }
        if let Some(par) = text.find(&format!("token={prefix}")) {
            return extract_cashu_token(&text[par + 6..]);
        }
        if let Some(pos) = text.find(prefix) {
            return extract_cashu_token(&text[pos..]);
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    const TOKEN: &str = "cashuAUt4stFakeToken0000";

    #[test]
    fn image_layout_matches_device_expectations() {
        let img = build_ndef_text_image(TOKEN, NDEF_AREA_512).unwrap();
        assert_eq!(img.len(), 512);
        assert_eq!(&img[..4], &[0xE1, 0x40, 0x40, 0x00]); // CC, 512-byte area
        assert_eq!(img[4], 0x03); // NDEF TLV
        assert_eq!(img[5] as usize, 0x1F); // record length
        assert_eq!(&img[6..10], &[0xD1, 0x01, 0x1B, 0x54]); // text record hdr
        assert_eq!(&img[10..13], &[0x02, b'e', b'n']); // status + lang
        assert!(img[13..].starts_with(b"cashuAUt4st"));
        assert_eq!(img[6 + 0x1F], 0xFE); // terminator right after record
    }

    #[test]
    fn round_trips_through_the_parser() {
        let img = build_ndef_text_image("hello world", NDEF_AREA_512).unwrap();
        assert_eq!(parse_ndef_text(&img).as_deref(), Some("hello world"));
    }

    #[test]
    fn token_survives_build_parse_extract() {
        let img = build_ndef_text_image(TOKEN, NDEF_AREA_512).unwrap();
        let text = parse_ndef_text(&img).unwrap();
        assert_eq!(extract_cashu_token(&text), Some(TOKEN));
    }

    #[test]
    fn extraction_covers_all_device_strategies() {
        assert_eq!(extract_cashu_token(TOKEN), Some(TOKEN));
        assert_eq!(
            extract_cashu_token("https://x.example/#token=cashuBAbCdEf"),
            Some("cashuBAbCdEf")
        );
        assert_eq!(
            extract_cashu_token("https://x.example/?token=cashuAAq1"),
            Some("cashuAAq1")
        );
        assert_eq!(
            extract_cashu_token("junk cashuAZz9 trailing"),
            Some("cashuAZz9")
        );
        assert_eq!(extract_cashu_token("no tokens here"), None);
    }

    #[test]
    fn oversize_record_is_rejected() {
        let long = "x".repeat(600);
        assert!(matches!(
            build_ndef_text_image(&long, NDEF_AREA_512),
            Err(ImageError::RecordTooLong { .. })
        ));
    }

    #[test]
    fn three_byte_tlv_length_form() {
        let text = "y".repeat(300);
        let img = build_ndef_text_image(&text, 2040).unwrap();
        // 3-byte TLV length: 03 FF hi lo
        assert_eq!(&img[4..6], &[0x03, 0xFF]);
        let record_len = ((img[6] as usize) << 8) | img[7] as usize;
        // record = long-form header (7) + status/lang (3) + text
        assert_eq!(record_len, 300 + 7 + 3);
        // long-form record header: MB|ME without SR, 4-byte length
        assert_eq!(img[8], 0x51);
        let plen = u32::from_be_bytes([img[10], img[11], img[12], img[13]]) as usize;
        assert_eq!(plen, 300 + 3);
        assert_eq!(parse_ndef_text(&img).as_deref(), Some(text.as_str()));
    }

    #[test]
    fn rejects_non_ndef_images() {
        assert_eq!(parse_ndef_text(&[0x00; 16]), None);
    }
}
