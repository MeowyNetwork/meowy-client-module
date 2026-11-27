//! The launcher's end of the pipe to the pack running in the game
//! (`src/launcher_pipe.h` is the other end; DESIGN §10).
//!
//! `\\.\pipe\meowy-launcher-<the user's SID>`: created first by this process
//! (FILE_FLAG_FIRST_PIPE_INSTANCE, so nothing else can sit on the name), open
//! to the current user only (an explicit DACL) and to local clients only. A
//! client is kept only if it's `gmod.exe`/`hl2.exe` in the game's folder,
//! checked with PROCESS_QUERY_LIMITED_INFORMATION (as the watcher does).
//! The one instance is made once and serves client after client
//! (DisconnectNamedPipe, then ConnectNamedPipe again), so the name is never
//! let go between two games; a client that isn't the game is dropped and the
//! next one waits 250 ms.
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

    use windows_sys::Win32::Foundation::{
        CloseHandle, GetLastError, LocalFree, ERROR_IO_PENDING, ERROR_NO_DATA, ERROR_PIPE_CONNECTED, HANDLE, INVALID_HANDLE_VALUE,
    };
    use windows_sys::Win32::Security::Authorization::{
        ConvertSidToStringSidW, ConvertStringSecurityDescriptorToSecurityDescriptorW, SDDL_REVISION_1,
    };
    use windows_sys::Win32::Security::{GetTokenInformation, TokenUser, PSECURITY_DESCRIPTOR, SECURITY_ATTRIBUTES, TOKEN_QUERY, TOKEN_USER};
    use windows_sys::Win32::Storage::FileSystem::{
        ReadFile, WriteFile, FILE_FLAG_FIRST_PIPE_INSTANCE, FILE_FLAG_OVERLAPPED, PIPE_ACCESS_DUPLEX,
    };
    use windows_sys::Win32::System::Pipes::{
        ConnectNamedPipe, CreateNamedPipeW, DisconnectNamedPipe, GetNamedPipeClientProcessId, PeekNamedPipe, PIPE_READMODE_BYTE,
        PIPE_REJECT_REMOTE_CLIENTS, PIPE_TYPE_BYTE, PIPE_WAIT,
    };
    use windows_sys::Win32::System::Threading::{GetCurrentProcess, OpenProcess, OpenProcessToken, QueryFullProcessImageNameW, INFINITE};
    use windows_sys::Win32::System::IO::{CancelIoEx, GetOverlappedResult, OVERLAPPED};

    use super::{parse_line, take_lines, Msg};
    use crate::win::Signal;

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

    /// The pipe's one instance, with its security and the event its
    /// overlapped calls signal. The name stays ours for as long as this lives:
    /// clients come and go on the same instance.
    pub struct Pipe {
        handle: HANDLE,
        event: Signal,
        sd: PSECURITY_DESCRIPTOR,
    }

    // SAFETY: the handles and the descriptor belong to this value only.
    unsafe impl Send for Pipe {}

    impl Drop for Pipe {
        fn drop(&mut self) {
            // SAFETY: ours; no I/O is left pending (every call waits for its own).
            unsafe {
                if self.handle != INVALID_HANDLE_VALUE {
                    CloseHandle(self.handle);
                }
                if !self.sd.is_null() {
                    LocalFree(self.sd);
                }
            }
        }
    }

    /// Makes the pipe (the first and only instance of its name). Err when
    /// there's no SID, or another process holds the name.
    pub fn claim() -> Result<Pipe, String> {
        let sid = user_sid().ok_or("no user SID")?;
        let name = wide(&pipe_name(&sid));
        // Full access for this user only; no one else, not even Everyone-read.
        let sddl = wide(&format!("D:P(A;;GA;;;{sid})"));
        let mut pipe = Pipe { handle: INVALID_HANDLE_VALUE, event: Signal::manual(), sd: null_mut() };
        if pipe.event.handle().is_null() {
            return Err("couldn't make the pipe's event".into());
        }
        // SAFETY: a valid SDDL string; sd is freed in Drop.
        if unsafe { ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.as_ptr(), SDDL_REVISION_1, &mut pipe.sd, null_mut()) } == 0 {
            return Err("couldn't build the pipe's security".into());
        }
        let sa =
            SECURITY_ATTRIBUTES { nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32, lpSecurityDescriptor: pipe.sd, bInheritHandle: 0 };
        // SAFETY: valid name and security; the handle is closed in Drop.
        pipe.handle = unsafe {
            CreateNamedPipeW(
                name.as_ptr(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                1,
                4096,
                4096,
                0,
                &sa,
            )
        };
        if pipe.handle == INVALID_HANDLE_VALUE {
            return Err(format!("couldn't make the pipe (error {})", unsafe { GetLastError() }));
        }
        Ok(pipe)
    }

    enum Io {
        Done(u32),
        Failed(u32),
        Stopped,
    }

    /// What a call that takes an OVERLAPPED said: 0 when it finished at once,
    /// else Windows' error (ERROR_IO_PENDING: it goes on).
    fn started(ok: i32) -> u32 {
        if ok != 0 {
            0
        } else {
            // SAFETY: read right after the call.
            unsafe { GetLastError() }
        }
    }

    impl Pipe {
        /// A fresh OVERLAPPED signalled through our event (reset first).
        fn overlapped(&self) -> Box<OVERLAPPED> {
            self.event.reset();
            // SAFETY: a zeroed OVERLAPPED is the documented start.
            let mut ov: Box<OVERLAPPED> = Box::new(unsafe { std::mem::zeroed() });
            ov.hEvent = self.event.handle();
            ov
        }

        /// Finishes an overlapped call made with `ov` (`err`: what `started`
        /// said): waits for it, or for `stop`. On a stop the call is cancelled
        /// and waited for, so `ov` and the call's buffer are free again.
        fn finish(&self, ov: &OVERLAPPED, err: u32, stop: &Signal) -> Io {
            let mut n = 0u32;
            // SAFETY: `ov` and the call's buffer are the caller's, alive until this
            // returns; nothing is left pending when it does.
            unsafe {
                if err != 0 {
                    if err != ERROR_IO_PENDING {
                        return Io::Failed(err);
                    }
                    if crate::win::wait_any(&[self.event.handle(), stop.handle()], INFINITE) != Some(0) || stop.is_set() {
                        CancelIoEx(self.handle, ov);
                        GetOverlappedResult(self.handle, ov, &mut n, 1);
                        return Io::Stopped;
                    }
                }
                if GetOverlappedResult(self.handle, ov, &mut n, 1) == 0 {
                    return Io::Failed(GetLastError());
                }
            }
            Io::Done(n)
        }

        /// Waits for a client. Ok(true): one is connected; Ok(false): one came
        /// and went before it was seen (disconnect, then wait again); Err(None):
        /// stopped; Err(Some(e)): the instance failed.
        fn connect(&self, stop: &Signal) -> Result<bool, Option<u32>> {
            let mut ov = self.overlapped();
            // SAFETY: our pipe; `ov` lives until `finish` returns.
            let err = started(unsafe { ConnectNamedPipe(self.handle, &mut *ov) });
            match err {
                ERROR_PIPE_CONNECTED => return Ok(true),
                ERROR_NO_DATA => return Ok(false),
                _ => {}
            }
            match self.finish(&ov, err, stop) {
                Io::Done(_) => Ok(true),
                Io::Failed(ERROR_NO_DATA) => Ok(false),
                Io::Failed(e) => Err(Some(e)),
                Io::Stopped => Err(None),
            }
        }

        fn write(&self, data: &[u8], stop: &Signal) -> bool {
            let mut ov = self.overlapped();
            // SAFETY: our pipe; `data` and `ov` live until `finish` returns.
            let err = started(unsafe { WriteFile(self.handle, data.as_ptr(), data.len() as u32, null_mut(), &mut *ov) });
            matches!(self.finish(&ov, err, stop), Io::Done(n) if n as usize == data.len())
        }

        fn read(&self, buf: &mut [u8], stop: &Signal) -> Option<usize> {
            let mut ov = self.overlapped();
            // SAFETY: our pipe; `buf` and `ov` live until `finish` returns.
            let err = started(unsafe { ReadFile(self.handle, buf.as_mut_ptr(), buf.len() as u32, null_mut(), &mut *ov) });
            match self.finish(&ov, err, stop) {
                Io::Done(n) if n > 0 => Some(n as usize),
                _ => None,
            }
        }

        /// Clients, one at a time, until `stop` (Ok) or the instance fails
        /// (Err, with Windows' error code). A client that isn't the game is
        /// dropped, and the next one is only taken 250 ms later.
        fn run(
            &self,
            check: &ClientCheck,
            outgoing: &Receiver<String>,
            on_msg: &mut impl FnMut(Option<Msg>),
            stop: &Signal,
        ) -> Result<(), u32> {
            loop {
                if stop.is_set() {
                    return Ok(());
                }
                match self.connect(stop) {
                    Ok(true) => {}
                    Ok(false) => {
                        // SAFETY: our pipe.
                        unsafe { DisconnectNamedPipe(self.handle) };
                        continue;
                    }
                    Err(None) => return Ok(()),
                    Err(Some(e)) => return Err(e),
                }
                let game = client_image(self.handle).is_some_and(|img| check(&img));
                if game {
                    self.talk(outgoing, on_msg, stop);
                    on_msg(None);
                }
                // SAFETY: our pipe; the same instance takes the next client.
                unsafe { DisconnectNamedPipe(self.handle) };
                if !game && stop.wait(250) {
                    return Ok(());
                }
            }
        }

        /// One client, until it goes away: writes what's queued, reads what's
        /// there, every 100 ms, all on this thread.
        fn talk(&self, outgoing: &Receiver<String>, on_msg: &mut impl FnMut(Option<Msg>), stop: &Signal) {
            let _ = outgoing.try_iter().count(); // anything queued for an earlier client is stale
            let mut buf = Vec::new();
            let mut chunk = vec![0u8; 4096];
            loop {
                for line in outgoing.try_iter() {
                    if !self.write(line.as_bytes(), stop) {
                        return;
                    }
                }
                let mut avail = 0u32;
                // SAFETY: our pipe.
                if unsafe { PeekNamedPipe(self.handle, null_mut(), 0, null_mut(), &mut avail, null_mut()) } == 0 {
                    return;
                }
                if avail > 0 {
                    // Reads at most what's there, so this doesn't wait.
                    let want = (avail as usize).min(chunk.len());
                    let Some(got) = self.read(&mut chunk[..want], stop) else { return };
                    buf.extend_from_slice(&chunk[..got]);
                    let Some(lines) = take_lines(&mut buf) else { return };
                    for line in lines {
                        if let Some(msg) = parse_line(&line) {
                            on_msg(Some(msg));
                        }
                    }
                }
                if stop.wait(100) {
                    return;
                }
            }
        }
    }

    /// Serves the pipe (one client at a time) until `stop` is set:
    /// `on_msg(Some(msg))` for each line from the pack, `on_msg(None)` when it
    /// goes away; lines from `outgoing` are written to it. Never gives up: when
    /// the pipe can't be made (another process holds the name) or fails, it
    /// says so in the log and tries again after 1 s, 5 s, then every 30 s.
    pub fn serve(check: ClientCheck, outgoing: Receiver<String>, mut on_msg: impl FnMut(Option<Msg>), stop: &Signal) {
        const BACKOFF_MS: [u32; 3] = [1000, 5000, 30_000];
        let mut failures = 0usize;
        let mut last_said = String::new();
        while !stop.is_set() {
            let said = match claim() {
                Ok(pipe) => {
                    if failures > 0 {
                        crate::log::line("pipe: made again");
                    }
                    failures = 0;
                    last_said.clear();
                    match pipe.run(&check, &outgoing, &mut on_msg, stop) {
                        Ok(()) => return,
                        Err(e) => format!("pipe: lost the instance (error {e}); making it again"),
                    }
                }
                Err(e) => format!("pipe: {e}; trying again"),
            };
            // One line per kind of failure, not one per retry.
            if said != last_said {
                crate::log::line(&said);
                last_said = said;
            }
            let wait = BACKOFF_MS[failures.min(BACKOFF_MS.len() - 1)];
            failures += 1;
            if stop.wait(wait) {
                return;
            }
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
    /// says hello and a state; the launcher's join comes back to it. Then it
    /// goes away and comes back on the same instance, a client that isn't the
    /// game is dropped, one goes away mid-line, and a stop ends the server.
    #[cfg(windows)]
    #[test]
    fn round_trip_over_the_pipe() {
        use std::io::{BufRead, BufReader, Read, Write};
        use std::sync::atomic::{AtomicBool, Ordering};
        use std::sync::{mpsc, Arc};
        let (tx, rx) = mpsc::channel::<String>();
        let (got_tx, got_rx) = mpsc::channel::<Option<Msg>>();
        let me = std::env::current_exe().unwrap().to_string_lossy().to_lowercase();
        let refuse = Arc::new(AtomicBool::new(false));
        let stop = Arc::new(crate::win::Signal::manual());
        let server = {
            let (refuse, stop) = (refuse.clone(), stop.clone());
            std::thread::spawn(move || {
                let check: ClientCheck = Box::new(move |img| img == me && !refuse.load(Ordering::SeqCst));
                serve(
                    check,
                    rx,
                    move |m| {
                        let _ = got_tx.send(m);
                    },
                    &stop,
                );
            })
        };
        let name = pipe_name(&user_sid().unwrap());
        let open = || {
            for _ in 0..100 {
                if let Ok(f) = std::fs::OpenOptions::new().read(true).write(true).open(&name) {
                    return f;
                }
                std::thread::sleep(std::time::Duration::from_millis(20));
            }
            panic!("couldn't connect to the pipe");
        };
        let t = std::time::Duration::from_secs(5);
        let hello = Some(Msg::Hello { version: "1.6.0".into() });

        let mut client = open();
        client.write_all(b"{\"t\":\"hello\",\"v\":\"1.6.0\"}\n{\"t\":\"state\",\"s\":\"ingame\",\"a\":\"1.2.3.4:27015\"}\n").unwrap();
        assert_eq!(got_rx.recv_timeout(t).unwrap(), hello);
        assert_eq!(
            got_rx.recv_timeout(t).unwrap(),
            Some(Msg::State { state: "ingame".into(), address: Some("1.2.3.4:27015".into()) })
        );
        tx.send(join_line("138.201.62.52:27015").unwrap()).unwrap();
        let mut line = String::new();
        BufReader::new(&client).read_line(&mut line).unwrap();
        assert_eq!(line, "{\"t\":\"join\",\"a\":\"138.201.62.52:27015\"}\n");
        // A second server on the same name is refused (FILE_FLAG_FIRST_PIPE_INSTANCE).
        assert!(claim().is_err(), "nothing else can take the name");
        drop(client);
        assert_eq!(got_rx.recv_timeout(t).unwrap(), None, "the pack went away");

        // The game again (a restart): the same instance takes it.
        let mut client = open();
        client.write_all(b"{\"t\":\"hello\",\"v\":\"1.6.0\"}\n").unwrap();
        assert_eq!(got_rx.recv_timeout(t).unwrap(), hello, "reconnected");
        assert!(claim().is_err(), "the name was never let go");
        // Gone in the middle of a line: only the disconnect is seen.
        client.write_all(b"{\"t\":\"state\",").unwrap();
        drop(client);
        assert_eq!(got_rx.recv_timeout(t).unwrap(), None, "disconnected while connected");

        // Not the game: dropped without a word, and the pipe stays up.
        refuse.store(true, Ordering::SeqCst);
        let mut stranger = open();
        let _ = stranger.write_all(b"{\"t\":\"hello\",\"v\":\"6.6.6\"}\n");
        let mut rest = Vec::new();
        let _ = stranger.read_to_end(&mut rest); // ends when the server disconnects it
        drop(stranger);
        refuse.store(false, Ordering::SeqCst);
        let mut client = open();
        client.write_all(b"{\"t\":\"hello\",\"v\":\"1.6.0\"}\n").unwrap();
        assert_eq!(got_rx.recv_timeout(t).unwrap(), hello, "nothing from the stranger, the game after it");

        // A stop while connected ends the server, which says the pack went away.
        stop.set();
        assert_eq!(got_rx.recv_timeout(t).unwrap(), None);
        server.join().unwrap();
        drop(client);
        // A stop while waiting for a client ends it too, and the name is free again.
        let stop2 = Arc::new(crate::win::Signal::manual());
        let s2 = stop2.clone();
        let (_tx2, rx2) = mpsc::channel::<String>();
        let waiting = std::thread::spawn(move || serve(Box::new(|_| true), rx2, |_| {}, &s2));
        std::thread::sleep(std::time::Duration::from_millis(200));
        stop2.set();
        waiting.join().unwrap();
        assert!(claim().is_ok(), "the name is free once the server is gone");
    }
}
