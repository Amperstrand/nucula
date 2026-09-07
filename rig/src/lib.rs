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
pub fn atom_default_port() -> String {
    std::env::var("ATOM_PORT").unwrap_or_else(|_| {
        "/dev/serial/by-id/usb-M5STACK_Inc._M5_Serial_Converter_9D529068B4-if00-port0".into()
    })
}

#[cfg(feature = "payer")]
pub mod payer;
