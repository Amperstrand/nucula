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

#[cfg(feature = "live")]
pub mod acr;

#[cfg(feature = "live")]
pub mod atom_console;

#[cfg(feature = "payer")]
pub mod payer;
