//! The pack's signed updater (U1, `src/updater.cpp`), from the launcher's side:
//! the same release document, signed by the policy's release key only, the
//! same checks (`release::should_take`) and the same files, so while the game
//! is closed the launcher stages exactly what the pack would stage in the game.
//!
//!   %LOCALAPPDATA%\MeowyClientPack\bin\<version>\gmcl_meowy_<arch>.dll
//!   state\pending_<arch>.txt  written last, atomically ("version x\nsha256 y\nok 0\n")
//!   state\release_seq.txt     the seq floor, after a successful stage only
//!   state\install_token       made the same way if missing; never sent anywhere
//!
//! `current`, `previous` and `bad` belong to the start-up stub, which alone
//! promotes, verifies and rolls back: this never writes them.
//!
//! The running version, from outside the game: the stub's choice
//! (`pack_versions::Choose`: current, then previous, if hash and policy allow),
//! else the installed copy in `lua\bin`, by its own version resource (the
//! pack's VERSIONINFO, src/version.h). A copy without one (installed before the
//! pack had it) is "not known": nothing is staged, the pack updates itself in game.

use crate::release::{self, Facts, Pointer, Release};
use std::path::{Path, PathBuf};

pub const HOST: &str = "meowynetwork.com";
pub const DOC_PATH: &str = "/pack/v1/release-stable.txt";
pub const SIG_PATH: &str = "/pack/v1/release-stable.sig";

pub fn dll_name(arch: &str) -> String {
    format!("gmcl_meowy_{arch}.dll")
}

pub fn state_path(data: &Path, which: &str, arch: &str) -> PathBuf {
    data.join("state").join(format!("{which}_{arch}.txt"))
}

pub fn version_dll(data: &Path, version: &str, arch: &str) -> PathBuf {
    data.join("bin").join(version).join(dll_name(arch))
}

pub fn read_pointer(path: &Path) -> Option<Pointer> {
    let b = std::fs::read(path).ok().filter(|b| b.len() <= 512)?;
    Pointer::parse(&String::from_utf8_lossy(&b))
}

pub fn bad_list(data: &Path, arch: &str) -> Vec<String> {
    std::fs::read(state_path(data, "bad", arch))
        .ok()
        .filter(|b| b.len() <= 4096)
        .map(|b| String::from_utf8_lossy(&b).lines().map(|l| l.trim().to_string()).filter(|l| !l.is_empty()).collect())
        .unwrap_or_default()
}

pub fn last_seq(data: &Path) -> u64 {
    std::fs::read_to_string(data.join("state").join("release_seq.txt")).ok().and_then(|s| s.trim().parse().ok()).unwrap_or(0)
}

/// What the launcher shows: the pointers, the seq floor, the last check.
#[derive(Debug, Clone, Default, serde::Serialize)]
pub struct Status {
    pub arch: String,
    pub running: Option<String>,
    pub current: Option<String>,
    pub pending: Option<String>,
    pub previous: Option<String>,
    pub bad: Vec<String>,
    pub release_seq: u64,
    pub last_check: String,
}

/// The player turned updates off in the pack ("meowy_updates=0" in its
/// `menu_settings.txt`, read the engine's way: a number, 0 is off). Then nothing
/// is staged here, and the start-up stub starts no new version either
/// (`pack_versions::UpdatesOff`).
pub fn updates_off(data: &Path) -> bool {
    let Ok(bytes) = std::fs::read(data.join("menu_settings.txt")) else { return false };
    let text = String::from_utf8_lossy(&bytes[..bytes.len().min(4096)]).into_owned();
    for line in text.lines() {
        if let Some(value) = line.strip_prefix("meowy_updates=") {
            let value = value.trim_start();
            let digits = value.strip_prefix(['-', '+']).unwrap_or(value);
            return !digits.bytes().take_while(u8::is_ascii_digit).any(|b| b != b'0');
        }
    }
    false
}

#[cfg(windows)]
pub use staging::*;

#[cfg(windows)]
mod staging {
    use super::*;
    use crate::win;

