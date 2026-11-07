//! "Lazy fonts" without touching the game (start-up #9a, measured 2026-11-07:
//! −1.23 s median from game start to the pack's start-up, 5 pairs).
//!
//! At start-up Garry's Mod registers every font file in
//! `garrysmod\cache\workshop\resource\fonts` (extracted from Workshop
//! addons, 344 files / 117 MB on this PC) before the main menu, and again at
//! every map load. The ones at start-up are only needed once a server's Lua
//! runs, after a map load, which registers them again anyway. So for a launch
//! it starts, the launcher renames the folder aside and puts it back as soon
//! as the pack's start-up stub runs (after the start-up font pass), or after
//! a timeout, at game exit, or at its own next start.
//!
//! File operations only, on the game's cache: never a link (a junction there
//! is left alone), never a file anywhere else. If the game made a new
//! `fonts` folder in the meantime, the files come back into it, a name the
//! new folder already has keeping the new copy.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

pub const FONTS: &str = "cache\\workshop\\resource\\fonts";
pub const ASIDE: &str = "cache\\workshop\\resource\\fonts.meowy-lazy";

fn is_link(p: &Path) -> bool {
    fs::symlink_metadata(p).map(|m| m.file_type().is_symlink() || is_reparse(&m)).unwrap_or(false)
}

#[cfg(windows)]
fn is_reparse(m: &fs::Metadata) -> bool {
    use std::os::windows::fs::MetadataExt;
    m.file_attributes() & 0x400 != 0 // FILE_ATTRIBUTE_REPARSE_POINT
}
#[cfg(not(windows))]
fn is_reparse(_: &fs::Metadata) -> bool {
    false
}

pub fn fonts_dir(garrysmod: &Path) -> PathBuf {
    garrysmod.join(FONTS)
}
pub fn aside_dir(garrysmod: &Path) -> PathBuf {
    garrysmod.join(ASIDE)
}

/// Renames the fonts folder aside. Ok(true) if it did; Ok(false) when there's
/// nothing to do (no folder, an empty one, or a link). A leftover aside folder
/// is put back first.
pub fn set_aside(garrysmod: &Path) -> io::Result<bool> {
    restore(garrysmod)?;
    let fonts = fonts_dir(garrysmod);
    if !fonts.is_dir() || is_link(&fonts) || fs::read_dir(&fonts)?.next().is_none() {
        return Ok(false);
    }
    fs::rename(&fonts, aside_dir(garrysmod))?;
    Ok(true)
}

/// Puts the folder back. Ok(n): files that came back (0 when nothing was aside).
pub fn restore(garrysmod: &Path) -> io::Result<usize> {
    let aside = aside_dir(garrysmod);
    if !aside.is_dir() || is_link(&aside) {
        return Ok(0);
    }
    let fonts = fonts_dir(garrysmod);
    if !fonts.exists() {
        let n = fs::read_dir(&aside)?.count();
        fs::rename(&aside, &fonts)?;
        return Ok(n);
    }
    // The game made a new folder meanwhile: merge, the new copy winning.
    let mut n = 0;
    for entry in fs::read_dir(&aside)? {
        let entry = entry?;
        let target = fonts.join(entry.file_name());
        if entry.file_type()?.is_file() && !target.exists() {
            fs::rename(entry.path(), &target)?;
            n += 1;
        }
    }
    for entry in fs::read_dir(&aside)? {
        let entry = entry?;
        if entry.file_type()?.is_file() {
            fs::remove_file(entry.path())?; // the game's newer copy is in fonts\ already
        }
    }
    let _ = fs::remove_dir(&aside); // only if empty (a sub-folder is left as it is)
    Ok(n)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn setup(name: &str) -> PathBuf {
        let root = std::env::temp_dir().join(format!("meowy_launcher_fonts_{name}_{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(fonts_dir(&root)).unwrap();
        for f in ["a.ttf", "b.otf"] {
            fs::write(fonts_dir(&root).join(f), f).unwrap();
        }
        root
    }

    #[test]
    fn aside_and_back() {
        let root = setup("round");
        assert!(set_aside(&root).unwrap());
        assert!(!fonts_dir(&root).exists() && aside_dir(&root).is_dir());
        assert_eq!(restore(&root).unwrap(), 2);
        assert!(fonts_dir(&root).join("a.ttf").is_file() && !aside_dir(&root).exists());
        assert_eq!(restore(&root).unwrap(), 0, "nothing aside: nothing to do");
        fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn merges_when_the_game_made_a_new_folder() {
        let root = setup("merge");
        assert!(set_aside(&root).unwrap());
        fs::create_dir_all(fonts_dir(&root)).unwrap();
        fs::write(fonts_dir(&root).join("a.ttf"), "newer").unwrap();
        fs::write(fonts_dir(&root).join("c.ttf"), "c").unwrap();
        assert_eq!(restore(&root).unwrap(), 1, "b came back; a was there");
        assert_eq!(fs::read_to_string(fonts_dir(&root).join("a.ttf")).unwrap(), "newer", "the game's copy wins");
        assert!(fonts_dir(&root).join("b.otf").is_file() && fonts_dir(&root).join("c.ttf").is_file());
        assert!(!aside_dir(&root).exists());
        fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn leftovers_and_nothing_to_do() {
        let root = setup("left");
        // A launch that died with the folder aside: the next set_aside puts it back first.
        fs::rename(fonts_dir(&root), aside_dir(&root)).unwrap();
        assert!(set_aside(&root).unwrap());
        assert!(aside_dir(&root).join("a.ttf").is_file() && !fonts_dir(&root).exists());
        restore(&root).unwrap();
        // Empty or missing folder: nothing moved.
        for f in ["a.ttf", "b.otf"] {
            fs::remove_file(fonts_dir(&root).join(f)).unwrap();
        }
        assert!(!set_aside(&root).unwrap());
        fs::remove_dir(fonts_dir(&root)).unwrap();
        assert!(!set_aside(&root).unwrap());
        fs::remove_dir_all(&root).unwrap();
    }

    #[cfg(windows)]
    #[test]
    fn leaves_a_junction_alone() {
        let root = setup("link");
        let real = root.join("elsewhere");
        fs::rename(fonts_dir(&root), &real).unwrap();
        let ok = std::process::Command::new("cmd")
            .args(["/c", "mklink", "/J"])
            .arg(fonts_dir(&root))
            .arg(&real)
            .output()
            .map(|o| o.status.success())
            .unwrap_or(false);
        if ok {
            assert!(!set_aside(&root).unwrap(), "a junction isn't moved");
            assert!(fonts_dir(&root).exists() && !aside_dir(&root).exists());
            fs::remove_dir(fonts_dir(&root)).unwrap(); // removes the junction only
        }
        assert!(real.join("a.ttf").is_file(), "what it pointed to is untouched");
        fs::remove_dir_all(&root).unwrap();
    }
}
