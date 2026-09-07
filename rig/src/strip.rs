//! DLEQ stripping for cashuB (V4 CBOR) tokens (`payer` feature).
//!
//! A cashuB token is base64url-encoded CBOR:
//!
//! ```text
//! { "m": <mint url>, "u": <unit>, "t": [ {
//!       "i": <keyset id>, "p": [ {
//!             "a": <amount>, "s": <secret>, "c": <C point>,
//!             "d": { "e": .., "s": .., "r": .. } } ] } ] }
//! ```
//!
//! The per-proof `"d"` DLEQ map is optional; a receiver that only
//! redeems proofs does not need it, and dropping it shrinks a 1-sat
//! token from ~374 to ~230 chars — under the 256-byte emulated-tag
//! image cap of an ACR1252U card-emulation write.
//!
//! [`strip_dleq`] copies the token CBOR verbatim, omitting `"d"`
//! entries from proof maps only. A `"d"` key anywhere else is foreign
//! to the format and is kept. The proof-map header is re-emitted with
//! the reduced entry count; every other byte is byte-identical.

use core::fmt;

use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use base64::Engine as _;

/// Maximum CBOR nesting the walker descends into (untrusted input).
const MAX_DEPTH: u8 = 32;

#[derive(Debug, PartialEq, Eq)]
pub enum StripError {
    /// Input does not start with the `cashuB` prefix.
    Prefix,
    /// Token body is not valid base64url.
    Base64,
    /// CBOR input ended where more bytes were required.
    Eof,
    /// Structurally invalid for a cashuB token.
    Malformed(&'static str),
    /// Well-formed CBOR this walker does not handle (indefinite
    /// lengths, tags, excessive nesting).
    Unsupported(&'static str),
    /// Bytes left over after the top-level CBOR item.
    Trailing,
}

impl fmt::Display for StripError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            StripError::Prefix => write!(f, "not a cashuB token (missing prefix)"),
            StripError::Base64 => write!(f, "token body is not valid base64url"),
            StripError::Eof => write!(f, "truncated CBOR"),
            StripError::Malformed(what) => write!(f, "malformed cashuB CBOR: {what}"),
            StripError::Unsupported(what) => write!(f, "unsupported CBOR: {what}"),
            StripError::Trailing => write!(f, "trailing bytes after top-level CBOR item"),
        }
    }
}

impl std::error::Error for StripError {}

/// Reads a CBOR head, returning `(major type, argument, rest)`. Only
/// definite lengths: additional-info 28..=30 (reserved) and 31
/// (indefinite) are rejected.
fn read_head(input: &[u8]) -> Result<(u8, u64, &[u8]), StripError> {
    let (&initial, mut rest) = input.split_first().ok_or(StripError::Eof)?;
    let arg = match initial & 0x1f {
        n @ 0..=23 => n as u64,
        24 => take_be(&mut rest, 1)?,
        25 => take_be(&mut rest, 2)?,
        26 => take_be(&mut rest, 4)?,
        27 => take_be(&mut rest, 8)?,
        _ => return Err(StripError::Unsupported("indefinite or reserved length")),
    };
    Ok((initial >> 5, arg, rest))
}

/// Consumes `n` bytes as a big-endian integer from the front of
/// `input`.
fn take_be(input: &mut &[u8], n: usize) -> Result<u64, StripError> {
    if input.len() < n {
        return Err(StripError::Eof);
    }
    let mut buf = [0u8; 8];
    buf[8 - n..].copy_from_slice(&input[..n]);
    *input = &input[n..];
    Ok(u64::from_be_bytes(buf))
}

/// Total encoded length of the first CBOR item in `input`.
fn item_len(input: &[u8], depth: u8) -> Result<usize, StripError> {
    if depth == 0 {
        return Err(StripError::Unsupported("nesting too deep"));
    }
    let (major, arg, rest) = read_head(input)?;
    let head = input.len() - rest.len();
    let arg = usize::try_from(arg).map_err(|_| StripError::Unsupported("length too large"))?;
    match major {
        0 | 1 | 7 => Ok(head),
        2 | 3 => {
            if arg > rest.len() {
                return Err(StripError::Eof);
            }
            Ok(head + arg)
        }
        4 => {
            let mut n = head;
            for _ in 0..arg {
                n += item_len(&input[n..], depth - 1)?;
            }
            Ok(n)
        }
        5 => {
            let mut n = head;
            for _ in 0..arg {
                n += item_len(&input[n..], depth - 1)?; // key
                n += item_len(&input[n..], depth - 1)?; // value
            }
            Ok(n)
        }
        _ => Err(StripError::Unsupported("tags")),
    }
}