    fn write_atomic(path: &Path, data: &[u8]) -> std::io::Result<()> {
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir)?;
        }
        let mut tmp = path.as_os_str().to_owned();
        tmp.push(".tmp");
        std::fs::write(&tmp, data)?;
        std::fs::rename(&tmp, path)
    }

    pub fn sha256_hex(data: &[u8]) -> String {
        win::sha256(data).iter().map(|b| format!("{b:02x}")).collect()
    }

    fn file_sha256(path: &Path) -> Option<String> {
        let b = std::fs::read(path).ok().filter(|b| !b.is_empty() && b.len() as u64 <= release::MAX_DLL)?;
        Some(sha256_hex(&b))
    }

    /// state\install_token: 32 hex; made with the system RNG if missing (as the pack).
    pub fn install_token(data: &Path) -> Option<String> {
        let path = data.join("state").join("install_token");
        if let Ok(t) = std::fs::read_to_string(&path) {
            let t = t.trim().to_string();
            if t.len() == 32 && t.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)) {
                return Some(t);
            }
        }
        let t: String = win::random_bytes(16)?.iter().map(|b| format!("{b:02x}")).collect();
        write_atomic(&path, format!("{t}\n").as_bytes()).ok()?;
        Some(t)
    }

    pub fn rollout_slot(token: &str, salt: &str) -> i32 {
        release::rollout_slot(&win::sha256(format!("{token}{salt}").as_bytes()))
    }

    /// The start-up stub can load updates: its DLL exports meowy_stub_version (=> 1).
    pub fn stub_version(garrysmod: &Path, arch: &str) -> i32 {
        let stub = garrysmod.join("lua\\bin").join(format!("gmsv_meowy_boot_{arch}.dll"));
        std::fs::read(stub).map(|b| crate::pe::exports(&b, "meowy_stub_version") as i32).unwrap_or(0)
    }

    /// The version the stub would load next (`pack_versions::Choose`), or the
    /// installed copy's own version resource; None when neither is known.
    pub fn running_version(data: &Path, garrysmod: &Path, arch: &str, policy: Option<&crate::policy::Policy>) -> Option<String> {
        let bad = bad_list(data, arch);
        for which in ["current", "previous"] {
            let Some(p) = read_pointer(&state_path(data, which, arch)) else { continue };
            if !bad.contains(&p.version)
                && release::policy_allows(policy, &p.version)
                && file_sha256(&version_dll(data, &p.version, arch)).as_deref() == Some(p.sha256.as_str())
            {
                return Some(p.version);
            }
        }
        installed_version(garrysmod, arch)
    }

    /// The `lua\bin` copy's version from its VERSIONINFO (src/version.h), if it has one.
    pub fn installed_version(garrysmod: &Path, arch: &str) -> Option<String> {
        win::file_version(&garrysmod.join("lua\\bin").join(dll_name(arch))).filter(|v| release::valid_version(v))
    }

    pub fn status(data: &Path, garrysmod: Option<&Path>, arch: &str, policy: Option<&crate::policy::Policy>, last_check: &str) -> Status {
        let v = |which| read_pointer(&state_path(data, which, arch)).map(|p| p.version);
        Status {
            arch: arch.into(),
            running: garrysmod.and_then(|g| running_version(data, g, arch, policy)),
            current: v("current"),
            pending: v("pending"),
            previous: v("previous"),
            bad: bad_list(data, arch),
            release_seq: last_seq(data),
            last_check: last_check.into(),
        }
    }

    fn remove_folder(dir: &Path) {
        let _ = std::fs::remove_dir_all(dir);
    }

    /// bin\<version> folders nothing points to (current, previous, pending kept), and partials.
    pub fn clean_up(data: &Path, arch: &str) {
        let keep: Vec<String> = ["current", "previous", "pending"]
            .iter()
            .filter_map(|w| read_pointer(&state_path(data, w, arch)).map(|p| p.version))
            .collect();
        // The other architecture's pointers too: one bin\ holds both.
        let other = if arch == "win64" { "win32" } else { "win64" };
        let keep_other: Vec<String> = ["current", "previous", "pending"]
            .iter()
            .filter_map(|w| read_pointer(&state_path(data, w, other)).map(|p| p.version))
            .collect();
        let Ok(entries) = std::fs::read_dir(data.join("bin")) else { return };
        for e in entries.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            if !e.path().is_dir() || name.starts_with('.') {
                continue;
            }
            if name.ends_with(".partial") || !(keep.contains(&name) || keep_other.contains(&name)) {
                remove_folder(&e.path());
            }
        }
    }

    /// Downloads (through `fetch`, which must give exactly `size` bytes or fail),
    /// checks and stages `r` for the next start. Ok(said) or Err(why).
    pub fn stage(
        data: &Path,
        r: &Release,
        arch: &str,
        fetch: impl Fn(&str, u64) -> Result<Vec<u8>, String>,
    ) -> Result<String, String> {
        let file = release::own_file(r, arch).ok_or_else(|| format!("{} has no file for this game build", r.version))?;
        let bytes = fetch(&file.path, file.size).map_err(|_| format!("couldn't download {}", r.version))?;
        if bytes.len() as u64 != file.size {
            return Err(format!("couldn't download {}", r.version));
        }
        let bin = data.join("bin");
        let partial = bin.join(format!("{}.partial", r.version));
        let final_dir = bin.join(&r.version);
        remove_folder(&partial);
        std::fs::create_dir_all(&partial).map_err(|e| e.to_string())?;
        let dll = partial.join(dll_name(arch));
        if write_atomic(&dll, &bytes).is_err() || file_sha256(&dll).as_deref() != Some(file.sha256.as_str()) {
            remove_folder(&partial);
            return Err(format!("the download of {} didn't match its signed hash", r.version));
        }
        remove_folder(&final_dir);
        if std::fs::rename(&partial, &final_dir).is_err() {
            remove_folder(&partial);
            return Err(format!("couldn't put {} in place", r.version));
        }
        let p = Pointer { version: r.version.clone(), sha256: file.sha256.clone(), ok: 0 };
        write_atomic(&state_path(data, "pending", arch), p.text().as_bytes()).map_err(|_| format!("couldn't record {}", r.version))?;
        let _ = write_atomic(&data.join("state").join("release_seq.txt"), format!("{}\n", r.seq).as_bytes());
        clean_up(data, arch);
        Ok(format!("{} is ready: it starts next time you open Garry's Mod", r.version))
    }

    /// One check, as `updater::CheckOnce`: Ok(Some(version)) when one was staged.
    /// `fetch(path, cap)` gets a body of at most `cap` bytes (Err otherwise).
    #[allow(clippy::too_many_arguments)]
    pub fn check_once(
        keys: &crate::policy::Keys,
        data: &Path,
        garrysmod: &Path,
        arch: &str,
        policy: Option<&crate::policy::Policy>,
        now: u64,
        game_running: &dyn Fn() -> bool,
        fetch: &dyn Fn(&str, u64) -> Result<Vec<u8>, String>,
    ) -> (String, Option<String>) {
        if updates_off(data) {
            return ("updates are off in the pack's settings (meowy_updates 0)".into(), None);
        }
        let body = fetch(DOC_PATH, release::MAX_BODY as u64).unwrap_or_default();
        let sig = if body.is_empty() { Vec::new() } else { fetch(SIG_PATH, 256).unwrap_or_default() };
        if body.is_empty() || sig.is_empty() {
            return ("no release published yet (or the site couldn't be reached)".into(), None);
        }
        if !body.starts_with(b"meowy-pack-release 1\n") {
            return ("no release published yet".into(), None);
        }
        if crate::policy::signed_by(keys, &body, &String::from_utf8_lossy(&sig)) != Some(crate::policy::Key::Release) {
            return ("the release document isn't signed by the release key".into(), None);
        }
        let r = match release::parse(&body) {
            Ok(r) => r,
            Err(why) => return (format!("the release document doesn't parse ({why})"), None),
        };
        let Some(token) = install_token(data) else { return ("no install token".into(), None) };
        let facts = Facts {
            running: running_version(data, garrysmod, arch, policy),
            stub: stub_version(garrysmod, arch),
            last_seq: last_seq(data),
            now,
            slot: rollout_slot(&token, &r.salt),
            bad: bad_list(data, arch),
            policy_allows: release::policy_allows(policy, &r.version),
            arch: arch.into(),
        };
        if let Err(why) = release::should_take(&r, &facts) {
            return (why, None);
        }
        if read_pointer(&state_path(data, "pending", arch)).is_some_and(|p| p.version == r.version) {
            return (format!("{} is ready: it starts next time you open Garry's Mod", r.version), None);
        }
        // The pack stages while the game runs; the launcher only while it doesn't.
        if game_running() {
            return ("Garry's Mod is running: the pack checks for updates itself".into(), None);
        }
        match stage(data, &r, arch, |path, size| fetch(path, size)) {
            Ok(said) => (said, Some(r.version)),
            Err(why) => (why, None),
        }
    }

}

