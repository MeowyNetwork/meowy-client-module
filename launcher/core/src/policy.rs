//! The pack's signed policy (Meowy Network's kill switch, K1), from the
//! launcher's side. The same document, keys, files and rules as
//! `src/policy.cpp`, so either one may fetch and keep a policy and the other
//! trusts it:
//!   - parsed strictly (`parse`, a port of `policy::Parse`);
//!   - signed with ECDSA P-256 / SHA-256 by one of the pack's keys, read at
//!     build time from `src/policy.cpp` (build.rs), checked through BCrypt;
//!   - kept in `%LOCALAPPDATA%\MeowyClientPack\state` (policy.txt, policy.sig,
//!     and policy.bin for the start-up stub) only if `may_replace` allows:
//!     a higher seq, and the ops and recovery keys may only make it stricter.
//!
//! The launcher fetches the same files while the game is closed, so the
//! stub's policy.bin is current before the pack would load, and obeys
//! `off launcher` (all of its own features) and `off launcher_<feature>`.

mod keys {
    include!(concat!(env!("OUT_DIR"), "/policy_keys.rs"));
}

pub const MAX_BODY: usize = 8 * 1024;
pub const HOST: &str = "meowynetwork.com";
pub const TXT_PATH: &str = "/pack/v1/policy.txt";
pub const SIG_PATH: &str = "/pack/v1/policy.sig";

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Policy {
    pub seq: u64,
    pub pack_enabled: bool,
    pub min_version: String,
    pub off: Vec<String>,
    pub revoked: Vec<String>,
    pub notice: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Key {
    Release,
    Ops,
    Recovery,
}

fn digits(s: &str, max: usize) -> bool {
    !s.is_empty() && s.len() <= max && s.bytes().all(|b| b.is_ascii_digit())
}

/// 1 to 4 dot-separated numbers of 1 to 5 digits.
fn version_text(s: &str) -> bool {
    let parts: Vec<&str> = s.split('.').collect();
    (1..=4).contains(&parts.len()) && parts.iter().all(|p| digits(p, 5))
}

fn name_text(s: &str) -> bool {
    !s.is_empty() && s.len() <= 48 && s.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_')
}

/// A strict parse, as `policy::Parse`. Err names what's wrong.
pub fn parse(body: &[u8]) -> Result<Policy, &'static str> {
    if body.is_empty() || body.len() > MAX_BODY {
        return Err("size");
    }
    if *body.last().unwrap() != b'\n' || body.contains(&b'\r') || body.contains(&0) {
        return Err("line ends");
    }
    let mut p = Policy::default();
    let (mut seq, mut enabled, mut min, mut notice) = (false, false, false, false);
    for (n, line) in body[..body.len() - 1].split(|&b| b == b'\n').enumerate() {
        if n == 0 {
            if line != b"meowy-pack-policy 1" {
                return Err("header");
            }
            continue;
        }
        let Some(space) = line.iter().position(|&b| b == b' ') else { return Err("line") };
        if space == 0 || space + 1 >= line.len() {
            return Err("line");
        }
        let kw = &line[..space];
        let value = &line[space + 1..];
        // Values other than a notice are ASCII by their rules; a notice must be UTF-8 without controls.
        let text = std::str::from_utf8(value).ok();
        match kw {
            b"seq" => {
                let v = text.ok_or("seq")?;
                if seq || !digits(v, 19) || v.starts_with('0') {
                    return Err("seq");
                }
                p.seq = v.parse().map_err(|_| "seq")?;
                seq = true;
            }
            b"pack_enabled" => {
                if enabled || (value != b"0" && value != b"1") {
                    return Err("pack_enabled");
                }
                p.pack_enabled = value == b"1";
                enabled = true;
            }
            b"min_version" => {
                let v = text.ok_or("min_version")?;
                if min || !version_text(v) {
                    return Err("min_version");
                }
                p.min_version = v.into();
                min = true;
            }
            b"off" => {
                let v = text.ok_or("off")?;
                if !name_text(v) || p.off.iter().any(|o| o == v) || p.off.len() >= 64 {
                    return Err("off");
                }
                p.off.push(v.into());
            }
            b"revoked" => {
                let v = text.ok_or("revoked")?;
                if !version_text(v) || p.revoked.iter().any(|o| o == v) || p.revoked.len() >= 64 {
                    return Err("revoked");
                }
                p.revoked.push(v.into());
            }
            b"notice" => {
                let v = text.ok_or("notice")?;
                if notice || v.len() > 200 || v.bytes().any(|b| b < 0x20 || b == 0x7F) {
                    return Err("notice");
                }
                p.notice = v.into();
                notice = true;
            }
            _ => {
                // A keyword from a newer policy: skipped, if it looks like one.
                if !kw.iter().all(|&b| b.is_ascii_lowercase() || b == b'_') {
                    return Err("keyword");
                }
            }
        }
    }
    if !seq || !enabled {
        return Err(if seq { "no pack_enabled" } else { "no seq" });
    }
    Ok(p)
}

