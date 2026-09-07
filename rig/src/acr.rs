//! ACR1252U control over PC/SC escape commands (`live` feature).
//!
//! Everything here was established empirically against firmware
//! ACR1252U_V101.0 on pcsclite 2.0.3; the quirks are load-bearing and
//! documented where they bite.

use core::fmt;

use pcsc::{Card, Context, Protocols, ShareMode};

/// pcsclite `SCARD_CTL_CODE(1)` — IOCTL_SMARTCARD_VENDOR_IFD_EXCHANGE.
/// Advertised by libccid as FEATURE_CCID_ESC_COMMAND (tag 0x13) only
/// when `ifdDriverOptions` bit 0 is set in ifd-ccid's Info.plist (we
/// set 0x0003). NOT the `SCARD_CTL_CODE(3500)` of older guides, and on
/// pcsclite 2.x the formula is `0x42000000 + code` with no shift.
const ESCAPE_IOCTL: u64 = 0x4200_0001;

/// MIFARE Ultralight card-emulation NFCMode byte.
const NFC_MODE_ULTRALIGHT: u8 = 0x01;

#[derive(Debug)]
pub enum AcrError {
    NoReader,
    /// Escape response did not start with the E1 status header.
    BadFraming(Vec<u8>),
    /// Card-emulation preload readback did not match the image.
    ReadbackMismatch(Vec<u8>),
    Pcsc(pcsc::Error),
}

impl fmt::Display for AcrError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            AcrError::NoReader => write!(f, "no ACR1252 PICC reader found"),
            AcrError::BadFraming(b) => write!(f, "bad escape framing: {b:02X?}"),
            AcrError::ReadbackMismatch(b) => {
                write!(f, "CE preload readback mismatch: {b:02X?}")
            }
            AcrError::Pcsc(e) => write!(f, "pcsc: {e}"),
        }
    }
}

impl std::error::Error for AcrError {}

impl From<pcsc::Error> for AcrError {
    fn from(e: pcsc::Error) -> Self {
        AcrError::Pcsc(e)
    }
}

pub struct Acr1252 {
    card: Card,
}

impl Acr1252 {
    /// Open the first ACR1252 contactless interface in DIRECT share
    /// mode: a reader-level channel, no card in the field required.
    pub fn open() -> Result<Self, AcrError> {
        let ctx = Context::establish(pcsc::Scope::User)?;
        let mut names = [0u8; 2048];
        let reader = ctx
            .list_readers(&mut names)?
            .find(|r| r.to_string_lossy().contains("PICC"))
            .ok_or(AcrError::NoReader)?;
        let card = ctx.connect(reader, ShareMode::Direct, Protocols::UNDEFINED)?;
        Ok(Self { card })
    }

    fn escape(&mut self, cmd: &[u8]) -> Result<Vec<u8>, AcrError> {
        let mut recv = [0u8; 522]; // dwMaxCCIDMsgLen
        let resp = self.card.control(ESCAPE_IOCTL, cmd, &mut recv)?;
        // Response framing: E1 00 00 00 <len> <payload...>
        if resp.len() < 5 || resp[0] != 0xE1 {
            return Err(AcrError::BadFraming(resp.to_vec()));
        }
        let len = resp[4] as usize;
        Ok(resp[5..5 + len].to_vec())
    }

    pub fn firmware(&mut self) -> Result<String, AcrError> {
        let p = self.escape(&[0xE0, 0x00, 0x00, 0x18, 0x00])?;
        Ok(String::from_utf8_lossy(&p).trim().to_string())
    }

    /// Which tag types the READER side detects. NVM-backed: survives
    /// replugs. Keep at 0xFF — card emulation does not radiate with
    /// all types muted (bit 0).
    pub fn set_picc_operating_parameter(&mut self, param: u8) -> Result<u8, AcrError> {
        let p = self.escape(&[0xE0, 0x00, 0x00, 0x20, 0x01, param])?;
        Ok(p.first().copied().unwrap_or(0))
    }

