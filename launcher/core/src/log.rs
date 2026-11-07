//! `launcher\launcher.log`: one line per thing the launcher did, for support.
//! Kept under 256 KB (the older half is dropped). No player data beyond
//! server addresses the player joined.

use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};

static FILE: Mutex<Option<PathBuf>> = Mutex::new(None);
const MAX: u64 = 256 * 1024;

pub fn init(dir: &Path) {
    let _ = std::fs::create_dir_all(dir);
    *FILE.lock().unwrap() = Some(dir.join("launcher.log"));
}

pub fn line(msg: &str) {
    let guard = FILE.lock().unwrap();
    let Some(path) = guard.as_ref() else { return };
    if std::fs::metadata(path).map(|m| m.len() > MAX).unwrap_or(false) {
        if let Ok(old) = std::fs::read(path) {
            let half = &old[old.len() / 2..];
            let start = half.iter().position(|&b| b == b'\n').map(|p| p + 1).unwrap_or(0);
            let _ = std::fs::write(path, &half[start..]);
        }
    }
    let t = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(path) {
        let _ = writeln!(f, "{t} {msg}");
    }
}