/// "1.6.0" vs "1.10": numeric parts, missing ones are 0.
pub fn compare_versions(a: &str, b: &str) -> std::cmp::Ordering {
    let parts = |s: &str| s.split('.').map(|p| p.parse::<u64>().unwrap_or(0)).collect::<Vec<_>>();
    let (x, y) = (parts(a), parts(b));
    for i in 0..x.len().max(y.len()) {
        let o = x.get(i).unwrap_or(&0).cmp(y.get(i).unwrap_or(&0));
        if o.is_ne() {
            return o;
        }
    }
    std::cmp::Ordering::Equal
}

/// Seqs from 10^15 up are the release key's alone; the ops and recovery keys
/// step at most `MAX_STEP_SEQ` above the kept seq (above 0 when none is kept).
/// The same numbers as `policy::kReleaseOnlySeq` / `kMaxStepSeq`.
pub const RELEASE_ONLY_SEQ: u64 = 1_000_000_000_000_000;
pub const MAX_STEP_SEQ: u64 = 1000;

/// Whether `next`, signed by `key`, may replace `cur` (`policy::MayReplace`).
pub fn may_replace(cur: Option<&Policy>, next: &Policy, key: Key) -> Result<(), &'static str> {
    if let Some(cur) = cur {
        if next.seq <= cur.seq {
            return Err("not newer than the one kept");
        }
    }
    if key == Key::Release {
        return Ok(());
    }
    // The ops and recovery keys: a seq the release key can always beat, so a
    // stolen key can't freeze the policy for good; and no notice of their own.
    let base = cur.map_or(0, |c| c.seq);
    if next.seq >= RELEASE_ONLY_SEQ || next.seq - base > MAX_STEP_SEQ {
        return Err("the ops key can't raise the seq that far");
    }
    if !next.notice.is_empty() && cur.is_none_or(|c| next.notice != c.notice) {
        return Err("the ops key can't set a notice");
    }
    let Some(cur) = cur else { return Ok(()) };
    if next.pack_enabled && !cur.pack_enabled {
        return Err("the ops key can't switch the pack back on");
    }
    if cur.off.iter().any(|o| !next.off.contains(o)) {
        return Err("the ops key can't switch a feature back on");
    }
    if cur.revoked.iter().any(|r| !next.revoked.contains(r)) {
        return Err("the ops key can't un-revoke a version");
    }
    if !cur.min_version.is_empty() && (next.min_version.is_empty() || compare_versions(&next.min_version, &cur.min_version).is_lt()) {
        return Err("the ops key can't lower min_version");
    }
    Ok(())
}

/// Whether the launcher may use `feature` ("repair", "fast_start", "links",
/// "servers") under `policy` (None: no policy kept, everything on).
pub fn launcher_allows(policy: Option<&Policy>, feature: &str) -> bool {
    let Some(p) = policy else { return true };
    let off = |name: &str| p.off.iter().any(|o| o == name);
    if off("launcher") || off(&format!("launcher_{feature}")) {
        return false;
    }
    // With the pack switched off, nothing that prepares the pack's start.
    !(matches!(feature, "repair" | "fast_start") && !p.pack_enabled)
}