#[cfg(all(test, windows))]
mod tests {
    use super::*;
    use crate::release::tests::doc;

    fn setup(name: &str) -> (PathBuf, PathBuf) {
        let root = std::env::temp_dir().join(format!("meowy_launcher_updater_{name}_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let data = root.join("MeowyClientPack");
        let gm = root.join("garrysmod");
        std::fs::create_dir_all(data.join("state")).unwrap();
        std::fs::create_dir_all(gm.join("lua\\bin")).unwrap();
        (data, gm)
    }

    #[test]
    fn updates_off_like_the_stub() {
        let (data, gm) = setup("off");
        let settings = data.join("menu_settings.txt");
        assert!(!updates_off(&data), "no settings file: on");
        std::fs::write(&settings, "meowy_loadingscreen=1\nmeowy_updates=1\n").unwrap();
        assert!(!updates_off(&data), "1 is on");
        std::fs::write(&settings, "meowy_loadingscreen=1\r\nmeowy_updates=0\r\n").unwrap();
        assert!(updates_off(&data), "0 is off");
        std::fs::write(&settings, "meowy_updates_extra=0\n").unwrap();
        assert!(!updates_off(&data), "only its own name");
        // Off: the check stops before fetching anything.
        std::fs::write(&settings, "meowy_updates=0\n").unwrap();
        let keys = crate::policy::Keys::production();
        let never = || false;
        let no_fetch = |_: &str, _: u64| -> Result<Vec<u8>, String> { panic!("fetched while updates are off") };
        let (said, staged) = check_once(&keys, &data, &gm, "win64", None, 0, &never, &no_fetch);
        assert!(said.contains("updates are off") && staged.is_none(), "{said}");
        std::fs::remove_dir_all(data.parent().unwrap()).unwrap();
    }

    #[test]
    fn sha256_and_token() {
        assert_eq!(sha256_hex(b"abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        let (data, _) = setup("token");
        let t = install_token(&data).unwrap();
        assert_eq!(t.len(), 32);
        assert_eq!(install_token(&data).unwrap(), t, "kept, not remade");
        assert!((0..100).contains(&rollout_slot(&t, "0123abcd")));
        std::fs::remove_dir_all(data.parent().unwrap()).unwrap();
    }

    #[test]
    fn running_version_from_outside() {
        let (data, gm) = setup("running");
        let dll = gm.join("lua\\bin").join(dll_name("win64"));
        std::fs::write(&dll, b"installed pack, no version resource").unwrap();
        assert_eq!(running_version(&data, &gm, "win64", None), None, "no version resource: not known");
        // A DLL with a version resource (kernel32 stands in for a pack built with VERSIONINFO).
        let sys = std::env::var("SystemRoot").unwrap_or_else(|_| "C:\\Windows".into());
        std::fs::copy(format!("{sys}\\System32\\kernel32.dll"), &dll).unwrap();
        let v = running_version(&data, &gm, "win64", None).expect("read from the DLL's VERSIONINFO");
        assert!(v.starts_with("10.") && release::valid_version(&v), "{v}");
        // A current pointer whose file matches wins over the installed copy.
        let staged = version_dll(&data, "1.7.0", "win64");
        std::fs::create_dir_all(staged.parent().unwrap()).unwrap();
        std::fs::write(&staged, b"1.7.0 dll").unwrap();
        let p = Pointer { version: "1.7.0".into(), sha256: sha256_hex(b"1.7.0 dll"), ok: 2 };
        std::fs::write(state_path(&data, "current", "win64"), p.text()).unwrap();
        assert_eq!(running_version(&data, &gm, "win64", None).as_deref(), Some("1.7.0"));
        std::fs::write(&staged, b"changed").unwrap();
        assert_eq!(running_version(&data, &gm, "win64", None), Some(v.clone()), "a current whose hash fails is passed over");
        std::fs::write(&staged, b"1.7.0 dll").unwrap();
        std::fs::write(state_path(&data, "bad", "win64"), "1.7.0\n").unwrap();
        assert_eq!(running_version(&data, &gm, "win64", None), Some(v), "a bad current is passed over");
        std::fs::remove_dir_all(data.parent().unwrap()).unwrap();
    }

    #[test]
    fn stages_like_the_pack() {
        let (data, _) = setup("stage");
        let dll = b"new pack dll bytes".to_vec();
        let sha = sha256_hex(&dll);
        let body = doc("").replace(crate::release::tests::SHA, &sha).replace(" 1234 ", &format!(" {} ", dll.len()));
        let r = crate::release::parse(body.as_bytes()).unwrap();
        // A wrong download: nothing staged, no pointer, seq floor unchanged.
        let wrong = stage(&data, &r, "win64", |_, size| Ok(vec![b'x'; size as usize]));
        assert!(wrong.unwrap_err().contains("didn't match its signed hash"));
        assert!(!state_path(&data, "pending", "win64").exists() && last_seq(&data) == 0);
        assert!(!data.join("bin\\1.7.0.partial").exists() && !data.join("bin\\1.7.0").exists());
        let short = stage(&data, &r, "win64", |_, _| Ok(b"short".to_vec()));
        assert!(short.unwrap_err().contains("couldn't download"));
        // The right one.
        std::fs::create_dir_all(data.join("bin\\0.9.0")).unwrap(); // nothing points here: cleaned up
        let ok = stage(&data, &r, "win64", |path, size| {
            assert_eq!((path, size), ("/pack/v1/1.7.0/gmcl_meowy_win64.dll", dll.len() as u64));
            Ok(dll.clone())
        });
        assert_eq!(ok.unwrap(), "1.7.0 is ready: it starts next time you open Garry's Mod");
        assert_eq!(std::fs::read(version_dll(&data, "1.7.0", "win64")).unwrap(), dll);
        assert_eq!(read_pointer(&state_path(&data, "pending", "win64")), Some(Pointer { version: "1.7.0".into(), sha256: sha, ok: 0 }));
        assert_eq!(last_seq(&data), 7);
        assert!(!data.join("bin\\0.9.0").exists(), "an unreferenced folder is cleaned up");
        assert!(!state_path(&data, "current", "win64").exists(), "current is the stub's");
        std::fs::remove_dir_all(data.parent().unwrap()).unwrap();
    }

    #[test]
    fn check_once_refuses_what_the_pack_would() {
        let (data, gm) = setup("check");
        let keys = crate::policy::Keys::production();
        let never = || false;
        let html = |_: &str, _: u64| Ok(b"<!doctype html>".to_vec());
        let (said, staged) = check_once(&keys, &data, &gm, "win64", None, 1800000000, &never, &html);
        assert_eq!((said.as_str(), staged), ("no release published yet", None), "the site's own page");
        let unsigned = |p: &str, _: u64| Ok(if p == DOC_PATH { doc("").into_bytes() } else { b"00".repeat(64) });
        let (said, _) = check_once(&keys, &data, &gm, "win64", None, 1800000000, &never, &unsigned);
        assert_eq!(said, "the release document isn't signed by the release key");
        let down = |_: &str, _: u64| Err("offline".to_string());
        assert!(check_once(&keys, &data, &gm, "win64", None, 1800000000, &never, &down).0.contains("couldn't be reached"));
        std::fs::remove_dir_all(data.parent().unwrap()).unwrap();
    }
}