/// Reads the head of a definite-length map/array of the expected major
/// type, rejecting entry counts that exceed the remaining bytes (every
/// child item is at least one byte).
fn read_container<'a>(
    input: &'a [u8],
    major: u8,
    what: &'static str,
) -> Result<(u64, &'a [u8]), StripError> {
    let (got, arg, rest) = read_head(input)?;
    if got != major {
        return Err(StripError::Malformed(what));
    }
    if arg > rest.len() as u64 {
        return Err(StripError::Eof);
    }
    Ok((arg, rest))
}

/// True if `key_item` encodes the text string `want`.
fn key_is(key_item: &[u8], want: &[u8]) -> bool {
    match read_head(key_item) {
        Ok((3, arg, payload)) => arg == want.len() as u64 && payload == want,
        _ => false,
    }
}

/// Appends the minimal head for a definite-length item of `major` with
/// `arg` as its argument.
fn push_head(out: &mut Vec<u8>, major: u8, arg: u64) {
    let hi = major << 5;
    match arg {
        0..=23 => out.push(hi | arg as u8),
        24..=0xff => out.extend_from_slice(&[hi | 24, arg as u8]),
        0x100..=0xffff => {
            out.push(hi | 25);
            out.extend_from_slice(&(arg as u16).to_be_bytes());
        }
        0x1_0000..=0xffff_ffff => {
            out.push(hi | 26);
            out.extend_from_slice(&(arg as u32).to_be_bytes());
        }
        _ => {
            out.push(hi | 27);
            out.extend_from_slice(&arg.to_be_bytes());
        }
    }
}

/// Copies one CBOR item verbatim; returns the unconsumed rest.
fn copy_verbatim<'a>(
    input: &'a [u8],
    out: &mut Vec<u8>,
    depth: u8,
) -> Result<&'a [u8], StripError> {
    let n = item_len(input, depth)?;
    out.extend_from_slice(&input[..n]);
    Ok(&input[n..])
}

/// Copies the value of a map entry whose `key_item` is already copied;
/// descends into the known cashuB paths (`"t"` token array, `"p"`
/// proof array), copies anything else verbatim.
fn copy_root_map<'a>(
    input: &'a [u8],
    out: &mut Vec<u8>,
    depth: u8,
) -> Result<&'a [u8], StripError> {
    let (count, mut cur) = read_container(input, 5, "top-level item is not a map")?;
    out.extend_from_slice(&input[..input.len() - cur.len()]);
    for _ in 0..count {
        let n = item_len(cur, depth - 1)?;
        let (key, rest) = cur.split_at(n);
        out.extend_from_slice(key);
        cur = if key_is(key, b"t") {
            copy_token_array(rest, out, depth - 1)?
        } else {
            copy_verbatim(rest, out, depth - 1)?
        };
    }
    Ok(cur)
}

fn copy_token_array<'a>(
    input: &'a [u8],
    out: &mut Vec<u8>,
    depth: u8,
) -> Result<&'a [u8], StripError> {
    let (count, mut cur) = read_container(input, 4, "\"t\" is not an array")?;
    out.extend_from_slice(&input[..input.len() - cur.len()]);
    for _ in 0..count {
        cur = copy_token_map(cur, out, depth - 1)?;
    }
    Ok(cur)
}

fn copy_token_map<'a>(
    input: &'a [u8],
    out: &mut Vec<u8>,
    depth: u8,
) -> Result<&'a [u8], StripError> {
    let (count, mut cur) = read_container(input, 5, "\"t\" entry is not a map")?;
    out.extend_from_slice(&input[..input.len() - cur.len()]);
    for _ in 0..count {
        let n = item_len(cur, depth - 1)?;
        let (key, rest) = cur.split_at(n);
        out.extend_from_slice(key);
        cur = if key_is(key, b"p") {
            copy_proof_array(rest, out, depth - 1)?
        } else {
            copy_verbatim(rest, out, depth - 1)?
        };
    }
    Ok(cur)
}

fn copy_proof_array<'a>(
    input: &'a [u8],
    out: &mut Vec<u8>,
    depth: u8,
) -> Result<&'a [u8], StripError> {
    let (count, mut cur) = read_container(input, 4, "\"p\" is not an array")?;
    out.extend_from_slice(&input[..input.len() - cur.len()]);
    for _ in 0..count {
        cur = copy_proof_map(cur, out, depth - 1)?;
    }
    Ok(cur)
}