/// What the start-up stub reads (`WriteStubFile`, line for line): it passes
/// over a downloaded version below min_version or revoked (pack_versions.h).
pub fn stub_file(p: &Policy) -> String {
    let mut s = format!("enabled {}\nseq {}\n", if p.pack_enabled { 1 } else { 0 }, p.seq);
    if !p.min_version.is_empty() {
        s += &format!("min_version {}\n", p.min_version);
    }
    for r in &p.revoked {
        s += &format!("revoked {r}\n");
    }
    if !p.notice.is_empty() {
        s += &format!("notice {}\n", p.notice);
    }
    s
}

fn hex(s: &str) -> Option<Vec<u8>> {
    if s.len() % 2 != 0 || !s.is_ascii() {
        return None;
    }
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).ok()).collect()
}

/// The keys: the pack's, or a test's.
#[derive(Clone, Copy)]
pub struct Keys {
    pub release: Option<[u8; 64]>,
    pub ops: Option<[u8; 64]>,
    pub recovery: Option<[u8; 64]>,
}

impl Keys {
    pub const fn production() -> Keys {
        Keys { release: keys::RELEASE, ops: keys::OPS, recovery: keys::RECOVERY }
    }
    pub fn any(&self) -> bool {
        self.release.is_some() || self.ops.is_some() || self.recovery.is_some()
    }
}

/// Which key signed exactly `body` (`policy::SignedBy`).
#[cfg(windows)]
pub fn signed_by(keys: &Keys, body: &[u8], sig_hex: &str) -> Option<Key> {
    let sig = sig_hex.trim();
    if sig.len() != 128 {
        return None;
    }
    let sig = hex(sig)?;
    [(keys.release, Key::Release), (keys.ops, Key::Ops), (keys.recovery, Key::Recovery)]
        .into_iter()
        .find(|(k, _)| k.is_some_and(|k| crate::win::verify_p256(&k, body, &sig)))
        .map(|(_, which)| which)
}

#[cfg(windows)]
pub use store::*;

#[cfg(windows)]
mod store {
    use super::*;
    use std::path::{Path, PathBuf};

    pub fn state_dir() -> Option<PathBuf> {
        crate::settings::pack_data_dir().map(|d| d.join("state"))
    }

    fn write_atomic(path: &Path, data: &[u8]) -> std::io::Result<()> {
        let mut tmp = path.as_os_str().to_owned();
        tmp.push(".tmp");
        std::fs::write(&tmp, data)?;
        std::fs::rename(&tmp, path)
    }

    /// The policy kept in `dir`, verified again (`policy::Current`).
    pub fn current(keys: &Keys, dir: &Path) -> Option<(Policy, Key)> {
        let body = std::fs::read(dir.join("policy.txt")).ok().filter(|b| b.len() <= MAX_BODY)?;
        let sig = std::fs::read_to_string(dir.join("policy.sig")).ok()?;
        let key = signed_by(keys, &body, &sig)?;
        Some((parse(&body).ok()?, key))
    }

    /// Held around every read-check-write of the kept files, by this process and
    /// the pack in the game (`policy::kFileMutexName`, the same name), so neither
    /// puts an older policy back over one the other just kept. A holder stuck
    /// for 10 s is gone ahead without.
    pub const FILE_MUTEX_NAME: &str = "Local\\MeowyClientPack.Policy";

    pub struct FileGuard(windows_sys::Win32::Foundation::HANDLE, bool);

