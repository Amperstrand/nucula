//! Serial console driver for the atom running the nucula port
//! (`live` feature).

use core::fmt;
use std::io::{Read, Write};
use std::time::{Duration, Instant};

use serialport::SerialPort;

const PROMPT: &str = "nucula> ";

#[derive(Debug)]
pub enum ConsoleError {
    Open(String),
    Io(std::io::Error),
    Timeout { waiting_for: String, got: String },
}

impl fmt::Display for ConsoleError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ConsoleError::Open(p) => write!(f, "cannot open serial port {p}"),
            ConsoleError::Io(e) => write!(f, "serial io: {e}"),
            ConsoleError::Timeout { waiting_for, got } => write!(
                f,
                "timed out waiting for {waiting_for:?}; last output: {got}"
            ),
        }
    }
}

impl std::error::Error for ConsoleError {}

impl From<std::io::Error> for ConsoleError {
    fn from(e: std::io::Error) -> Self {
        ConsoleError::Io(e)
    }
}

pub struct AtomConsole {
    port: Box<dyn SerialPort>,
}

impl AtomConsole {
    pub fn open(path: &str) -> Result<Self, ConsoleError> {
        let port = serialport::new(path, 115_200)
            .timeout(Duration::from_millis(100))
            .open()
            .map_err(|e| ConsoleError::Open(format!("{path}: {e}")))?;
        let mut con = Self { port };
        // Wake the prompt; discard whatever was buffered.
        let _ = con.cmd("");
        Ok(con)
    }

    /// Send a console line and read until the prompt re-appears.
    pub fn cmd(&mut self, line: &str) -> Result<String, ConsoleError> {
        let out = self.write_line(line)?;
        self.read_until(PROMPT, Duration::from_secs(10))?;
        Ok(out)
    }

    fn write_line(&mut self, line: &str) -> Result<String, ConsoleError> {
        self.port
            .write_all(format!("{line}\r\n").as_bytes())?;
        self.read_until(PROMPT, Duration::from_secs(10))
    }

    /// Read until `pattern` shows up (substrings, prompt included in
    /// the returned text) or the deadline passes.
    pub fn read_until(
        &mut self,
        pattern: &str,
        deadline: Duration,
    ) -> Result<String, ConsoleError> {
        let end = Instant::now() + deadline;
        let mut out = String::new();
        let mut buf = [0u8; 512];
        loop {
            match self.port.read(&mut buf) {
                Ok(0) => {}
                Ok(n) => {
                    out.push_str(&String::from_utf8_lossy(&buf[..n]));
                    if out.contains(pattern) {
                        return Ok(out);
                    }
                }
                Err(e) if e.kind() == std::io::ErrorKind::TimedOut => {}
                Err(e) => return Err(e.into()),
            }
            if Instant::now() >= end {
                return Err(ConsoleError::Timeout {
                    waiting_for: pattern.to_string(),
                    got: out,
                });
            }
        }
    }

    pub fn status(&mut self) -> Result<String, ConsoleError> {
        self.cmd("status")
    }

    pub fn mint_add(&mut self, url: &str) -> Result<String, ConsoleError> {
        self.cmd(&format!("mint add {url}"))
    }

    pub fn nfc_request(&mut self, amount: u64) -> Result<String, ConsoleError> {
        self.cmd(&format!("nfc request {amount}"))
    }

    pub fn nfc_stop(&mut self) -> Result<String, ConsoleError> {
        self.cmd("nfc stop")
    }

    /// Wait for a log line containing `pattern` (log output arrives
    /// outside the prompt cadence; use for nfc task events).
    pub fn wait_for_log(&mut self, pattern: &str, timeout: Duration) -> Result<String, ConsoleError> {
        self.read_until(pattern, timeout)
    }
}
