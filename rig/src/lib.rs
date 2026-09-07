//! Host-side rig for the nucula atom port.
//!
//! Three layers, bottom-up:
//! - [`ndef_t2t`]: pure-logic NDEF Type 2 image building/parsing (unit
//!   tested everywhere, no hardware).
//! - [`acr`]: ACR1252U control over PC/SC escape commands (`live`
//!   feature) — card-emulation preload, NVM reader settings.
//! - e2e tests (`tests/`, `live` feature): drive the ACR and the atom
//!   console over serial through a full over-the-air token receive.

pub mod ndef_t2t;
pub mod locks;

#[cfg(feature = "live")]
pub mod acr;

#[cfg(feature = "live")]
pub mod atom_console;

#[cfg(feature = "live")]
pub mod rig;

/// Default atom serial for the lab rig (by-id path survives USB
/// re-enumeration; shared by the console driver and preflight).
/// Board targets supported by the rig. Each maps to a default serial
/// port; override with ATOM_PORT for any board.
#[derive(Debug, Clone, PartialEq)]
pub enum Board {
    Atom,
    M5Stick,
}

impl Board {
    pub fn default_port(&self) -> &'static str {
        match self {
            Board::Atom => "/dev/serial/by-id/usb-M5STACK_Inc._M5_Serial_Converter_9D529068B4-if00-port0",
            Board::M5Stick => "/dev/serial/by-id/usb-Hades2001_M5stack_49D6163EBE-if00-port0",
        }
    }

    pub fn name(&self) -> &'static str {
        match self {
            Board::Atom => "atom",
            Board::M5Stick => "m5stick",
        }
    }
}

/// Detect the board from NUCULA_BOARD env var, or by probing which
/// serial port exists.
pub fn detect_board() -> Board {
    if let Ok(b) = std::env::var("NUCULA_BOARD") {
        return match b.as_str() {
            "m5stick" => Board::M5Stick,
            _ => Board::Atom,
        };
    }
    // Probe: check which default port exists
    for board in [Board::Atom, Board::M5Stick] {
        if std::path::Path::new(board.default_port()).exists() {
            return board;
        }
    }
    Board::Atom
}

/// Default serial port: ATOM_PORT env var, else the detected board's.
pub fn atom_default_port() -> String {
    if let Ok(p) = std::env::var("ATOM_PORT") {
        return p;
    }
    detect_board().default_port().to_string()
}

#[cfg(feature = "payer")]
pub mod payer;

#[cfg(feature = "payer")]
pub mod strip;