    impl FileGuard {
        pub fn take() -> FileGuard {
            use windows_sys::Win32::Foundation::{WAIT_ABANDONED, WAIT_OBJECT_0};
            use windows_sys::Win32::System::Threading::{CreateMutexW, WaitForSingleObject};
            let name: Vec<u16> = FILE_MUTEX_NAME.encode_utf16().chain(Some(0)).collect();
            // SAFETY: a valid name; the handle is closed in Drop.
            unsafe {
                let h = CreateMutexW(std::ptr::null(), 0, name.as_ptr());
                if h.is_null() {
                    return FileGuard(h, false);
                }
                let r = WaitForSingleObject(h, 10_000);
                FileGuard(h, r == WAIT_OBJECT_0 || r == WAIT_ABANDONED)
            }
        }
    }

    impl Drop for FileGuard {
        fn drop(&mut self) {
            use windows_sys::Win32::Foundation::CloseHandle;
            use windows_sys::Win32::System::Threading::ReleaseMutex;
            // SAFETY: our own handle; released only if this thread owns it.
            unsafe {
                if self.1 {
                    ReleaseMutex(self.0);
                }
                if !self.0.is_null() {
                    CloseHandle(self.0);
                }
            }
        }
    }

    /// The seq in a stub file's text, if it has one.
    fn stub_seq(text: &str) -> Option<u64> {
        text.lines().find_map(|l| l.strip_prefix("seq ")).and_then(|s| s.parse().ok())
    }

    /// policy.bin written again from the kept policy when it differs (a stub
    /// file from an older launcher lacked min_version and revoked), but never
    /// with a lower seq than it has (a newer policy being kept by a process that
    /// doesn't take the mutex, such as the 1.7.0 pack). Whether it was rewritten.
    pub fn heal_stub(keys: &Keys, dir: &Path) -> bool {
        let _files = FileGuard::take();
        let Some((p, _)) = current(keys, dir) else { return false };
        let want = stub_file(&p);
        let path = dir.join("policy.bin");
        let have = std::fs::read_to_string(&path).ok();
        if have.as_deref() == Some(want.as_str()) || have.as_deref().and_then(stub_seq).is_some_and(|s| s > p.seq) {
            return false;
        }
        write_atomic(&path, want.as_bytes()).is_ok()
    }