    pub fn read_picc_operating_parameter(&mut self) -> Result<u8, AcrError> {
        let p = self.escape(&[0xE0, 0x00, 0x00, 0x20, 0x00])?;
        Ok(p.first().copied().unwrap_or(0))
    }

    /// Auto PICC polling setting (0x8F ≈ factory default). NVM-backed.
    pub fn set_auto_polling(&mut self, setting: u8) -> Result<u8, AcrError> {
        let p = self.escape(&[0xE0, 0x00, 0x00, 0x23, 0x01, setting])?;
        Ok(p.first().copied().unwrap_or(0))
    }

    pub fn read_auto_polling(&mut self) -> Result<u8, AcrError> {
        let p = self.escape(&[0xE0, 0x00, 0x00, 0x23, 0x00])?;
        Ok(p.first().copied().unwrap_or(0))
    }

    pub fn exit_card_emulation(&mut self) -> Result<(), AcrError> {
        self.escape(&[0xE0, 0x00, 0x00, 0x40, 0x03, 0x00, 0x00, 0x00])?;
        Ok(())
    }

    /// Write into the emulated-card data area. NOTE: StartOffset is a
    /// single byte — addressability beyond offset 255 is unverified,
    /// keep images and chunks below it.
    pub fn write_ce_data(&mut self, start: u8, data: &[u8]) -> Result<(), AcrError> {
        let mut cmd = Vec::with_capacity(data.len() + 9);
        cmd.extend_from_slice(&[
            0xE0, 0x00, 0x00, 0x60, (data.len() + 4) as u8, 0x01,
            NFC_MODE_ULTRALIGHT, start, data.len() as u8,
        ]);
        cmd.extend_from_slice(data);
        self.escape(&cmd)?;
        Ok(())
    }

    pub fn read_ce_data(&mut self, start: u8, len: u8) -> Result<Vec<u8>, AcrError> {
        let p = self.escape(&[
            0xE0, 0x00, 0x00, 0x60, 0x04, 0x00,
            NFC_MODE_ULTRALIGHT, start, len,
        ])?;
        Ok(p)
    }

    /// ONE-WAY PER POWER CYCLE: after entering card emulation the CCID
    /// stack stops answering every escape (timeouts) until the reader
    /// is physically unplugged. USB port reset, set_configuration,
    /// the SAM interface, and sysfs authorized toggling all fail to
    /// revive it — see rig/README.md. Call only when the rig is ready
    /// to run the whole CE scenario.
    pub fn enter_ultralight_emulation(&mut self) -> Result<(), AcrError> {
        self.escape(&[
            0xE0, 0x00, 0x00, 0x40, 0x03, NFC_MODE_ULTRALIGHT, 0x00, 0x00,
        ])?;
        Ok(())
    }

    /// Preload a full NDEF image (see [`crate::ndef_t2t`]), verify by
    /// readback, then enter emulation. Tag types stay enabled (muted
    /// types mute the emulation too), but auto-polling is QUIETED
    /// first: a mode-switch escape racing an in-flight poll cycle is
    /// what wedges the firmware's CCID loop (see README), and NVM
    /// settings survive replugs.
    pub fn present_ndef_image(&mut self, image: &[u8]) -> Result<(), AcrError> {
        self.set_picc_operating_parameter(0xFF)?;
        self.set_auto_polling(0x00)?; // quiet BEFORE any mode switch
        // ...and let the quiet beat the next poll cycle before a mode
        // switch goes out: an exit raced an in-flight poll exactly once
        // (the rig preflight had just restored polling 0x8F) and wedged
        // the CCID loop until a physical replug.
        std::thread::sleep(std::time::Duration::from_millis(1000));
        self.exit_card_emulation()?;
        for (off, chunk) in image.chunks(48).enumerate() {
            self.write_ce_data((off * 48) as u8, chunk)?;
        }
        let head = self.read_ce_data(0, 48)?;
        if head.len() < image.len().min(16) || head[..image.len().min(16)] != image[..image.len().min(16)] {
            return Err(AcrError::ReadbackMismatch(head));
        }
        self.enter_ultralight_emulation()
    }
}

