//! The pack's release documents (U1), as `src/updater.cpp` reads them:
//!
//! ```text
//! meowy-pack-release 1
//! seq <decimal>                       higher than any taken before
//! version <x.y.z>
//! expires <unix seconds>
//! min_stub <n>
//! rollout <0..100>
//! salt <8..64 hex>
//! file win64 <size> <sha256> <path>   path on meowynetwork.com, under /pack/v1/
//! file win32 <size> <sha256> <path>
//! notes <path>                        optional
//! ```
//!
//! A port of `updater::Parse` and `updater::ShouldTake`, with the same rules and
//! the same reasons, so the launcher stages exactly what the pack would.

pub const MAX_BODY: usize = 4 * 1024;
/// `pack_versions::kMaxDll`.
pub const MAX_DLL: u64 = 32 * 1024 * 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct File {
    pub arch: String,
    pub size: u64,
    pub sha256: String,
    pub path: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Release {
    pub seq: u64,
    pub version: String,
    pub expires: u64,
    pub min_stub: i32,
    pub rollout: i32,
    pub salt: String,
    pub files: Vec<File>,
    pub notes: String,
}

fn digits(s: &str, max: usize) -> bool {
    !s.is_empty() && s.len() <= max && s.bytes().all(|b| b.is_ascii_digit())
}

fn hex(s: &str, min: usize, max: usize) -> bool {
    (min..=max).contains(&s.len()) && s.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

/// `pack_versions::ValidVersion`: 1 to 4 numbers of up to 5 digits.
pub fn valid_version(v: &str) -> bool {
    let parts: Vec<&str> = v.split('.').collect();
    (1..=4).contains(&parts.len()) && parts.iter().all(|p| digits(p, 5))
}

/// `pack_versions::ValidSha256`: 64 lowercase hex digits.
pub fn valid_sha256(h: &str) -> bool {
    hex(h, 64, 64)
}

/// `/pack/v1/` then letters, digits, '.', '_', '-', '/', no "..", no "//", not ending in '/'.
fn site_path(p: &str) -> bool {
    p.len() <= 200
        && p.starts_with("/pack/v1/")
        && !p.contains("..")
        && !p.contains("//")
        && p.bytes().all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'-' | b'/'))
        && !p.ends_with('/')
}

pub fn parse(body: &[u8]) -> Result<Release, &'static str> {
    if body.is_empty() || body.len() > MAX_BODY {
        return Err("size");
    }
    if *body.last().unwrap() != b'\n' || body.contains(&b'\r') || body.contains(&0) {
        return Err("line ends");
    }
    let text = std::str::from_utf8(body).map_err(|_| "line")?;
    let mut r = Release::default();
    let (mut seq, mut version, mut expires, mut stub, mut rollout, mut salt, mut notes) = (false, false, false, false, false, false, false);
    for (n, line) in text[..text.len() - 1].split('\n').enumerate() {
        if n == 0 {
            if line != "meowy-pack-release 1" {
                return Err("header");
            }
            continue;
        }
        let Some(space) = line.find(' ') else { return Err("line") };
        if space == 0 || space + 1 >= line.len() {
            return Err("line");
        }
        let (kw, v) = (&line[..space], &line[space + 1..]);
        match kw {
            "seq" => {
                if seq || !digits(v, 19) || v.starts_with('0') {
                    return Err("seq");
                }
                r.seq = v.parse().map_err(|_| "seq")?;
                seq = true;
            }
            "version" => {
                if version || !valid_version(v) {
                    return Err("version");
                }
                r.version = v.into();
                version = true;
            }
            "expires" => {
                if expires || !digits(v, 12) {
                    return Err("expires");
                }
                r.expires = v.parse().map_err(|_| "expires")?;
                expires = true;
            }
            "min_stub" => {
                if stub || !digits(v, 2) {
                    return Err("min_stub");
                }
                r.min_stub = v.parse().map_err(|_| "min_stub")?;
                stub = true;
            }
            "rollout" => {
                if rollout || !digits(v, 3) || v.parse::<i32>().map_or(true, |x| x > 100) {
                    return Err("rollout");
                }
                r.rollout = v.parse().map_err(|_| "rollout")?;
                rollout = true;
            }
            "salt" => {
                if salt || !hex(v, 8, 64) {
                    return Err("salt");
                }
                r.salt = v.into();
                salt = true;
            }
            "file" => {
                let f: Vec<&str> = v.split(' ').collect();
                if f.len() != 4 || (f[0] != "win64" && f[0] != "win32") || !digits(f[1], 9) || !valid_sha256(f[2]) || !site_path(f[3]) {
                    return Err("file");
                }
                if r.files.iter().any(|e| e.arch == f[0]) {
                    return Err("file twice");
                }
                let size: u64 = f[1].parse().map_err(|_| "file")?;
                if size == 0 || size > MAX_DLL {
                    return Err("file size");
                }
                r.files.push(File { arch: f[0].into(), size, sha256: f[2].into(), path: f[3].into() });
            }
            "notes" => {
                if notes || !site_path(v) {
                    return Err("notes");
                }
                r.notes = v.into();
                notes = true;
            }
            _ => {
                if !kw.bytes().all(|b| b.is_ascii_lowercase() || b == b'_') {
                    return Err("keyword");
                }
            }
        }
    }
    if !seq || !version || !expires || !stub || !rollout || !salt || r.files.is_empty() {
        return Err("missing a field");
    }
    Ok(r)
}

