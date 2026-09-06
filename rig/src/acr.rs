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