// ---------------------------------------------------------------------------
// Reader-mode card access: write an NDEF message onto a Type 4 tag in
// the field (the relay carrier — a blank NTAG424 boltcard). Uses the
// same PICC interface in PC/SC card mode (T=1 over T=CL).
// ---------------------------------------------------------------------------

use pcsc::Disposition;

#[derive(Debug)]
pub enum CardError {
    NoCard(String),
    Apdu { cmd: &'static str, sw: u16 },
    Pcsc(pcsc::Error),
    BadCc,
    /// The token's NDEF file exceeds the card's declared capacity —
    /// not fixable in software; use a bigger-NDEF carrier.
    Capacity { need: usize, have: usize },
}

impl fmt::Display for CardError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            CardError::NoCard(w) => write!(f, "no type 4 tag in the field: {w}"),
            CardError::Apdu { cmd, sw } => write!(f, "{cmd}: SW {sw:04X}"),
            CardError::Pcsc(e) => write!(f, "pcsc: {e}"),
            CardError::BadCc => write!(f, "implausible capability container"),
            CardError::Capacity { need, have } => write!(
                f,
                "NDEF file needs {need} bytes, card capacity {have} — use a \
                 bigger-NDEF carrier (NTAG 424 DNA class)"
            ),
        }
    }
}

impl std::error::Error for CardError {}

impl From<pcsc::Error> for CardError {
    fn from(e: pcsc::Error) -> Self {
        CardError::Pcsc(e)
    }
}

pub struct Acr1252Card {
    card: pcsc::Card,
}

impl Acr1252Card {
    /// Connect to the (single) tag the reader is polling. Auto-polling
    /// must be on — `Acr1252::set_auto_polling(0x8F)` first.
    pub fn connect() -> Result<Self, CardError> {
        let ctx = Context::establish(pcsc::Scope::User)?;
        let mut names = [0u8; 2048];
        let reader = ctx
            .list_readers(&mut names)?
            .find(|r| r.to_string_lossy().contains("PICC"))
            .ok_or(CardError::NoCard("PICC interface missing".into()))?;
        let card = ctx
            .connect(reader, ShareMode::Shared, Protocols::T1)
            .map_err(|e| CardError::NoCard(e.to_string()))?;
        Ok(Self { card })
    }

    /// Transmit one APDU, following GetResponse chains; returns the
    /// response body with the trailing SW stripped (Err on != 9000).
    fn apdu(&mut self, cmd: &'static str, send: &[u8]) -> Result<Vec<u8>, CardError> {
        let mut buf = [0u8; 512];
        let mut send = send.to_vec();
        let mut out = Vec::new();
        loop {
            let resp = self.card.transmit(&send, &mut buf)?;
            if resp.len() < 2 {
                return Err(CardError::Apdu { cmd, sw: 0 });
            }
            let sw = ((resp[resp.len() - 2] as u16) << 8) | resp[resp.len() - 2 + 1] as u16;
            let body = &resp[..resp.len() - 2];
            match sw {
                0x9000 => {
                    out.extend_from_slice(body);
                    return Ok(out);
                }
                0x6100..=0x61FF => {
                    out.extend_from_slice(body);
                    send = vec![0x00, 0xC0, 0x00, 0x00, (sw & 0xFF) as u8];
                }
                _ => return Err(CardError::Apdu { cmd, sw }),
            }
        }
    }