    /// A fetched policy: kept if it verifies, parses and may replace the kept
    /// one (`policy::Offer`). Ok(seq) when it was kept.
    pub fn offer(keys: &Keys, dir: &Path, body: &[u8], sig_hex: &str) -> Result<u64, String> {
        let sig = sig_hex.trim();
        let key = signed_by(keys, body, sig).ok_or("not signed by one of our keys")?;
        let next = parse(body).map_err(|why| format!("doesn't parse ({why})"))?;
        let _files = FileGuard::take();
        let cur = current(keys, dir).map(|(p, _)| p);
        may_replace(cur.as_ref(), &next, key)?;
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
        write_atomic(&dir.join("policy.txt"), body)
            .and_then(|_| write_atomic(&dir.join("policy.sig"), format!("{sig}\n").as_bytes()))
            .and_then(|_| write_atomic(&dir.join("policy.bin"), stub_file(&next).as_bytes()))
            .map_err(|e| format!("couldn't be kept ({e})"))?;
        Ok(next.seq)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // tests/policy_smoke.cpp's vectors (.NET-signed, like sign-policy.ps1).
    const T_RELEASE: &str = "ce0f613c979afd27f505b74c81bd7a2d290302978571a52b85d50b8a628e63768f48ffaab29d552511cf25016031400623cb01f1d7902e106d66d262aa66f3ff";
    const T_OPS: &str = "23382955a362316831cf2b419eb3a6614daf64296b737ed1522a51578aaf67b00de5190d1911473e011ca6a549a5eec070f83f1799a095ae53049eedac68b075";
    const T_RECOVERY: &str = "060434149f915e4c92e5172341aef705f3b9e59e6d794fc12d3f43c0283280c781a9d6f6f8446406f42f07cf32bdc8c17a4364aeb2347bfac497d5434d72b430";
    const P1: (&str, &str) = ("meowy-pack-policy 1\nseq 1\npack_enabled 1\noff meowy_pak_cache\n", "a347bd868e414cc51d08d9fc7b2813ab45afc1d38dd73f6bb78951261533c6ffc76d1e6f530788cc19116157015d0097ee0b95bc0a9dcd557bf4f5c6cecdbb1b");
    const P2: (&str, &str) = ("meowy-pack-policy 1\nseq 2\npack_enabled 1\noff meowy_pak_cache\noff meowy_prewarm\nrevoked 1.6.1\n", "db43a9d071a2165c03f6e65316665a01307c21893ffae0fa09ce66dfdb645bce3ed57b21305e345463e54be1971f69d92b6c739651184c4d408e67cf2b0f1001");
    const P3: (&str, &str) = ("meowy-pack-policy 1\nseq 3\npack_enabled 1\n", "e8d61975cc0e9d86fc63dfe9c3d5944a8ec0bb06cae4f18acb9f656d6c547707b7563b9ecad5395c137b527bcbfeb6f50051afdfad892bbf534e998c63d2f9cb");
    const P4: (&str, &str) = ("meowy-pack-policy 1\nseq 4\npack_enabled 0\noff meowy_pak_cache\noff meowy_prewarm\nrevoked 1.6.1\n", "61c915601ec02c8f76ef8801d06c3f9f8789e45738be4e550cbe6fd35ff7a4db1ecb2e8e6c5f43e8fffc8876b1e2c78f7936470b84f17cff0333d4044562756e");
    // The recovery key with a notice of its own, and the ops key at the largest seq: both refused.
    const P4N: (&str, &str) = ("meowy-pack-policy 1\nseq 5\npack_enabled 0\noff meowy_pak_cache\noff meowy_prewarm\nrevoked 1.6.1\nnotice Update at evil.example to keep playing.\n", "a3b2cc256ca1d66fd9d477d96f2dd512955634b3c0c28ed890475d16ce8563e9038051b58162f100274fe8533e02eb791b321e394c3f5bdefe890a408745cd2e");
    const PMAX: (&str, &str) = ("meowy-pack-policy 1\nseq 9999999999999999999\npack_enabled 0\noff meowy_pak_cache\noff meowy_prewarm\nrevoked 1.6.1\n", "45182755e76cc52910f92525b9ea2e3500061913562bc15e01f7a5671ddebb324f404c4d0bc06eca065e7d79297e706f1501d6cc9e3af17ec15bc61563e0c220");
    const P5: (&str, &str) = ("meowy-pack-policy 1\nseq 5\npack_enabled 1\nmin_version 1.6.0\nfuture_field whatever it is\nnotice Back on.\n", "7b4fc4732ba87d28f0565ea104ef29d423b8c81f939709fde0fe6f4db9d7ad26a4caa01c556fcb28dabf4532cf3ad379d1680dc02af36958731c8ff3784946b1");
    // Signed with the production release key (the pack's own check of its keys).
    const REAL: (&str, &str) = ("meowy-pack-policy 1\nseq 1\npack_enabled 1\n", "2fe5a03a75b75fefd541d0db5578fb8b77708b3a336e6038c1e83ce39a994f0a211ab51eb34b7bba73b7e5093bd336824a2401abf97dc6b951af0b5e8ac2ea46");

    fn key(h: &str) -> Option<[u8; 64]> {
        hex(h).map(|v| v.try_into().unwrap())
    }
    fn test_keys() -> Keys {
        Keys { release: key(T_RELEASE), ops: key(T_OPS), recovery: key(T_RECOVERY) }
    }

    #[test]
    fn parses_like_the_pack() {
        let p = parse(P4N.0.as_bytes()).unwrap();
        assert_eq!((p.seq, p.pack_enabled, p.notice.as_str()), (5, false, "Update at evil.example to keep playing."));
        assert_eq!(p.off, vec!["meowy_pak_cache", "meowy_prewarm"]);
        assert_eq!(p.revoked, vec!["1.6.1"]);
        assert_eq!(parse(P5.0.as_bytes()).unwrap().min_version, "1.6.0", "an unknown keyword is skipped");
        for (bad, why) in [
            ("", "size"),
            ("meowy-pack-policy 1\nseq 1\npack_enabled 1", "line ends"),
            ("meowy-pack-policy 1\r\nseq 1\npack_enabled 1\n", "line ends"),
            ("meowy-pack-policy 2\nseq 1\npack_enabled 1\n", "header"),
            ("meowy-pack-policy 1\nseq 01\npack_enabled 1\n", "seq"),
            ("meowy-pack-policy 1\nseq 1\nseq 2\npack_enabled 1\n", "seq"),
            ("meowy-pack-policy 1\nseq 1\npack_enabled 2\n", "pack_enabled"),
            ("meowy-pack-policy 1\nseq 1\npack_enabled 1\noff Meowy\n", "off"),
            ("meowy-pack-policy 1\nseq 1\npack_enabled 1\noff a\noff a\n", "off"),
            ("meowy-pack-policy 1\nseq 1\npack_enabled 1\nrevoked 1.2.3.4.5\n", "revoked"),
            ("meowy-pack-policy 1\nseq 1\npack_enabled 1\nnotice a\u{7}b\n", "notice"),
            ("meowy-pack-policy 1\nseq 1\npack_enabled 1\nNew-Key x\n", "keyword"),
            ("meowy-pack-policy 1\nseq 1\n", "no pack_enabled"),
            ("meowy-pack-policy 1\npack_enabled 1\n", "no seq"),
            ("meowy-pack-policy 1\nseq 1\npack_enabled 1\nnoline\n", "line"),
        ] {
            assert_eq!(parse(bad.as_bytes()), Err(why), "{bad:?}");
        }
        assert_eq!(parse(b"meowy-pack-policy 1\nseq 1\npack_enabled 1\nnotice \xff\n"), Err("notice"), "bad UTF-8");
    }

    #[test]
    fn versions_and_replacing() {
        use std::cmp::Ordering::*;
        assert_eq!(compare_versions("1.6.0", "1.10"), Less);
        assert_eq!(compare_versions("1.6", "1.6.0"), Equal);
        let p = |b: &str| parse(b.as_bytes()).unwrap();
        let (p1, p2, p3, p4) = (p(P1.0), p(P2.0), p(P3.0), p(P4.0));
        assert_eq!(may_replace(None, &p1, Key::Ops), Ok(()), "anything over none");
        assert_eq!(may_replace(Some(&p1), &p2, Key::Ops), Ok(()), "ops: stricter");
        assert!(may_replace(Some(&p2), &p3, Key::Ops).is_err(), "ops can't switch features back on");
        assert_eq!(may_replace(Some(&p2), &p3, Key::Release), Ok(()), "release may");
        assert_eq!(may_replace(Some(&p3), &p4, Key::Recovery), Ok(()), "recovery: off");
        assert!(may_replace(Some(&p4), &p1, Key::Release).is_err(), "never an older seq");
    }

    /// A stolen ops or recovery key can't sign a seq the release key can't beat,
    /// or a notice of its own (`policy_smoke`'s same checks).
    #[test]
    fn ops_seq_and_notice() {
        let p = |b: &str| parse(b.as_bytes()).unwrap();
        let cur = p("meowy-pack-policy 1\nseq 1\npack_enabled 1\nnotice Maintenance tonight.\n");
        let max = p("meowy-pack-policy 1\nseq 9999999999999999999\npack_enabled 0\n");
        assert_eq!(max.seq, 9999999999999999999);
        for key in [Key::Ops, Key::Recovery] {
            assert!(may_replace(Some(&cur), &max, key).is_err(), "{key:?}: seq 9999999999999999999");
            assert!(may_replace(None, &max, key).is_err(), "{key:?}: seq 9999999999999999999 with none kept");
        }
        assert_eq!(may_replace(Some(&cur), &max, Key::Release), Ok(()), "release: any seq");
        let mut next = Policy { seq: 1 + MAX_STEP_SEQ, pack_enabled: false, notice: cur.notice.clone(), ..Default::default() };
        assert_eq!(may_replace(Some(&cur), &next, Key::Ops), Ok(()), "a step of MAX_STEP_SEQ, keeping the notice");
        next.seq += 1;
        assert!(may_replace(Some(&cur), &next, Key::Ops).is_err(), "one more");
        assert!(may_replace(None, &next, Key::Ops).is_err(), "none kept: up to MAX_STEP_SEQ");
        let near = Policy { seq: RELEASE_ONLY_SEQ - 2, ..cur.clone() };
        next.seq = RELEASE_ONLY_SEQ - 1;
        assert_eq!(may_replace(Some(&near), &next, Key::Ops), Ok(()), "just below the release key's range");
        let edge = Policy { seq: RELEASE_ONLY_SEQ - 1, ..cur.clone() };
        next.seq = RELEASE_ONLY_SEQ;
        assert!(may_replace(Some(&edge), &next, Key::Ops).is_err(), "the release key's range");
        assert_eq!(may_replace(Some(&edge), &next, Key::Release), Ok(()));
        next.seq = 2;
        next.notice.clear();
        assert_eq!(may_replace(Some(&cur), &next, Key::Ops), Ok(()), "dropping the notice");
        next.notice = "Download the fix at evil.example".into();
        assert!(may_replace(Some(&cur), &next, Key::Ops).is_err(), "a new notice");
        assert!(may_replace(Some(&cur), &next, Key::Recovery).is_err(), "a new notice (recovery)");
        assert!(may_replace(None, &next, Key::Ops).is_err(), "a notice with none kept");
        assert_eq!(may_replace(Some(&cur), &next, Key::Release), Ok(()), "release: any notice");
    }

    #[test]
    fn launcher_switches() {
        let p = |b: &str| parse(b.as_bytes()).unwrap();
        assert!(launcher_allows(None, "repair"));
        let on = p("meowy-pack-policy 1\nseq 1\npack_enabled 1\n");
        assert!(launcher_allows(Some(&on), "fast_start"));
        let off_one = p("meowy-pack-policy 1\nseq 1\npack_enabled 1\noff launcher_fast_start\n");
        assert!(!launcher_allows(Some(&off_one), "fast_start") && launcher_allows(Some(&off_one), "links"));
        let off_all = p("meowy-pack-policy 1\nseq 1\npack_enabled 1\noff launcher\n");
        assert!(!launcher_allows(Some(&off_all), "links") && !launcher_allows(Some(&off_all), "servers"));
        let pack_off = p("meowy-pack-policy 1\nseq 1\npack_enabled 0\n");
        assert!(!launcher_allows(Some(&pack_off), "repair") && !launcher_allows(Some(&pack_off), "fast_start"));
        assert!(launcher_allows(Some(&pack_off), "links"), "joining still works with the pack off");
        // Line for line what the pack's WriteStubFile writes (tests/policy_smoke.cpp).
        assert_eq!(stub_file(&p(P4.0)), "enabled 0\nseq 4\nrevoked 1.6.1\n");
        assert_eq!(stub_file(&p(P5.0)), "enabled 1\nseq 5\nmin_version 1.6.0\nnotice Back on.\n");
    }

    #[cfg(windows)]
    #[test]
    fn signatures_like_the_pack() {
        let k = test_keys();
        assert_eq!(signed_by(&k, P1.0.as_bytes(), P1.1), Some(Key::Release));
        assert_eq!(signed_by(&k, P2.0.as_bytes(), P2.1), Some(Key::Ops));
        assert_eq!(signed_by(&k, P4.0.as_bytes(), P4.1), Some(Key::Recovery));
        let mut tampered = P1.0.as_bytes().to_vec();
        let at = tampered.len() - 2;
        tampered[at] ^= 1;
        assert_eq!(signed_by(&k, &tampered, P1.1), None, "a changed byte");
        assert_eq!(signed_by(&k, P1.0.as_bytes(), P3.1), None, "another body's signature");
        assert_eq!(signed_by(&k, P1.0.as_bytes(), "abcd"), None);
        assert_eq!(signed_by(&Keys::production(), P1.0.as_bytes(), P1.1), None, "test vectors aren't production keys");
    }

    /// The keys build.rs read out of src/policy.cpp are the ones the pack
    /// uses: the real first policy verifies with the production release key.
    #[cfg(windows)]
    #[test]
    fn production_keys_from_the_pack() {
        let k = Keys::production();
        assert!(k.release.is_some() && k.ops.is_some() && k.recovery.is_some(), "src/policy.cpp has its keys");
        assert_eq!(signed_by(&k, REAL.0.as_bytes(), REAL.1), Some(Key::Release));
    }

    #[cfg(windows)]
    #[test]
    fn keeps_like_the_pack() {
        let dir = std::env::temp_dir().join(format!("meowy_launcher_policy_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let k = test_keys();
        assert_eq!(offer(&k, &dir, P1.0.as_bytes(), P1.1), Ok(1));
        assert_eq!(current(&k, &dir).map(|(p, key)| (p.seq, key)), Some((1, Key::Release)));
        assert_eq!(offer(&k, &dir, P2.0.as_bytes(), &format!("{}\n", P2.1)), Ok(2), "ops, stricter; sig with a line end");
        assert!(offer(&k, &dir, P3.0.as_bytes(), P2.1).is_err(), "wrong signature");
        assert!(offer(&k, &dir, P1.0.as_bytes(), P1.1).is_err(), "an old seq can't be replayed");
        assert_eq!(offer(&k, &dir, P4.0.as_bytes(), P4.1), Ok(4));
        assert_eq!(std::fs::read_to_string(dir.join("policy.bin")).unwrap(), "enabled 0\nseq 4\nrevoked 1.6.1\n");
        assert!(offer(&k, &dir, P4N.0.as_bytes(), P4N.1).is_err(), "recovery: a notice of its own");
        assert!(offer(&k, &dir, PMAX.0.as_bytes(), PMAX.1).is_err(), "ops: seq 9999999999999999999");
        // A stub file from an older launcher (no revoked line) is written again.
        std::fs::write(dir.join("policy.bin"), b"enabled 0\nseq 4\n").unwrap();
        assert!(heal_stub(&k, &dir));
        assert_eq!(std::fs::read_to_string(dir.join("policy.bin")).unwrap(), "enabled 0\nseq 4\nrevoked 1.6.1\n");
        assert!(!heal_stub(&k, &dir), "already right");
        // Never a lower seq than the stub file has (a newer one being kept elsewhere).
        std::fs::write(dir.join("policy.bin"), b"enabled 1\nseq 5\n").unwrap();
        assert!(!heal_stub(&k, &dir), "never lowers the seq");
        assert_eq!(std::fs::read_to_string(dir.join("policy.bin")).unwrap(), "enabled 1\nseq 5\n");
        // The pack holding the named mutex (here another thread): the heal waits for it.
        std::fs::write(dir.join("policy.bin"), b"enabled 0\nseq 4\n").unwrap();
        let (held_tx, held_rx) = std::sync::mpsc::channel();
        let holder = std::thread::spawn(move || {
            let guard = FileGuard::take();
            held_tx.send(()).unwrap();
            std::thread::sleep(std::time::Duration::from_millis(400));
            drop(guard);
        });
        held_rx.recv().unwrap();
        let start = std::time::Instant::now();
        assert!(heal_stub(&k, &dir));
        assert!(start.elapsed() >= std::time::Duration::from_millis(300), "waited for the other holder");
        holder.join().unwrap();
        assert_eq!(FILE_MUTEX_NAME, "Local\\MeowyClientPack.Policy", "the name src/policy.h has");
        assert_eq!(offer(&k, &dir, P5.0.as_bytes(), P5.1), Ok(5), "release: back on");
        std::fs::write(dir.join("policy.txt"), b"meowy-pack-policy 1\nseq 9\npack_enabled 1\n").unwrap();
        assert_eq!(current(&k, &dir), None, "an edited file is ignored");
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
