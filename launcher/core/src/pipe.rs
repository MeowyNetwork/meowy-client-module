//! The launcher's end of the pipe to the pack running in the game
//! (`src/launcher_pipe.h` is the other end; DESIGN §10).
//!
//! `\\.\pipe\meowy-launcher-<the user's SID>`: created first by this process
//! (FILE_FLAG_FIRST_PIPE_INSTANCE, so nothing else can sit on the name), open
//! to the current user only (an explicit DACL) and to local clients only. A
//! client is kept only if it's `gmod.exe`/`hl2.exe` in the game's folder,
//! checked with PROCESS_QUERY_LIMITED_INFORMATION (as the watcher does).
//!
//! Lines of JSON, LF-ended, at most 512 bytes:
//!   pack -> launcher: `{"t":"hello","v":"1.6.0"}`,
//!                     `{"t":"state","s":"menu|joining|ingame"[,"a":"a.b.c.d:port"]}`
//!   launcher -> pack: `{"t":"join","a":"a.b.c.d:port"}` (public addresses only)

use serde::Serialize;

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(tag = "t", rename_all = "snake_case")]
pub enum Msg {
    Hello { version: String },
    State { state: String, address: Option<String> },
}

const MAX_LINE: usize = 512;

/// One line from the pack, strictly: unknown types, extra fields or bad
/// values are None.
pub fn parse_line(line: &str) -> Option<Msg> {
    if line.len() > MAX_LINE {
        return None;
    }
    let v: serde_json::Value = serde_json::from_str(line).ok()?;
    let o = v.as_object()?;
    let only = |keys: &[&str]| o.keys().all(|k| keys.contains(&k.as_str()));
    match o.get("t")?.as_str()? {
        "hello" if only(&["t", "v"]) => {
            let v = o.get("v")?.as_str()?;
            (v.len() <= 16 && v.bytes().all(|b| b.is_ascii_digit() || b == b'.')).then(|| Msg::Hello { version: v.into() })
        }
        "state" if only(&["t", "s", "a"]) => {
            let s = o.get("s")?.as_str()?;
            if !["menu", "joining", "ingame"].contains(&s) {
                return None;
            }
            let address = match o.get("a") {
                None => None,
                Some(a) => Some(crate::address::public(a.as_str()?)?),
            };
            Some(Msg::State { state: s.into(), address })
        }
        _ => None,
    }
}

/// The join line for `address`, or None if it isn't a public server address.
pub fn join_line(address: &str) -> Option<String> {
    crate::address::public(address).map(|a| format!("{{\"t\":\"join\",\"a\":\"{a}\"}}\n"))
}

/// Splits complete lines off `buf` (keeps a partial one); None if a line
/// grows past the limit (not our protocol).
pub fn take_lines(buf: &mut Vec<u8>) -> Option<Vec<String>> {
    let mut out = Vec::new();
    while let Some(nl) = buf.iter().position(|&b| b == b'\n') {
        let line: Vec<u8> = buf.drain(..=nl).collect();
        out.push(String::from_utf8_lossy(&line[..line.len() - 1]).into_owned());
    }
    (buf.len() <= MAX_LINE).then_some(out)
}

/// A folder as process image paths can show it, lowercase and ending in `\`:
/// as configured, and with junctions and links resolved (Windows reports a
/// process's image by its final path, so a game folder moved to another drive
/// behind a junction only matches that form). The second is left out when it's
/// the same or can't be resolved.
pub fn folder_forms(root: &std::path::Path) -> Vec<String> {
    fn form(s: &str) -> String {
        let mut s = s.to_lowercase();
        if let Some(unc) = s.strip_prefix("\\\\?\\unc\\") {
            s = format!("\\\\{unc}");
        } else if let Some(rest) = s.strip_prefix("\\\\?\\") {
            s = rest.to_string();
        }
        if !s.ends_with('\\') {
            s.push('\\');
        }
        s
    }
    let mut out = vec![form(&root.to_string_lossy())];
    if let Ok(real) = std::fs::canonicalize(root) {
        let real = form(&real.to_string_lossy());
        if !out.contains(&real) {
            out.push(real);
        }
    }
    out
}

