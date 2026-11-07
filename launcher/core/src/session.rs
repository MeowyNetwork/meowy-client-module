//! Did the game just crash on a server?
//!
//! The pack writes `%LOCALAPPDATA%\MeowyClientPack\on_server.txt`
//! (`<address> <unix time>\r\n<name>\r\n`, `src/resilience.cpp`) while the game
//! is connected to our server or a public one, and leaves it when the game
//! ends on a server. Measured 2026-11-07: a normal quit exits with 0, ending
//! the game in Task Manager exits with 0xFFFFFFFF and writes no crash report.

use std::path::Path;
use std::time::UNIX_EPOCH;

pub const SERVER_FILE: &str = "on_server.txt";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ServerFile {
    pub address: String,
    pub since: i64,
    pub name: String,
}

/// A server name to show: no control characters, at most 64 bytes, cut at a
/// character boundary (`resilience::CleanName`).
pub fn clean_name(name: &str) -> String {
    let mut out: String = name.chars().filter(|c| !c.is_control()).collect();
    if out.len() > 64 {
        let mut cut = 64;
        while !out.is_char_boundary(cut) {
            cut -= 1;
        }
        out.truncate(cut);
    }
    out
}

/// `on_server.txt`'s text, when the address in it is a public server's.
pub fn parse_server_file(text: &str) -> Option<ServerFile> {
    let mut lines = text.split('\n').map(|l| l.trim_end_matches('\r'));
    let (address, since) = lines.next()?.split_once(' ')?;
    let address = crate::address::public(address)?;
    let since = since.trim().parse::<i64>().ok().filter(|&t| t > 0)?;
    let name = clean_name(lines.next().unwrap_or(""));
    Some(ServerFile { address, since, name })
}

pub fn read_server_file(data_dir: &Path) -> Option<ServerFile> {
    let path = data_dir.join(SERVER_FILE);
    let meta = std::fs::metadata(&path).ok()?;
    if meta.len() > 512 {
        return None;
    }
    let bytes = std::fs::read(&path).ok()?;
    parse_server_file(&String::from_utf8_lossy(&bytes))
}

/// Shown (or answered): the pack mustn't ask again at the next start.
pub fn consume_server_file(data_dir: &Path) {
    let _ = std::fs::remove_file(data_dir.join(SERVER_FILE));
}

/// A crash report (`GarrysMod\crashes\*.txt` or `*.dmp`) written at or after `since`.
pub fn crash_report_since(game_root: &Path, since: i64) -> bool {
    let Ok(entries) = std::fs::read_dir(game_root.join("crashes")) else { return false };
    entries.flatten().any(|e| {
        let name = e.file_name().to_string_lossy().to_ascii_lowercase();
        (name.ends_with(".txt") || name.ends_with(".dmp"))
            && e.metadata()
                .and_then(|m| m.modified())
                .ok()
                .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
                .is_some_and(|d| d.as_secs() as i64 >= since)
    })
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Verdict {
    /// The game didn't end on a server (or the file is from an older session).
    NotOnServer,
    /// Ended on a server on purpose (exit code 0, no crash report).
    Quit,
    /// Crashed or was ended while on `address`.
    Crashed { address: String, name: String, report: bool },
}

/// `session_start`: the game process's creation time (unix seconds).
/// `report`: whether a crash report was written since a given time.
pub fn verdict(file: Option<ServerFile>, session_start: i64, exit_code: u32, report: impl Fn(i64) -> bool) -> Verdict {
    let Some(file) = file else { return Verdict::NotOnServer };
    // The pack writes the file once connected, so after the game started.
    // An older one is a session the launcher didn't see (it wasn't running).
    if file.since + 5 < session_start {
        return Verdict::NotOnServer;
    }
    let report = report(file.since);
    if report || exit_code != 0 {
        Verdict::Crashed { address: file.address, name: file.name, report }
    } else {
        Verdict::Quit
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_the_packs_file() {
        assert_eq!(
            parse_server_file("138.201.62.52:27015 1794079676\r\nMeowy Network | DarkRP\r\n"),
            Some(ServerFile { address: "138.201.62.52:27015".into(), since: 1794079676, name: "Meowy Network | DarkRP".into() })
        );
        // As measured: an empty name line.
        assert_eq!(
            parse_server_file("138.201.62.52:27015 1794079676\r\n\r\n").map(|f| f.name),
            Some(String::new())
        );
        assert_eq!(parse_server_file("1.2.3.4:27015 5").map(|f| f.since), Some(5), "no name line");
        for bad in ["", "192.168.1.1:27015 5\r\n", "1.2.3.4:27015\r\n", "1.2.3.4:27015 x\r\n", "1.2.3.4:27015 0\r\n", "1.2.3.4 5\r\n"] {
            assert_eq!(parse_server_file(bad), None, "{bad:?}");
        }
        let long = format!("1.2.3.4:1 9\r\n{}\u{7}é\r\n", "x".repeat(63));
        assert_eq!(parse_server_file(&long).unwrap().name, "x".repeat(63), "64 bytes, no split character");
    }

    #[test]
    fn verdicts() {
        let file = || Some(ServerFile { address: "1.2.3.4:27015".into(), since: 1000, name: "n".into() });
        let crashed = |report| Verdict::Crashed { address: "1.2.3.4:27015".into(), name: "n".into(), report };
        assert_eq!(verdict(None, 900, 0xFFFF_FFFF, |_| true), Verdict::NotOnServer);
        assert_eq!(verdict(file(), 900, 0, |_| false), Verdict::Quit, "quit on a server");
        assert_eq!(verdict(file(), 900, 0, |_| true), crashed(true), "crash report");
        assert_eq!(verdict(file(), 900, 0xFFFF_FFFF, |_| false), crashed(false), "ended in Task Manager");
        assert_eq!(verdict(file(), 900, 0xC000_0005, |t| t == 1000), crashed(true), "report checked from the file's time");
        assert_eq!(verdict(file(), 2000, 0xFFFF_FFFF, |_| true), Verdict::NotOnServer, "file from an older session");
    }

    #[test]
    fn reads_and_consumes() {
        let dir = std::env::temp_dir().join(format!("meowy_launcher_session_{}", std::process::id()));
        let crashes = dir.join("crashes");
        std::fs::create_dir_all(&crashes).unwrap();
        std::fs::write(dir.join(SERVER_FILE), "1.2.3.4:27015 1000\r\nname\r\n").unwrap();
        assert_eq!(read_server_file(&dir).unwrap().since, 1000);
        consume_server_file(&dir);
        assert_eq!(read_server_file(&dir), None);

        let now = std::time::SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs() as i64;
        assert!(!crash_report_since(&dir, now - 60));
        std::fs::write(crashes.join("a.log"), "x").unwrap();
        assert!(!crash_report_since(&dir, now - 60), "only .txt and .dmp");
        std::fs::write(crashes.join("x.TXT"), "x").unwrap();
        assert!(crash_report_since(&dir, now - 60));
        assert!(!crash_report_since(&dir, now + 3600));
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