/// Copies one proof map, omitting any `"d"` (DLEQ) entry. The map
/// header is re-emitted with the reduced entry count (minimal
/// encoding); the surviving entries are byte-identical.
fn copy_proof_map<'a>(
    input: &'a [u8],
    out: &mut Vec<u8>,
    depth: u8,
) -> Result<&'a [u8], StripError> {
    let (count, mut cur) = read_container(input, 5, "proof is not a map")?;
    let mut body = Vec::new();
    let mut kept = 0u64;
    for _ in 0..count {
        let key_n = item_len(cur, depth - 1)?;
        let val_n = item_len(&cur[key_n..], depth - 1)?;
        let n = key_n + val_n;
        if !key_is(&cur[..key_n], b"d") {
            kept += 1;
            body.extend_from_slice(&cur[..n]);
        }
        cur = &cur[n..];
    }
    push_head(out, 5, kept);
    out.extend_from_slice(&body);
    Ok(cur)
}

/// Strips the optional DLEQ proof (`"d"`) from every proof in a
/// cashuB (V4 CBOR) token and returns the shortened token.
///
/// Everything else — mint URL, unit, keyset IDs, proof amounts,
/// secrets, C points, entry order — is copied through byte-for-byte,
/// so the stripped token verifies identically at a DLEQ-optional
/// receiver.
pub fn strip_dleq(token: &str) -> Result<String, StripError> {
    let body = token.strip_prefix("cashuB").ok_or(StripError::Prefix)?;
    let raw = URL_SAFE_NO_PAD
        .decode(body.trim_end_matches('='))
        .map_err(|_| StripError::Base64)?;
    let mut out = Vec::with_capacity(raw.len());
    let rest = copy_root_map(&raw, &mut out, MAX_DEPTH)?;
    if !rest.is_empty() {
        return Err(StripError::Trailing);
    }
    Ok(format!("cashuB{}", URL_SAFE_NO_PAD.encode(&out)))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn head(major: u8, arg: u64) -> Vec<u8> {
        let mut out = Vec::new();
        push_head(&mut out, major, arg);
        out
    }

    fn text(s: &str) -> Vec<u8> {
        let mut out = head(3, s.len() as u64);
        out.extend_from_slice(s.as_bytes());
        out
    }

    fn bytes(b: &[u8]) -> Vec<u8> {
        let mut out = head(2, b.len() as u64);
        out.extend_from_slice(b);
        out
    }

    fn uint(n: u64) -> Vec<u8> {
        head(0, n)
    }

    fn b64(data: &[u8]) -> String {
        URL_SAFE_NO_PAD.encode(data)
    }

    /// One synthetic proof: `a`/`s`/`c` always, `d` when `with_dleq`.
    fn proof(with_dleq: bool) -> Vec<u8> {
        let secret = "11".repeat(32); // 64 hex chars
        let mut dleq = head(5, 3);
        for (k, v) in [("e", 0xaau8), ("s", 0xbb), ("r", 0xcc)] {
            dleq.extend_from_slice(&text(k));
            dleq.extend_from_slice(&bytes(&[v; 32]));
        }

        let mut p = head(5, if with_dleq { 4 } else { 3 });
        p.extend_from_slice(&text("a"));
        p.extend_from_slice(&uint(1));
        p.extend_from_slice(&text("s"));
        p.extend_from_slice(&text(&secret));
        p.extend_from_slice(&text("c"));
        p.extend_from_slice(&bytes(&[2u8; 33]));
        if with_dleq {
            p.extend_from_slice(&text("d"));
            p.extend_from_slice(&dleq);
        }
        p
    }

    /// The `"t"`-array token map carrying `proofs`.
    fn token_map(proofs: &[u8]) -> Vec<u8> {
        let mut t = head(5, 2);
        t.extend_from_slice(&text("i"));
        t.extend_from_slice(&bytes(&[0, 1, 2, 3, 4, 5, 6, 7]));
        t.extend_from_slice(&text("p"));
        t.extend_from_slice(&head(4, 1));
        t.extend_from_slice(proofs);
        t
    }

    /// A root map with `m`/`u`/`t` plus extra encoded entries.
    fn root(extra: &[(&str, &[u8])], tokens: &[u8]) -> Vec<u8> {
        let mut r = head(5, 3 + extra.len() as u64);
        r.extend_from_slice(&text("m"));
        r.extend_from_slice(&text("http://192.168.13.221:3338"));
        r.extend_from_slice(&text("u"));
        r.extend_from_slice(&text("sat"));
        r.extend_from_slice(&text("t"));
        r.extend_from_slice(&head(4, 1));
        r.extend_from_slice(tokens);
        for (k, v) in extra {
            r.extend_from_slice(&text(k));
            r.extend_from_slice(v);
        }
        r
    }

    fn token_string(cbor: &[u8]) -> String {
        format!("cashuB{}", b64(cbor))
    }

    #[test]
    fn rejects_non_cashub_tokens() {
        assert_eq!(strip_dleq(""), Err(StripError::Prefix));
        assert_eq!(strip_dleq("nonsense"), Err(StripError::Prefix));
        // cashuA is the JSON legacy format — nothing to strip here.
        let cashu_a = format!("cashuA{}", b64(b"{}"));
        assert_eq!(strip_dleq(&cashu_a), Err(StripError::Prefix));
    }

    #[test]
    fn rejects_invalid_base64() {
        assert_eq!(
            strip_dleq("cashuB!!!not-base64###"),
            Err(StripError::Base64)
        );
        assert_eq!(strip_dleq("cashuB%2F%2B"), Err(StripError::Base64));
    }

    #[test]
    fn rejects_malformed_cbor() {
        let empty = format!("cashuB{}", b64(&[]));
        assert_eq!(strip_dleq(&empty), Err(StripError::Eof));
        // map(1) with no entries
        let truncated = format!("cashuB{}", b64(&[0xa1]));
        assert_eq!(strip_dleq(&truncated), Err(StripError::Eof));
        // empty map plus a trailing uint
        let trailing = format!("cashuB{}", b64(&[0xa0, 0x00]));
        assert_eq!(strip_dleq(&trailing), Err(StripError::Trailing));
        // "t" entry is an array, not a map
        let mut bad = head(5, 3);
        bad.extend_from_slice(&text("m"));
        bad.extend_from_slice(&text("http://m"));
        bad.extend_from_slice(&text("u"));
        bad.extend_from_slice(&text("sat"));
        bad.extend_from_slice(&text("t"));
        bad.extend_from_slice(&head(4, 1));
        bad.extend_from_slice(&head(4, 1)); // array where a map belongs
        bad.extend_from_slice(&uint(1));
        let bad = token_string(&bad);
        assert!(matches!(strip_dleq(&bad), Err(StripError::Malformed(_))));
    }

    #[test]
    fn strips_dleq_and_keeps_everything_else() {
        let full = token_string(&root(&[], &token_map(&proof(true))));
        let stripped = strip_dleq(&full).expect("strips");
        assert!(
            stripped.len() < full.len(),
            "stripped {} not shorter than {}",
            stripped.len(),
            full.len()
        );
        // Byte-identical to the same token minted without DLEQ.
        assert_eq!(
            stripped,
            token_string(&root(&[], &token_map(&proof(false))))
        );
    }

    #[test]
    fn stripped_token_keeps_m_u_t_keys() {
        let stripped =
            strip_dleq(&token_string(&root(&[], &token_map(&proof(true))))).expect("strips");
        let raw = URL_SAFE_NO_PAD
            .decode(stripped.strip_prefix("cashuB").expect("prefix"))
            .expect("base64");
        let (major, count, mut cur) = read_head(&raw).expect("head");
        assert_eq!((major, count), (5, 3));
        let mut keys = Vec::new();
        for _ in 0..count {
            let n = item_len(cur, MAX_DEPTH).expect("key len");
            keys.push(cur[..n].to_vec());
            cur = &cur[n..];
            let n = item_len(cur, MAX_DEPTH).expect("value len");
            cur = &cur[n..];
        }
        assert_eq!(keys, [text("m"), text("u"), text("t")]);
    }

    #[test]
    fn token_without_dleq_round_trips_unchanged() {
        let plain = token_string(&root(&[], &token_map(&proof(false))));
        assert_eq!(strip_dleq(&plain).expect("strips"), plain);
    }

    #[test]
    fn foreign_d_keys_outside_proof_maps_are_kept() {
        // A "d" at the root level is foreign to the cashuB format; the
        // stripper only touches proof maps.
        let full = token_string(&root(&[("d", &bytes(b"keep"))], &token_map(&proof(false))));
        assert_eq!(strip_dleq(&full).expect("strips"), full);
    }
}
