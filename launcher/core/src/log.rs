//! `launcher\launcher.log`: one line per thing the launcher did, for support.
//! Kept under 256 KB (the older half is dropped). No player data beyond
//! server addresses the player joined.

use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, MutexGuard, TryLockError};
use std::time::{SystemTime, UNIX_EPOCH};

static FILE: Mutex<Option<PathBuf>> = Mutex::new(None);
const MAX: u64 = 256 * 1024;

/// The log's lock, also after a thread panicked while holding it: the path in
/// it is still good.
fn file() -> MutexGuard<'static, Option<PathBuf>> {
    FILE.lock().unwrap_or_else(|p| p.into_inner())
}

pub fn init(dir: &Path) {
    let _ = std::fs::create_dir_all(dir);
    *file() = Some(dir.join("launcher.log"));
}

pub fn line(msg: &str) {
    write(&file(), msg);
}

/// As `line`, but gives up instead of waiting when the log is busy: for the
/// panic hook, which may run on a thread that panicked inside `line`.
pub fn try_line(msg: &str) {
    match FILE.try_lock() {
        Ok(guard) => write(&guard, msg),
        Err(TryLockError::Poisoned(p)) => write(&p.into_inner(), msg),
        Err(TryLockError::WouldBlock) => {}
    }
}

fn write(path: &Option<PathBuf>, msg: &str) {
    let Some(path) = path.as_ref() else { return };
    if std::fs::metadata(path).map(|m| m.len() > MAX).unwrap_or(false) {
        if let Ok(old) = std::fs::read(path) {
            let half = &old[old.len() / 2..];
            let start = half.iter().position(|&b| b == b'\n').map(|p| p + 1).unwrap_or(0);
            // The kept half goes to a file of its own first, then takes the log's
            // name in one rename: a launcher stopped halfway leaves the old log whole.
            let mut tmp = path.as_os_str().to_owned();
            tmp.push(".tmp");
            if std::fs::write(&tmp, &half[start..]).and_then(|_| std::fs::rename(&tmp, path)).is_err() {
                let _ = std::fs::remove_file(&tmp);
            }
        }
    }
    let t = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(path) {
        let _ = writeln!(f, "{t} {msg}");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Rotation keeps the newer half, starting at a whole line, through the
    /// .tmp file (which doesn't stay behind).
    #[test]
    fn rotates_through_a_temp_file() {
        let dir = std::env::temp_dir().join(format!("meowy_launcher_log_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("launcher.log");
        let mut old = String::new();
        let mut i = 0;
        while old.len() as u64 <= MAX {
            old.push_str(&format!("1 line {i}\n"));
            i += 1;
        }
        std::fs::write(&path, &old).unwrap();
        write(&Some(path.clone()), "newest");
        let now = std::fs::read_to_string(&path).unwrap();
        assert!((now.len() as u64) < MAX / 2 + 100, "the older half dropped");
        assert!(now.starts_with("1 line "), "starts at a whole line");
        assert!(now.trim_end().ends_with(" newest"));
        assert!(!dir.join("launcher.log.tmp").exists());
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
