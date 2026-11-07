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

/// Whether `next`, signed by `key`, may replace `cur` (`policy::MayReplace`).
pub fn may_replace(cur: Option<&Policy>, next: &Policy, key: Key) -> Result<(), &'static str> {
    let Some(cur) = cur else { return Ok(()) };
    if next.seq <= cur.seq {
        return Err("not newer than the one kept");
    }
    if key == Key::Release {
        return Ok(());
    }
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

/// What the start-up stub reads (`WriteStubFile`).
pub fn stub_file(p: &Policy) -> String {
    let mut s = format!("enabled {}\nseq {}\n", if p.pack_enabled { 1 } else { 0 }, p.seq);
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

    /// A fetched policy: kept if it verifies, parses and may replace the kept
    /// one (`policy::Offer`). Ok(seq) when it was kept.
    pub fn offer(keys: &Keys, dir: &Path, body: &[u8], sig_hex: &str) -> Result<u64, String> {
        let sig = sig_hex.trim();
        let key = signed_by(keys, body, sig).ok_or("not signed by one of our keys")?;
        let next = parse(body).map_err(|why| format!("doesn't parse ({why})"))?;
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
    const T_RELEASE: &str = "f530598b2c2aa7fb0aa05a1a33f7fb471ea56b878e82e83e4c9290a2181b2ba38a29edf32476d150c86b66821d5416c12294be78bf44550ece9c1065322efb9f";
    const T_OPS: &str = "e0d84f65f3589c5451c976af9da54fa32826806ee092067ea619f33e9bfaf89a9a920435803d2b33c4b492dc18b2ffa66dd998bc9afc2c74e6dc77981bf4e86c";
    const T_RECOVERY: &str = "78a09ae8854b931eb4a3782ab0726000a8a900407dd61fac7631a1bc0492d9ae19f81f09013c1118ed26f55108888307cae65bd839608f055374155a0d7e4ce1";
    const P1: (&str, &str) = ("meowy-pack-policy 1\nseq 1\npack_enabled 1\noff meowy_pak_cache\n", "90f879c83ad608f679e2f66797cb132331ee464b3b0d65160cc0391a8d72c8a989e37fdae652378a80620aeeaf6b27a63f7a924d0ea39c7d9207d3b8bd515dcb");
    const P2: (&str, &str) = ("meowy-pack-policy 1\nseq 2\npack_enabled 1\noff meowy_pak_cache\noff meowy_prewarm\nrevoked 1.6.1\n", "da20f19618897f98418697809c8f036f8eff4c01a74969f55ff100037158d135b3e438ac8d6fa50122fe8f165ca3735d8f6b04799317b3dac1918241fdcf108d");
    const P3: (&str, &str) = ("meowy-pack-policy 1\nseq 3\npack_enabled 1\n", "e4235645efbf4614c326607080d3406b1ef0a13e7625b730e20ae5d909aa5e68382b76bff8cd666327bef0ae96f87b081d8d90262dc8900f6c6a71b16c60e44e");
    const P4: (&str, &str) = ("meowy-pack-policy 1\nseq 4\npack_enabled 0\noff meowy_pak_cache\noff meowy_prewarm\nrevoked 1.6.1\nnotice Paused while we fix a crash.\n", "18eff360838711746675f7e14aed6ae91ee445c446d0a21020f2270986451829d90cf172100eb173b595889956687f6544dace6726f82fa8f53832a883a30307");
    const P5: (&str, &str) = ("meowy-pack-policy 1\nseq 5\npack_enabled 1\nmin_version 1.6.0\nfuture_field whatever it is\n", "571e28c3eccf93d369610fdb8e8fa2a04909f527dce518cfeb1c88305c10511a4f41e975e715eabf6e6d58ee4dec4ccc0edfa8d11185870002cf8a6ce37fd93d");
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
        let p = parse(P4.0.as_bytes()).unwrap();
        assert_eq!((p.seq, p.pack_enabled, p.notice.as_str()), (4, false, "Paused while we fix a crash."));
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
        assert_eq!(stub_file(&p(P4.0)), "enabled 0\nseq 4\nnotice Paused while we fix a crash.\n");
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
        assert_eq!(std::fs::read_to_string(dir.join("policy.bin")).unwrap(), "enabled 0\nseq 4\nnotice Paused while we fix a crash.\n");
        std::fs::write(dir.join("policy.txt"), b"meowy-pack-policy 1\nseq 9\npack_enabled 1\n").unwrap();
        assert_eq!(current(&k, &dir), None, "an edited file is ignored");
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