/// What `updater::Facts` holds.
#[derive(Debug, Clone, Default)]
pub struct Facts {
    /// The version that would run (None: not known from outside; never take then).
    pub running: Option<String>,
    pub stub: i32,
    pub last_seq: u64,
    pub now: u64,
    /// This install's place in a rollout (`rollout_slot`).
    pub slot: i32,
    pub bad: Vec<String>,
    pub policy_allows: bool,
    /// The architecture staged for: "win64" or "win32".
    pub arch: String,
    /// This architecture's copy of the document's version is missing or doesn't
    /// match its signed hash (the 1.7.0 launcher emptied a whole `bin\<version>`):
    /// the same document (seq == last_seq) may be taken again, never an older one.
    pub staged_copy_gone: bool,
}

/// `pack_versions::Compare`.
pub fn compare(a: &str, b: &str) -> std::cmp::Ordering {
    crate::policy::compare_versions(a, b)
}

/// The rollout slot from SHA-256(token || salt): first 4 bytes, big-endian, mod 100.
pub fn rollout_slot(digest: &[u8; 32]) -> i32 {
    (u32::from_be_bytes([digest[0], digest[1], digest[2], digest[3]]) % 100) as i32
}

pub fn own_file<'a>(r: &'a Release, arch: &str) -> Option<&'a File> {
    r.files.iter().find(|f| f.arch == arch)
}

/// `updater::ShouldTake`, in the same order and with the same words.
pub fn should_take(r: &Release, f: &Facts) -> Result<String, String> {
    if r.seq < f.last_seq || (r.seq == f.last_seq && !f.staged_copy_gone) {
        return Err("the release document isn't newer than the last one taken".into());
    }
    if f.now >= r.expires {
        return Err("the release document has expired".into());
    }
    let Some(running) = &f.running else {
        return Err("the installed pack's version isn't known here: the pack updates itself in the game".into());
    };
    if compare(&r.version, running).is_le() {
        return Err(format!("up to date ({running})"));
    }
    if f.bad.contains(&r.version) {
        return Err(format!("{} failed to start here before", r.version));
    }
    if !f.policy_allows {
        return Err(format!("{} isn't allowed by Meowy Network's policy", r.version));
    }
    if f.stub < 1 || f.stub < r.min_stub {
        return Err(format!("{} needs a newer start-up stub: reinstall the pack", r.version));
    }
    if f.slot >= r.rollout {
        return Err(format!("{} is rolling out ({}%); not here yet", r.version, r.rollout));
    }
    if own_file(r, &f.arch).is_none() {
        return Err(format!("{} has no file for this game build", r.version));
    }
    Ok(format!("update to {}", r.version))
}

