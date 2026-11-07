//! The pack's start-up line in `garrysmod\lua\includes\menu.lua`.
//!
//! The same text and rules as `src/startup_hook.cpp` (the pack's own healing)
//! and the installer: one marked line appended with CRLF, matched by its
//! marker, written through a temporary file. A test reads
//! `src/startup_hook.cpp` and fails if the two copies of the text drift.
//!
//! Bytes, not strings: `menu.lua` is Garry's Mod's file and isn't promised
//! to be UTF-8.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

/// The marker every version's line carries.
pub const MARKER: &str = "meowy-client-pack";
/// The line itself (`startup_hook.cpp` `kLine`).
pub const LINE: &str = "pcall(require, \"meowy_boot\") -- meowy-client-pack v2 \
(loads the Meowy Client Pack at start-up; turn it off in Meowy settings, in the main menu)";

/// Where the line can be: the host it's written to first.
const HOSTS: [&str; 2] = ["lua\\includes\\menu.lua", "lua\\menu\\menu.lua"];
const MAX_FILE: u64 = 4 * 1024 * 1024;

fn find(hay: &[u8], needle: &[u8]) -> bool {
    hay.windows(needle.len()).any(|w| w == needle)
}

pub fn has_line(text: &[u8]) -> bool {
    find(text, MARKER.as_bytes())
}

/// `text` with the line appended on its own line, or unchanged if present.
pub fn with_line(text: &[u8]) -> Vec<u8> {
    if has_line(text) {
        return text.to_vec();
    }
    let mut out = text.to_vec();
    if !out.is_empty() && *out.last().unwrap() != b'\n' {
        out.extend_from_slice(b"\r\n");
    }
    out.extend_from_slice(LINE.as_bytes());
    out.extend_from_slice(b"\r\n");
    out
}

/// `text` with every marked line removed, and the CRLF `with_line` put in
/// front of it (as `startup_hook::WithoutLine` and the installer).
pub fn without_line(text: &[u8]) -> Vec<u8> {
    let kept: Vec<&[u8]> = text.split(|&b| b == b'\n').filter(|l| !has_line(l)).collect();
    let mut out = kept.join(&b'\n');
    if out.ends_with(b"\r\n") {
        out.truncate(out.len() - 2);
    }
    out
}

/// What `heal` did.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Heal {
    /// The line was already there.
    Present,
    /// The player turned "Start with the game" off; nothing written.
    Off,
    /// It was missing and is back.
    Added,
    Failed(String),
}

impl Heal {
    pub fn describe(&self) -> String {
        match self {
            Heal::Present => "present".into(),
            Heal::Off => "off".into(),
            Heal::Added => "added".into(),
            Heal::Failed(why) => format!("failed: {why}"),
        }
    }
}

fn read_small(file: &Path) -> io::Result<Vec<u8>> {
    let len = fs::metadata(file)?.len();
    if len > MAX_FILE {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "too big"));
    }
    fs::read(file)
}

/// Writes next to `file`, then swaps it in, so a crash never leaves half a menu.lua.
fn write_replacing(file: &Path, text: &[u8]) -> io::Result<()> {
    if let Ok(meta) = fs::metadata(file) {
        let mut perms = meta.permissions();
        if perms.readonly() {
            #[allow(clippy::permissions_set_readonly_false)]
            perms.set_readonly(false);
            fs::set_permissions(file, perms)?;
        }
    }
    let mut temp = file.as_os_str().to_owned();
    temp.push(".meowy-tmp");
    let temp = PathBuf::from(temp);
    let result = fs::write(&temp, text).and_then(|_| fs::rename(&temp, file));
    if result.is_err() {
        let _ = fs::remove_file(&temp);
    }
    result
}

fn off_file(data_dir: &Path) -> PathBuf {
    data_dir.join("startup_off.txt")
}

/// The line is in `includes\menu.lua` or `menu\menu.lua`.
pub fn present(garrysmod: &Path) -> bool {
    HOSTS.iter().any(|h| read_small(&garrysmod.join(h)).map(|t| has_line(&t)).unwrap_or(false))
}

/// The player turned start-up off (`startup_off.txt` in the pack's data folder).
pub fn off(data_dir: &Path) -> bool {
    off_file(data_dir).exists()
}