    /// Write an NDEF message (record bytes, see
    /// [`crate::ndef_t2t::build_ndef_text_record`]) into the tag's
    /// NDEF file: select application/CC/file, update NLEN + payload,
    /// verify by readback.
    pub fn write_ndef(&mut self, ndef_message: &[u8]) -> Result<(), CardError> {
        const SEL_APP: &[u8] = &[
            0x00, 0xA4, 0x04, 0x00, 0x07,
            0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01, 0x00,
        ];
        const SEL_CC: &[u8] = &[0x00, 0xA4, 0x00, 0x0C, 0x02, 0xE1, 0x03];
        const READ_CC: &[u8] = &[0x00, 0xB0, 0x00, 0x00, 0x0F];
        self.apdu("select NDEF app", SEL_APP)?;
        self.apdu("select CC", SEL_CC)?;
        let cc = self.apdu("read CC", READ_CC)?;
        if cc.len() < 11 {
            return Err(CardError::BadCc);
        }
        // CC layouts vary by mapping version (v2.0 moves the FID to
        // bytes 9-10); the NFC Forum well-known E104 works on both —
        // try it, fall back to the CC bytes 7-8.
        let mlc = ((cc[5] as u16) << 8) | cc[6] as u16;
        if let Some(cap) = cc_ndef_capacity(&cc) {
            let need = ndef_message.len() + 2; // NLEN + message
            if need > cap as usize {
                return Err(CardError::Capacity { need, have: cap as usize });
            }
        }
        let fid: u16 = 0xE104;
        let sel_file = [0x00, 0xA4, 0x00, 0x0C, 0x02, (fid >> 8) as u8, fid as u8];
        if self.apdu("select NDEF file", &sel_file).is_err() {
            let cc_fid = ((cc[7] as u16) << 8) | cc[8] as u16;
            let fallback = [0x00, 0xA4, 0x00, 0x0C, 0x02, (cc_fid >> 8) as u8, cc_fid as u8];
            self.apdu("select NDEF file", &fallback)?;
        }

        // NLEN (big-endian) then the message, chunked by MLc.
        let mut file = Vec::with_capacity(ndef_message.len() + 2);
        file.extend_from_slice(&(ndef_message.len() as u16).to_be_bytes());
        file.extend_from_slice(ndef_message);
        let chunk = mlc.min(0xF0) as usize;
        let mut off = 0usize;
        while off < file.len() {
            let end = (off + chunk).min(file.len());
            let mut upd = vec![0x00, 0xD6, (off >> 8) as u8, off as u8, (end - off) as u8];
            upd.extend_from_slice(&file[off..end]);
            self.apdu("update NDEF file", &upd)?;
            off = end;
        }

        // Readback: NLEN must round-trip.
        let rd = [0x00, 0xB0, 0x00, 0x00, 0x02];
        let nlen = self.apdu("verify NLEN", &rd)?;
        if nlen != (ndef_message.len() as u16).to_be_bytes() {
            return Err(CardError::Apdu { cmd: "verify NLEN", sw: 0xDEAD });
        }
        Ok(())
    }

    pub fn disconnect(self) {
        let _ = self.card.disconnect(Disposition::LeaveCard);
    }
}

/// NDEF file capacity from a capability container, when determinable:
/// v2.x File Control TLV (T=04) size field, or the v1 fixed layout.
fn cc_ndef_capacity(cc: &[u8]) -> Option<u16> {
    if cc.len() < 15 {
        return None;
    }
    // v2.x: walk TLVs after the fixed header (CCLEN 2, ver 1, MLe 2, MLc 2).
    if cc[2] >= 0x20 {
        // v2.x: TLV area begins right after the fixed header (CCLEN 2,
        // ver 1, MLe 2, MLc 2 = offset 7). T=04 is the NDEF File
        // Control TLV: T(1) L(1) FID(2) size(2) R(1) W(1).
        let cclen = (cc[0] as usize) * 256 + cc[1] as usize;
        let mut off = 7;
        while off + 2 <= cc.len().min(cclen) {
            let (t, l) = (cc[off], cc[off + 1] as usize);
            if t == 0x04 && l >= 4 && off + 2 + 6 <= cc.len().min(cclen) {
                let sz = ((cc[off + 4] as u16) << 8) | cc[off + 5] as u16;
                if sz > 0 {
                    return Some(sz);
                }
            }
            if l == 0 {
                break;
            }
            off += 2 + l;
        }
        return None;
    }
    // v1: FID at 7-8, size at 9-10.
    if cc[7] == 0xE1 && cc[8] == 0x04 {
        return Some(((cc[9] as u16) << 8) | cc[10] as u16);
    }
    None
}