/// A pointer file (`pack_versions::Pointer`): "version x\nsha256 y\nok n\n".
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Pointer {
    pub version: String,
    pub sha256: String,
    pub ok: i32,
}

impl Pointer {
    /// `pack_versions::ReadPointer`'s rules: version and hash required, ok optional.
    pub fn parse(text: &str) -> Option<Pointer> {
        let mut p = Pointer::default();
        let (mut v, mut h) = (false, false);
        for line in text.split('\n').map(|l| l.trim_end_matches('\r')) {
            if let Some(x) = line.strip_prefix("version ").filter(|x| valid_version(x)) {
                p.version = x.into();
                v = true;
            } else if let Some(x) = line.strip_prefix("sha256 ").filter(|x| valid_sha256(x)) {
                p.sha256 = x.into();
                h = true;
            } else if let Some(x) = line.strip_prefix("ok ").filter(|x| digits(x, 3)) {
                p.ok = x.parse().unwrap_or(0);
            }
        }
        (v && h).then_some(p)
    }

    pub fn text(&self) -> String {
        format!("version {}\nsha256 {}\nok {}\n", self.version, self.sha256, self.ok)
    }
}

/// `pack_versions::Allowed` / `policy::VersionAllowed` over a kept policy (None: all allowed).
pub fn policy_allows(p: Option<&crate::policy::Policy>, version: &str) -> bool {
    let Some(p) = p else { return true };
    p.pack_enabled
        && (p.min_version.is_empty() || compare(version, &p.min_version).is_ge())
        && !p.revoked.iter().any(|r| compare(version, r).is_eq())
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    pub const SHA: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

    pub fn doc(extra: &str) -> String {
        format!(
            "meowy-pack-release 1\nseq 7\nversion 1.7.0\nexpires 1900000000\nmin_stub 1\nrollout 100\nsalt 0123abcd\nfile win64 1234 {SHA} /pack/v1/1.7.0/gmcl_meowy_win64.dll\n{extra}"
        )
    }

    #[test]
    fn parses_like_the_pack() {
        let r = parse(doc("notes /pack/v1/1.7.0/notes.txt\nfuture_thing yes\n").as_bytes()).unwrap();
        assert_eq!((r.seq, r.version.as_str(), r.expires, r.min_stub, r.rollout), (7, "1.7.0", 1900000000, 1, 100));
        assert_eq!(r.files, vec![File { arch: "win64".into(), size: 1234, sha256: SHA.into(), path: "/pack/v1/1.7.0/gmcl_meowy_win64.dll".into() }]);
        assert_eq!(r.notes, "/pack/v1/1.7.0/notes.txt");
        let file = format!("file win64 1234 {SHA} /pack/v1/x.dll\n");
        for (bad, why) in [
            ("meowy-pack-release 2\n".to_string(), "header"),
            (doc("seq 8\n"), "seq"),
            (doc("version 1.8\n"), "version"),
            (doc(&file), "file twice"),
            (doc(&format!("file win32 0 {SHA} /pack/v1/x.dll\n")), "file size"),
            (doc(&format!("file win32 99999999 {SHA} /pack/v1/x.dll\n")), "file size"),
            (doc(&format!("file win32 1 {SHA} /pack/v1/../x.dll\n")), "file"),
            (doc(&format!("file win32 1 {SHA} /pack/v2/x.dll\n")), "file"),
            (doc(&format!("file win32 1 {SHA} https://evil/x.dll\n")), "file"),
            (doc(&format!("file arm64 1 {SHA} /pack/v1/x.dll\n")), "file"),
            (doc(&format!("file win32 1 {} /pack/v1/x.dll\n", SHA.to_uppercase())), "file"),
            (doc("notes /pack/v1/\n"), "notes"),
            (doc("Bad-Key 1\n"), "keyword"),
            (doc("x\r\n"), "line ends"),
        ] {
            assert_eq!(parse(bad.as_bytes()), Err(why), "{bad:?}");
        }
        assert_eq!(parse(b"meowy-pack-release 1\nseq 1\n"), Err("missing a field"));
        assert_eq!(parse(doc("").trim_end().as_bytes()), Err("line ends"));
        assert_eq!(parse(doc("rollout 101\n").replace("rollout 100\n", "").as_bytes()), Err("rollout"));
    }

    fn facts() -> Facts {
        Facts {
            running: Some("1.6.0".into()),
            stub: 1,
            last_seq: 6,
            now: 1800000000,
            slot: 42,
            bad: vec![],
            policy_allows: true,
            arch: "win64".into(),
            staged_copy_gone: false,
        }
    }

    #[test]
    fn should_take_in_the_packs_order() {
        let r = parse(doc("").as_bytes()).unwrap();
        assert_eq!(should_take(&r, &facts()), Ok("update to 1.7.0".into()));
        let no = |f: Facts| should_take(&r, &f).unwrap_err();
        assert!(no(Facts { last_seq: 7, ..facts() }).contains("isn't newer"));
        assert_eq!(should_take(&r, &Facts { last_seq: 7, staged_copy_gone: true, ..facts() }), Ok("update to 1.7.0".into()), "the same seq again, its copy gone");
        assert!(no(Facts { last_seq: 8, staged_copy_gone: true, ..facts() }).contains("isn't newer"), "never an older seq");
        assert!(no(Facts { now: 1900000000, ..facts() }).contains("expired"));
        assert!(no(Facts { running: None, ..facts() }).contains("isn't known here"));
        assert_eq!(no(Facts { running: Some("1.7.0".into()), ..facts() }), "up to date (1.7.0)");
        assert_eq!(no(Facts { running: Some("1.10".into()), ..facts() }), "up to date (1.10)", "never a downgrade");
        assert!(no(Facts { bad: vec!["1.7.0".into()], ..facts() }).contains("failed to start here before"));
        assert!(no(Facts { policy_allows: false, ..facts() }).contains("policy"));
        assert!(no(Facts { stub: 0, ..facts() }).contains("newer start-up stub"));
        let mut r2 = r.clone();
        r2.min_stub = 2;
        assert!(should_take(&r2, &facts()).unwrap_err().contains("newer start-up stub"));
        let mut r3 = r.clone();
        r3.rollout = 42;
        assert!(should_take(&r3, &facts()).unwrap_err().contains("rolling out (42%)"), "slot 42 isn't under 42%");
        r3.rollout = 43;
        assert!(should_take(&r3, &facts()).is_ok());
        assert!(no(Facts { arch: "win32".into(), ..facts() }).contains("no file for this game build"));
    }

    #[test]
    fn pointers_and_slots() {
        let p = Pointer { version: "1.7.0".into(), sha256: SHA.into(), ok: 0 };
        assert_eq!(p.text(), format!("version 1.7.0\nsha256 {SHA}\nok 0\n"));
        assert_eq!(Pointer::parse(&p.text()), Some(p));
        assert_eq!(Pointer::parse("version 1.7.0\n"), None, "a hash is required");
        assert_eq!(Pointer::parse(&format!("version x\nsha256 {SHA}\n")), None);
        let mut d = [0u8; 32];
        d[..4].copy_from_slice(&1234567u32.to_be_bytes());
        assert_eq!(rollout_slot(&d), 67);
        let pol = crate::policy::parse(b"meowy-pack-policy 1\nseq 1\npack_enabled 1\nmin_version 1.6.5\nrevoked 1.7.0\n").unwrap();
        assert!(!policy_allows(Some(&pol), "1.6.0") && !policy_allows(Some(&pol), "1.7.0") && policy_allows(Some(&pol), "1.7.1"));
        assert!(policy_allows(None, "1.0"));
    }
}