#[cfg(windows)]
pub use server::*;

#[cfg(windows)]
mod server {
    use std::ffi::OsStr;
    use std::os::windows::ffi::OsStrExt;
    use std::path::Path;
    use std::ptr::null_mut;
    use std::sync::mpsc::Receiver;
    use std::time::Duration;

    use windows_sys::Win32::Foundation::{CloseHandle, LocalFree, ERROR_PIPE_CONNECTED, GetLastError, HANDLE, INVALID_HANDLE_VALUE};
    use windows_sys::Win32::Security::Authorization::{
        ConvertSidToStringSidW, ConvertStringSecurityDescriptorToSecurityDescriptorW, SDDL_REVISION_1,
    };
    use windows_sys::Win32::Security::{GetTokenInformation, TokenUser, PSECURITY_DESCRIPTOR, SECURITY_ATTRIBUTES, TOKEN_QUERY, TOKEN_USER};
    use windows_sys::Win32::Storage::FileSystem::{ReadFile, WriteFile, FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_ACCESS_DUPLEX};
    use windows_sys::Win32::System::Pipes::{
        ConnectNamedPipe, CreateNamedPipeW, DisconnectNamedPipe, GetNamedPipeClientProcessId, PeekNamedPipe, PIPE_READMODE_BYTE,
        PIPE_REJECT_REMOTE_CLIENTS, PIPE_TYPE_BYTE, PIPE_WAIT,
    };
    use windows_sys::Win32::System::Threading::{GetCurrentProcess, OpenProcess, OpenProcessToken, QueryFullProcessImageNameW};

    use super::{parse_line, take_lines, Msg};

    fn wide(s: &str) -> Vec<u16> {
        OsStr::new(s).encode_wide().chain(Some(0)).collect()
    }

