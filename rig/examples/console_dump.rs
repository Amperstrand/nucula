//! One-off: dump the tag the device's reader currently sees (console
//! command passthrough, long-capture variant for slow dumps).
//!
//!   cargo run --features live --example console_dump -- nfcdump 68

use std::time::Duration;

use nucula_rig::atom_console::AtomConsole;

fn main() {
    let cmd = std::env::args().nth(1).unwrap_or_else(|| "help".into());
    let port = nucula_rig::atom_default_port();
    let mut c = AtomConsole::open(&port).expect("console");
    c.port_mut()
        .write_all(format!("{cmd}\r\n").as_bytes())
        .expect("write");
    match c.read_until("nucula> ", Duration::from_secs(90)) {
        Ok(out) => println!("{out}"),
        Err(e) => eprintln!("{cmd} failed: {e}"),
    }
}