/// Adds the line back if it's missing and start-up isn't off.
pub fn heal(garrysmod: &Path, data_dir: &Path) -> Heal {
    if present(garrysmod) {
        return Heal::Present;
    }
    if off(data_dir) {
        return Heal::Off;
    }
    let file = garrysmod.join(HOSTS[0]);
    let text = match read_small(&file) {
        Ok(t) => t,
        Err(e) => return Heal::Failed(format!("lua\\includes\\menu.lua not readable ({e})")),
    };
    match write_replacing(&file, &with_line(&text)) {
        Ok(()) => Heal::Added,
        Err(e) => Heal::Failed(format!("couldn't write lua\\includes\\menu.lua ({e})")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const STOCK: &[u8] = b"include( \"menu/menu.lua\" )";

    fn cat(parts: &[&[u8]]) -> Vec<u8> {
        parts.concat()
    }

    #[test]
    fn rules_match_startup_hook_smoke() {
        let line = LINE.as_bytes();
        let with = with_line(STOCK);
        assert_eq!(with, cat(&[STOCK, b"\r\n", line, b"\r\n"]), "adds the line on its own line");
        assert_eq!(with_line(&with), with, "adds it once");
        assert!(has_line(&with) && !has_line(STOCK), "found");
        assert_eq!(without_line(&with), STOCK, "removed: back to stock");

        let crlf: &[u8] = b"a\r\nb\r\n";
        assert_eq!(with_line(crlf), cat(&[crlf, line, b"\r\n"]), "CRLF file: no blank line");

        let v1 = cat(&[STOCK, b"\r\npcall(require, \"meowy_boot\") -- meowy-client-pack v1 (old)\r\n"]);
        assert!(has_line(&v1), "v1 line: present");
        assert_eq!(with_line(&v1), v1, "v1 line: not added twice");
        assert_eq!(without_line(&v1), STOCK, "v1 line: removed");

        assert_eq!(with_line(b""), cat(&[line, b"\r\n"]), "empty file");
    }

    /// The pack (`src/startup_hook.cpp`) writes the same line: if one changes,
    /// the other must too, or each would see the other's as "present" but
    /// players would get two different texts.
    #[test]
    fn same_text_as_the_pack() {
        let cpp = include_str!("../../../src/startup_hook.cpp");
        let marker = cpp.split("kMarker = \"").nth(1).and_then(|s| s.split('"').next()).unwrap();
        assert_eq!(marker, MARKER);
        let start = cpp.find("kLine = ").expect("kLine in startup_hook.cpp");
        // Join the C++ string literals up to the ';' that ends the
        // declaration: "a" "b" -> ab, with \" unescaped.
        let mut text = String::new();
        let mut chars = cpp[start..].chars();
        let mut inside = false;
        while let Some(c) = chars.next() {
            match (inside, c) {
                (false, ';') => break,
                (false, '"') => inside = true,
                (true, '"') => inside = false,
                (true, '\\') => text.push(chars.next().unwrap()),
                (true, c) => text.push(c),
                _ => {}
            }
        }
        assert_eq!(text, LINE);
    }

    #[test]
    fn heal_on_a_folder() {
        let root = std::env::temp_dir().join(format!("meowy_launcher_line_{}", std::process::id()));
        let data = root.join("data");
        let includes = root.join("lua").join("includes");
        fs::create_dir_all(&includes).unwrap();
        fs::create_dir_all(&data).unwrap();
        let menu = includes.join("menu.lua");
        fs::write(&menu, STOCK).unwrap();

        assert_eq!(heal(&root, &data), Heal::Added);
        assert_eq!(heal(&root, &data), Heal::Present);
        assert!(present(&root));
        assert_eq!(fs::read(&menu).unwrap(), with_line(STOCK), "stock text kept");

        // Steam puts the stock file back; the player turned start-up off.
        fs::write(&menu, STOCK).unwrap();
        fs::write(data.join("startup_off.txt"), b"off").unwrap();
        assert_eq!(heal(&root, &data), Heal::Off);
        assert_eq!(fs::read(&menu).unwrap(), STOCK, "off: nothing written");
        fs::remove_file(data.join("startup_off.txt")).unwrap();

        // A read-only menu.lua (Steam sometimes leaves them so).
        let mut perms = fs::metadata(&menu).unwrap().permissions();
        perms.set_readonly(true);
        fs::set_permissions(&menu, perms).unwrap();
        assert_eq!(heal(&root, &data), Heal::Added);
        assert!(present(&root));
        assert!(!includes.join("menu.lua.meowy-tmp").exists(), "no temporary file left");

        // No menu.lua at all: a failure, never a new file.
        fs::remove_file(&menu).unwrap();
        assert!(matches!(heal(&root, &data), Heal::Failed(_)));
        assert!(!menu.exists());

        fs::remove_dir_all(&root).unwrap();
    }
}
