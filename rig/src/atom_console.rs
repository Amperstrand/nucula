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
        let mut port = serialport::new(path, 115_200)
            .timeout(Duration::from_millis(100))
            .open()
            .map_err(|e| ConsoleError::Open(format!("{path}: {e}")))?;
        // CP210x auto-reset wiring (probe-verified in
        // tests/e2e.rs::console_line_probe): DTR low pulses EN, and the
        // open can land the control lines in a reset-holding state.
        // Drive the proven-alive combination and let it settle.
        port.write_data_terminal_ready(true)
            .map_err(|e| ConsoleError::Open(format!("{path}: dtr: {e}")))?;
        port.write_request_to_send(true)
            .map_err(|e| ConsoleError::Open(format!("{path}: rts: {e}")))?;
        std::thread::sleep(Duration::from_millis(300));
        let mut con = Self { port };
        // Wake the prompt; a line transition may have reset the board,
        // so retry once if the first sync lands mid-boot.
        if con.cmd("").is_err() {
            std::thread::sleep(Duration::from_millis(500));
            con.cmd("")?;
        }
        Ok(con)
    }

    /// Send a console line and read until the prompt re-appears.
    ///
    /// Exactly one prompt-sync: write_line already reads to the first
    /// prompt. (A bare CR LF yields two prompts — CR and LF each
    /// terminate a line — which masked a duplicate second read here:
    /// real commands emit exactly one prompt, and waiting for a second
    /// starved every non-empty cmd.)
    pub fn cmd(&mut self, line: &str) -> Result<String, ConsoleError> {
        self.write_line(line)
    }

    fn write_line(&mut self, line: &str) -> Result<String, ConsoleError> {
        self.port
            .write_all(format!("{line}\r\n").as_bytes())?;
        self.port.flush()?;
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