    /// The current user's SID as a string ("S-1-5-21-...").
    pub fn user_sid() -> Option<String> {
        // SAFETY: token and buffers we own; the string is freed with LocalFree.
        unsafe {
            let mut token: HANDLE = null_mut();
            if OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) == 0 {
                return None;
            }
            // u64s: TOKEN_USER holds pointers, so the buffer must be 8-byte aligned.
            let mut buf = [0u64; 64];
            let mut len = 0u32;
            let ok = GetTokenInformation(token, TokenUser, buf.as_mut_ptr().cast(), std::mem::size_of_val(&buf) as u32, &mut len);
            CloseHandle(token);
            if ok == 0 {
                return None;
            }
            let user = &*(buf.as_ptr() as *const TOKEN_USER);
            let mut s: *mut u16 = null_mut();
            if ConvertSidToStringSidW(user.User.Sid, &mut s) == 0 {
                return None;
            }
            let mut n = 0;
            while *s.add(n) != 0 {
                n += 1;
            }
            let out = String::from_utf16_lossy(std::slice::from_raw_parts(s, n));
            LocalFree(s.cast());
            Some(out)
        }
    }

    pub fn pipe_name(sid: &str) -> String {
        format!("\\\\.\\pipe\\meowy-launcher-{sid}")
    }

    /// Who may connect: given the client's image path (lowercase), yes or no.
    pub type ClientCheck = Box<dyn Fn(&str) -> bool + Send>;

    /// A client is the game when its image is gmod.exe/hl2.exe under `game_root`
    /// (either of its `folder_forms`).
    pub fn is_game_client(image: &str, game_root: &Path) -> bool {
        let image = image.to_lowercase();
        super::folder_forms(game_root).iter().any(|root| image.starts_with(root))
            && (image.ends_with("\\gmod.exe") || image.ends_with("\\hl2.exe"))
    }

    fn client_image(pipe: HANDLE) -> Option<String> {
        // SAFETY: query-only rights on the client's process; buffers sized.
        unsafe {
            let mut pid = 0u32;
            if GetNamedPipeClientProcessId(pipe, &mut pid) == 0 {
                return None;
            }
            let p = OpenProcess(crate::win::GAME_ACCESS, 0, pid);
            if p.is_null() {
                return None;
            }
            let mut buf = vec![0u16; 1024];
            let mut len = buf.len() as u32;
            let ok = QueryFullProcessImageNameW(p, 0, buf.as_mut_ptr(), &mut len);
            CloseHandle(p);
            (ok != 0).then(|| String::from_utf16_lossy(&buf[..len as usize]).to_lowercase())
        }
    }

    /// Serves the pipe forever (one client at a time): `on_msg(Some(msg))` for
    /// each line from the pack, `on_msg(None)` when it goes away; lines from
    /// `outgoing` are written to it. Returns Err if the pipe can't be made
    /// (another process holds the name, or no SID).
    pub fn serve(check: ClientCheck, outgoing: Receiver<String>, mut on_msg: impl FnMut(Option<Msg>)) -> Result<(), String> {
        let sid = user_sid().ok_or("no user SID")?;
        let name = wide(&pipe_name(&sid));
        // Full access for this user only; no one else, not even Everyone-read.
        let sddl = wide(&format!("D:P(A;;GA;;;{sid})"));
        let mut sd: PSECURITY_DESCRIPTOR = null_mut();
        // SAFETY: a valid SDDL string; sd is freed with LocalFree at the end.
        if unsafe { ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.as_ptr(), SDDL_REVISION_1, &mut sd, null_mut()) } == 0 {
            return Err("couldn't build the pipe's security".into());
        }
        let sa = SECURITY_ATTRIBUTES { nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32, lpSecurityDescriptor: sd, bInheritHandle: 0 };
        let result = (|| loop {
            // SAFETY: a fresh instance each time; the previous one was closed.
            let pipe = unsafe {
                CreateNamedPipeW(
                    name.as_ptr(),
                    PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                    1,
                    4096,
                    4096,
                    0,
                    &sa,
                )
            };
            if pipe == INVALID_HANDLE_VALUE {
                return Err(format!("couldn't make the pipe (error {})", unsafe { GetLastError() }));
            }
            // SAFETY: our pipe; blocks until a client connects.
            let connected = unsafe { ConnectNamedPipe(pipe, null_mut()) } != 0 || unsafe { GetLastError() } == ERROR_PIPE_CONNECTED;
            if connected && client_image(pipe).is_some_and(|img| check(&img)) {
                talk(pipe, &outgoing, &mut on_msg);
                on_msg(None);
            }
            // SAFETY: our pipe.
            unsafe {
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
            }
        })();
        // SAFETY: allocated by ConvertStringSecurityDescriptorToSecurityDescriptorW.
        unsafe { LocalFree(sd) };
        result
    }

    /// One client, until it goes away: writes what's queued, reads what's there,
    /// every 100 ms, all on this thread.
    fn talk(pipe: HANDLE, outgoing: &Receiver<String>, on_msg: &mut impl FnMut(Option<Msg>)) {
        let _ = outgoing.try_iter().count(); // anything queued for an earlier client is stale
        let mut buf = Vec::new();
        loop {
            for line in outgoing.try_iter() {
                let mut w = 0u32;
                // SAFETY: our pipe and a live buffer.
                if unsafe { WriteFile(pipe, line.as_ptr(), line.len() as u32, &mut w, null_mut()) } == 0 {
                    return;
                }
            }
            let mut avail = 0u32;
            // SAFETY: our pipe.
            if unsafe { PeekNamedPipe(pipe, null_mut(), 0, null_mut(), &mut avail, null_mut()) } == 0 {
                return;
            }
            if avail > 0 {
                let mut chunk = vec![0u8; avail.min(4096) as usize];
                let mut got = 0u32;
                // SAFETY: our pipe; reads at most what's there.
                if unsafe { ReadFile(pipe, chunk.as_mut_ptr(), chunk.len() as u32, &mut got, null_mut()) } == 0 || got == 0 {
                    return;
                }
                buf.extend_from_slice(&chunk[..got as usize]);
                let Some(lines) = take_lines(&mut buf) else { return };
                for line in lines {
                    if let Some(msg) = parse_line(&line) {
                        on_msg(Some(msg));
                    }
                }
            }
            std::thread::sleep(Duration::from_millis(100));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lines_from_the_pack() {
        assert_eq!(parse_line(r#"{"t":"hello","v":"1.6.0"}"#), Some(Msg::Hello { version: "1.6.0".into() }));
        assert_eq!(parse_line(r#"{"t":"state","s":"menu"}"#), Some(Msg::State { state: "menu".into(), address: None }));
        assert_eq!(
            parse_line(r#"{"t":"state","s":"ingame","a":"138.201.62.52:27015"}"#),
            Some(Msg::State { state: "ingame".into(), address: Some("138.201.62.52:27015".into()) })
        );
        for bad in [
            r#"{"t":"state","s":"dancing"}"#,
            r#"{"t":"state","s":"ingame","a":"10.0.0.1:27015"}"#,
            r#"{"t":"state","s":"ingame","a":5}"#,
            r#"{"t":"state","s":"menu","extra":1}"#,
            r#"{"t":"hello","v":"1.6.0; rm"}"#,
            r#"{"t":"join","a":"1.2.3.4:1"}"#,
            r#"{"t":"exec","c":"quit"}"#,
            "not json",
            "[]",
        ] {
            assert_eq!(parse_line(bad), None, "{bad}");
        }
        assert_eq!(parse_line(&format!("{{\"t\":\"hello\",\"v\":\"{}\"}}", "1".repeat(600))), None);
    }

    #[test]
    fn join_lines() {
        assert_eq!(join_line("138.201.62.52:27015").as_deref(), Some("{\"t\":\"join\",\"a\":\"138.201.62.52:27015\"}\n"));
        assert_eq!(join_line("192.168.0.1:27015"), None);
        assert_eq!(join_line("1.2.3.4:27015\"}\n{\"t\":\"x"), None);
    }

    #[test]
    fn framing() {
        let mut buf = b"{\"a\":1}\n{\"b\":".to_vec();
        assert_eq!(take_lines(&mut buf), Some(vec!["{\"a\":1}".to_string()]));
        assert_eq!(buf, b"{\"b\":");
        buf.extend_from_slice(b"2}\n");
        assert_eq!(take_lines(&mut buf), Some(vec!["{\"b\":2}".to_string()]));
        let mut long = vec![b'x'; 600];
        assert_eq!(take_lines(&mut long), None, "a line past the limit isn't our protocol");
    }

    #[cfg(windows)]
    #[test]
    fn game_clients_only() {
        let root = std::path::Path::new("C:\\Steam\\steamapps\\common\\GarrysMod");
        assert!(is_game_client("c:\\steam\\steamapps\\common\\garrysmod\\bin\\win64\\gmod.exe", root));
        assert!(is_game_client("c:\\steam\\steamapps\\common\\garrysmod\\hl2.exe", root));
        assert!(!is_game_client("c:\\steam\\steamapps\\common\\garrysmodx\\gmod.exe", root));
        assert!(!is_game_client("c:\\users\\a\\gmod.exe", root));
        assert!(!is_game_client("c:\\steam\\steamapps\\common\\garrysmod\\bin\\win64\\other.exe", root));
    }

    /// A game folder behind a junction (moved to another drive): the image path
    /// Windows reports is the real one, and it's still the game.
    #[cfg(windows)]
    #[test]
    fn game_behind_a_junction() {
        let base = std::env::temp_dir().join(format!("meowy_launcher_junction_{}", std::process::id()));
        let real = base.join("real");
        let link = base.join("link");
        std::fs::create_dir_all(real.join("bin\\win64")).unwrap();
        let made = std::process::Command::new("cmd")
            .args(["/c", "mklink", "/J"])
            .arg(&link)
            .arg(&real)
            .output()
            .unwrap();
        assert!(made.status.success(), "mklink /J");
        let forms = folder_forms(&link);
        assert_eq!(forms.len(), 2, "{forms:?}");
        let real_form = format!("{}\\", std::fs::canonicalize(&real).unwrap().to_string_lossy().to_lowercase().trim_start_matches("\\\\?\\"));
        assert_eq!(forms[1], real_form);
        assert!(!forms[1].starts_with("\\\\?\\"), "no \\\\?\\ prefix");
        let image = format!("{real_form}bin\\win64\\gmod.exe");
        assert!(is_game_client(&image, &link), "the real path, the junction configured");
        assert!(is_game_client(&format!("{}bin\\win64\\gmod.exe", forms[0]), &link), "the configured path still");
        // The real folder resolves to the same final form (a temp path with an 8.3
        // short name, as on CI, has its long form as the second one).
        assert_eq!(folder_forms(&real).last(), Some(&real_form), "the real folder: the same final form");
        assert!(is_game_client(&image, &real));
        let _ = std::fs::remove_dir(&link);
        let _ = std::fs::remove_dir_all(&base);
    }

    /// The real pipe, in this process: a client (standing in for the pack)
    /// says hello and a state; the launcher's join comes back to it.
    #[cfg(windows)]
    #[test]
    fn round_trip_over_the_pipe() {
        use std::io::{BufRead, BufReader, Write};
        use std::sync::mpsc;
        let (tx, rx) = mpsc::channel::<String>();
        let (got_tx, got_rx) = mpsc::channel::<Option<Msg>>();
        let me = std::env::current_exe().unwrap().to_string_lossy().to_lowercase();
        std::thread::spawn(move || {
            let check: ClientCheck = Box::new(move |img| img == me);
            let _ = serve(check, rx, move |m| {
                let _ = got_tx.send(m);
            });
        });
        let name = pipe_name(&user_sid().unwrap());
        let mut client = None;
        for _ in 0..50 {
            if let Ok(f) = std::fs::OpenOptions::new().read(true).write(true).open(&name) {
                client = Some(f);
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(20));
        }
        let mut client = client.expect("connected to the pipe");
        client.write_all(b"{\"t\":\"hello\",\"v\":\"1.6.0\"}\n{\"t\":\"state\",\"s\":\"ingame\",\"a\":\"1.2.3.4:27015\"}\n").unwrap();
        let t = std::time::Duration::from_secs(5);
        assert_eq!(got_rx.recv_timeout(t).unwrap(), Some(Msg::Hello { version: "1.6.0".into() }));
        assert_eq!(
            got_rx.recv_timeout(t).unwrap(),
            Some(Msg::State { state: "ingame".into(), address: Some("1.2.3.4:27015".into()) })
        );
        tx.send(join_line("138.201.62.52:27015").unwrap()).unwrap();
        let mut line = String::new();
        BufReader::new(&client).read_line(&mut line).unwrap();
        assert_eq!(line, "{\"t\":\"join\",\"a\":\"138.201.62.52:27015\"}\n");
        // A second server on the same name is refused (FILE_FLAG_FIRST_PIPE_INSTANCE).
        let (_tx2, rx2) = mpsc::channel::<String>();
        assert!(serve(Box::new(|_| true), rx2, |_| {}).is_err(), "nothing else can take the name");
        drop(client);
        assert_eq!(got_rx.recv_timeout(t).unwrap(), None, "the pack went away");
    }
}
