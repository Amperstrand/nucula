//! Machine-wide per-device advisory locks so concurrent projects and
//! sessions (this rig, bolty HIL, ad-hoc flashing) cannot stomp the
//! same hardware.
//!
//! Convention: `/tmp/amperstrand-rig/<device>.lock` — any Amperstrand
//! project can adopt the same path and coordinate without a central
//! service. This is the pragmatic layer under a future labgrid
//! exporter/place setup (labgrid place acquisition then replaces the
//! flock as the arbitration point).

use core::fmt;
use std::fs::{self, File, OpenOptions};
use std::path::PathBuf;

use fs2::FileExt;

pub const LOCK_DIR: &str = "/tmp/amperstrand-rig";

/// Lock acquisition failed: another process holds the device.
#[derive(Debug)]
pub struct LockHeld {
    pub name: String,
    /// PID line the holder wrote into the lock file (best effort).
    pub holder: String,
}

impl fmt::Display for LockHeld {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "device '{}' is locked by another session ({})",
            self.name,
            if self.holder.is_empty() { "unknown holder".to_string() } else { self.helder() }
        )
    }
}

impl LockHeld {
    fn helder(&self) -> String {
        self.holder.clone()
    }
}

/// Held device lock; released on drop.
pub struct DeviceLock {
    file: File,
    _name: String,
}

impl DeviceLock {
    /// Non-blocking exclusive lock on `<LOCK_DIR>/<name>.lock`.
    pub fn acquire(name: &str) -> Result<Self, LockHeld> {
        let dir = PathBuf::from(LOCK_DIR);
        fs::create_dir_all(&dir).ok();
        let path = dir.join(format!("{name}.lock"));
        let file = match OpenOptions::new()
            .create(true)
            .truncate(false)
            .write(true)
            .open(&path)
        {
            Ok(f) => f,
            Err(e) => {
                return Err(LockHeld {
                    name: name.to_string(),
                    holder: format!("open failed: {e}"),
                })
            }
        };
        if let Err(_) = file.try_lock_exclusive() {
            let holder = fs::read_to_string(&path)
                .unwrap_or_default()
                .trim()
                .to_string();
            return Err(LockHeld { name: name.to_string(), holder });
        }
        // Best-effort holder info for conflict diagnostics.
        let _ = fs::write(&path, format!("pid={}\n", std::process::id()));
        Ok(DeviceLock { file, _name: name.to_string() })
    }
}

impl Drop for DeviceLock {
    fn drop(&mut self) {
        let _ = self.file.unlock();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn exclusive_and_released_on_drop() {
        let a = DeviceLock::acquire("unit-test-device").expect("first acquire");
        let conflict = DeviceLock::acquire("unit-test-device");
        assert!(conflict.is_err(), "second acquire must fail");
        let err = conflict.err().unwrap();
        assert!(err.holder.contains("pid="), "holder info recorded: {err}");
        drop(a);
        // Re-acquire after release must succeed.
        let b = DeviceLock::acquire("unit-test-device").expect("re-acquire after drop");
        drop(b);
    }

    #[test]
    fn independent_devices_do_not_conflict() {
        let _a = DeviceLock::acquire("unit-test-a").expect("a");
        let _b = DeviceLock::acquire("unit-test-b").expect("b independent");
    }
}
