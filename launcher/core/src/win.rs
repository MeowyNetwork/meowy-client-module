//! The Windows calls: the current user's registry, opening a URL or folder,
//! and watching the game's processes.
//!
//! The watcher opens game processes with `PROCESS_SYNCHRONIZE |
//! PROCESS_QUERY_LIMITED_INFORMATION` and nothing else: enough to wait for
//! the exit, read the exit code, the image path and the start time. Never
//! memory, threads or handles inside the game.

use std::ffi::OsString;
use std::os::windows::ffi::{OsStrExt, OsStringExt};
use std::path::{Path, PathBuf};
use std::ptr::{null, null_mut};

use windows_sys::Win32::Foundation::{CloseHandle, FILETIME, HANDLE, INVALID_HANDLE_VALUE, WAIT_OBJECT_0, WAIT_TIMEOUT};
use windows_sys::Win32::System::Diagnostics::ToolHelp::{
    CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W, TH32CS_SNAPPROCESS,
};
use windows_sys::Win32::System::Registry::{
    RegDeleteKeyValueW, RegDeleteTreeW, RegGetValueW, RegSetKeyValueW, HKEY_CURRENT_USER, REG_SZ, RRF_RT_REG_DWORD,
    RRF_RT_REG_SZ,
};
use windows_sys::Win32::System::Threading::{
    GetExitCodeProcess, GetProcessTimes, OpenProcess, QueryFullProcessImageNameW, WaitForMultipleObjects,
    PROCESS_NAME_WIN32, PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_SYNCHRONIZE,
};
use windows_sys::Win32::UI::Shell::ShellExecuteW;
use windows_sys::Win32::UI::WindowsAndMessaging::SW_SHOWNORMAL;

/// The only rights the launcher ever asks a game process for.
pub const GAME_ACCESS: u32 = PROCESS_SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION;

fn wide(s: &str) -> Vec<u16> {
    std::ffi::OsStr::new(s).encode_wide().chain(Some(0)).collect()
}

// --- Registry (HKEY_CURRENT_USER only: no admin) -----------------------------

pub fn reg_get(subkey: &str, value: &str) -> Option<String> {
    let (k, v) = (wide(subkey), wide(value));
    let mut size: u32 = 0;
    // SAFETY: plain Win32 calls with valid NUL-terminated strings and sizes.
    unsafe {
        if RegGetValueW(HKEY_CURRENT_USER, k.as_ptr(), v.as_ptr(), RRF_RT_REG_SZ, null_mut(), null_mut(), &mut size) != 0 {
            return None;
        }
        let mut buf = vec![0u16; (size as usize).div_ceil(2) + 1];
        let mut bytes = (buf.len() * 2) as u32;
        if RegGetValueW(HKEY_CURRENT_USER, k.as_ptr(), v.as_ptr(), RRF_RT_REG_SZ, null_mut(), buf.as_mut_ptr().cast(), &mut bytes) != 0 {
            return None;
        }
        let len = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
        Some(String::from_utf16_lossy(&buf[..len]))
    }
}

pub fn reg_get_dword(subkey: &str, value: &str) -> Option<u32> {
    let (k, v) = (wide(subkey), wide(value));
    let mut data: u32 = 0;
    let mut size: u32 = 4;
    // SAFETY: a 4-byte buffer for a REG_DWORD.
    let r = unsafe {
        RegGetValueW(HKEY_CURRENT_USER, k.as_ptr(), v.as_ptr(), RRF_RT_REG_DWORD, null_mut(), (&mut data as *mut u32).cast(), &mut size)
    };
    (r == 0).then_some(data)
}

/// The Steam account signed in now (its 32-bit account id), if Steam says.
pub fn steam_active_account() -> Option<u32> {
    reg_get_dword("Software\\Valve\\Steam\\ActiveProcess", "ActiveUser").filter(|&id| id != 0)
}

pub fn reg_set(subkey: &str, value: &str, data: &str) -> bool {
    let (k, v, d) = (wide(subkey), wide(value), wide(data));
    // SAFETY: as above; RegSetKeyValueW creates the key if needed.
    unsafe { RegSetKeyValueW(HKEY_CURRENT_USER, k.as_ptr(), v.as_ptr(), REG_SZ, d.as_ptr().cast(), (d.len() * 2) as u32) == 0 }
}

pub fn reg_delete_value(subkey: &str, value: &str) -> bool {
    let (k, v) = (wide(subkey), wide(value));
    // SAFETY: as above.
    unsafe { RegDeleteKeyValueW(HKEY_CURRENT_USER, k.as_ptr(), v.as_ptr()) == 0 }
}

pub fn reg_delete_tree(subkey: &str) -> bool {
    let k = wide(subkey);
    // SAFETY: as above.
    unsafe { RegDeleteTreeW(HKEY_CURRENT_USER, k.as_ptr()) == 0 }
}

/// Steam's `steam.exe` and its folder, from Steam's own registry values.
pub fn steam_install() -> Option<(PathBuf, PathBuf)> {
    let fix = |s: String| PathBuf::from(s.replace('/', "\\"));
    let exe = reg_get("Software\\Valve\\Steam", "SteamExe").map(fix)?;
    let path = reg_get("Software\\Valve\\Steam", "SteamPath").map(fix)?;
    exe.is_file().then_some((exe, path))
}

// --- meowy:// and start at sign-in -------------------------------------------

const PROTOCOL_KEY: &str = "Software\\Classes\\meowy";
const RUN_KEY: &str = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
pub const RUN_VALUE: &str = "MeowyLauncher";

pub fn protocol_command() -> Option<String> {
    reg_get(&format!("{PROTOCOL_KEY}\\shell\\open\\command"), "")
}

pub fn protocol_command_for(exe: &Path) -> String {
    format!("\"{}\" \"%1\"", exe.display())
}

pub fn register_protocol(exe: &Path) -> bool {
    reg_set(PROTOCOL_KEY, "", "URL:Meowy Launcher")
        && reg_set(PROTOCOL_KEY, "URL Protocol", "")
        && reg_set(&format!("{PROTOCOL_KEY}\\DefaultIcon"), "", &format!("\"{}\",0", exe.display()))
        && reg_set(&format!("{PROTOCOL_KEY}\\shell\\open\\command"), "", &protocol_command_for(exe))
}

/// The exe a `"<exe>" ...` command starts.
pub fn command_exe(command: &str) -> Option<PathBuf> {
    let rest = command.strip_prefix('"')?;
    rest.split_once('"').map(|(exe, _)| PathBuf::from(exe))
}

pub fn run_command_for(exe: &Path) -> String {
    format!("\"{}\" --tray", exe.display())
}

pub fn start_at_sign_in() -> bool {
    reg_get(RUN_KEY, RUN_VALUE).is_some()
}

pub fn set_start_at_sign_in(exe: &Path, on: bool) -> bool {
    if on {
        reg_set(RUN_KEY, RUN_VALUE, &run_command_for(exe))
    } else {
        reg_delete_value(RUN_KEY, RUN_VALUE) || !start_at_sign_in()
    }
}

/// For tests: remove what `register_protocol` wrote under a test key.
pub fn delete_protocol_tree() -> bool {
    reg_delete_tree(PROTOCOL_KEY)
}

// --- Folder changes ----------------------------------------------------------

/// A watch for a file being created, deleted or renamed directly in a folder
/// (not below it), armed when made: a change after `new` and before `wait`
/// still counts.
pub struct NameWatch(HANDLE);

// SAFETY: a change-notification handle may be waited on from any thread.
unsafe impl Send for NameWatch {}

impl NameWatch {
    pub fn new(dir: &Path) -> Option<NameWatch> {
        use windows_sys::Win32::Storage::FileSystem::{FindFirstChangeNotificationW, FILE_NOTIFY_CHANGE_FILE_NAME};
        let d = wide(&dir.to_string_lossy());
        // SAFETY: a valid path; the handle is closed in Drop.
        let h = unsafe { FindFirstChangeNotificationW(d.as_ptr(), 0, FILE_NOTIFY_CHANGE_FILE_NAME) };
        (h != INVALID_HANDLE_VALUE).then_some(NameWatch(h))
    }

    /// True if a change came (or had come) within `timeout_ms`.
    pub fn wait(&self, timeout_ms: u32) -> bool {
        use windows_sys::Win32::System::Threading::WaitForSingleObject;
        // SAFETY: our own handle.
        unsafe { WaitForSingleObject(self.0, timeout_ms) == WAIT_OBJECT_0 }
    }
}

impl Drop for NameWatch {
    fn drop(&mut self) {
        use windows_sys::Win32::Storage::FileSystem::FindCloseChangeNotification;
        // SAFETY: our own handle.
        unsafe { FindCloseChangeNotification(self.0) };
    }
}

// --- Signatures ---------------------------------------------------------------

/// ECDSA P-256 over SHA-256 of `body`, `sig` = r || s (64 bytes), against the
/// public key X || Y, through Windows' BCrypt (as the pack's VerifyWithKey).
pub fn verify_p256(key_xy: &[u8; 64], body: &[u8], sig: &[u8]) -> bool {
    use windows_sys::Win32::Security::Cryptography::{
        BCryptCloseAlgorithmProvider, BCryptDestroyKey, BCryptHash, BCryptImportKeyPair, BCryptOpenAlgorithmProvider,
        BCryptVerifySignature, BCRYPT_ECCKEY_BLOB, BCRYPT_ECCPUBLIC_BLOB, BCRYPT_ECDSA_P256_ALGORITHM, BCRYPT_ECDSA_PUBLIC_P256_MAGIC,
        BCRYPT_SHA256_ALGORITHM,
    };
    if sig.len() != 64 || body.len() > u32::MAX as usize {
        return false;
    }
    // SAFETY: handles we open and close here; buffers sized as BCrypt asks.
    unsafe {
        let mut sha = null_mut();
        if BCryptOpenAlgorithmProvider(&mut sha, BCRYPT_SHA256_ALGORITHM, null(), 0) != 0 {
            return false;
        }
        let mut digest = [0u8; 32];
        let hashed = BCryptHash(sha, null(), 0, body.as_ptr(), body.len() as u32, digest.as_mut_ptr(), 32) == 0;
        BCryptCloseAlgorithmProvider(sha, 0);
        if !hashed {
            return false;
        }
        let mut ecdsa = null_mut();
        if BCryptOpenAlgorithmProvider(&mut ecdsa, BCRYPT_ECDSA_P256_ALGORITHM, null(), 0) != 0 {
            return false;
        }
        let header = BCRYPT_ECCKEY_BLOB { dwMagic: BCRYPT_ECDSA_PUBLIC_P256_MAGIC, cbKey: 32 };
        let mut blob = Vec::with_capacity(8 + 64);
        blob.extend_from_slice(&header.dwMagic.to_le_bytes());
        blob.extend_from_slice(&header.cbKey.to_le_bytes());
        blob.extend_from_slice(key_xy);
        let mut key = null_mut();
        let ok = BCryptImportKeyPair(ecdsa, null_mut(), BCRYPT_ECCPUBLIC_BLOB, &mut key, blob.as_ptr(), blob.len() as u32, 0) == 0
            && BCryptVerifySignature(key, null(), digest.as_ptr(), 32, sig.as_ptr(), 64, 0) == 0;
        if !key.is_null() {
            BCryptDestroyKey(key);
        }
        BCryptCloseAlgorithmProvider(ecdsa, 0);
        ok
    }
}

/// SHA-256 through BCrypt.
pub fn sha256(data: &[u8]) -> [u8; 32] {
    use windows_sys::Win32::Security::Cryptography::{BCryptCloseAlgorithmProvider, BCryptHash, BCryptOpenAlgorithmProvider, BCRYPT_SHA256_ALGORITHM};
    let mut out = [0u8; 32];
    // SAFETY: a provider we open and close; buffers sized.
    unsafe {
        let mut alg = null_mut();
        if BCryptOpenAlgorithmProvider(&mut alg, BCRYPT_SHA256_ALGORITHM, null(), 0) == 0 {
            BCryptHash(alg, null(), 0, data.as_ptr(), data.len() as u32, out.as_mut_ptr(), 32);
            BCryptCloseAlgorithmProvider(alg, 0);
        }
    }
    out
}

/// `n` bytes from the system's preferred RNG.
pub fn random_bytes(n: usize) -> Option<Vec<u8>> {
    use windows_sys::Win32::Security::Cryptography::{BCryptGenRandom, BCRYPT_USE_SYSTEM_PREFERRED_RNG};
    let mut buf = vec![0u8; n];
    // SAFETY: a buffer of n bytes.
    (unsafe { BCryptGenRandom(null_mut(), buf.as_mut_ptr(), n as u32, BCRYPT_USE_SYSTEM_PREFERRED_RNG) } == 0).then_some(buf)
}

/// A file's own version resource (VS_FIXEDFILEINFO FileVersion) as "a.b.c"
/// (or "a.b.c.d" when the fourth part isn't 0), or None without one.
pub fn file_version(path: &Path) -> Option<String> {
    use windows_sys::Win32::Storage::FileSystem::{GetFileVersionInfoSizeW, GetFileVersionInfoW, VerQueryValueW, VS_FIXEDFILEINFO};
    let p = wide(&path.to_string_lossy());
    // SAFETY: a buffer of the size Windows asks for; the fixed-info pointer
    // points into it and is read only while the buffer lives.
    unsafe {
        let mut handle = 0u32;
        let size = GetFileVersionInfoSizeW(p.as_ptr(), &mut handle);
        if size == 0 || size > 1 << 20 {
            return None;
        }
        let mut buf = vec![0u8; size as usize];
        if GetFileVersionInfoW(p.as_ptr(), 0, size, buf.as_mut_ptr().cast()) == 0 {
            return None;
        }
        let root = wide("\\");
        let mut info: *mut core::ffi::c_void = null_mut();
        let mut len = 0u32;
        if VerQueryValueW(buf.as_ptr().cast(), root.as_ptr(), &mut info, &mut len) == 0
            || info.is_null()
            || (len as usize) < std::mem::size_of::<VS_FIXEDFILEINFO>()
        {
            return None;
        }
        let f = std::ptr::read_unaligned(info as *const VS_FIXEDFILEINFO);
        if f.dwSignature != 0xFEEF04BD {
            return None;
        }
        let (a, b, c, d) = (f.dwFileVersionMS >> 16, f.dwFileVersionMS & 0xFFFF, f.dwFileVersionLS >> 16, f.dwFileVersionLS & 0xFFFF);
        Some(if d == 0 { format!("{a}.{b}.{c}") } else { format!("{a}.{b}.{c}.{d}") })
    }
}

// --- Shell ------------------------------------------------------------------

/// Opens a URL (`steam://...`) or a folder with the shell.
pub fn shell_open(target: &str) -> bool {
    let (op, t) = (wide("open"), wide(target));
    // SAFETY: valid strings; a return above 32 means success.
    unsafe { ShellExecuteW(null_mut(), op.as_ptr(), t.as_ptr(), null(), null(), SW_SHOWNORMAL) as usize > 32 }
}

/// Whether this process was started "as administrator" from a session that
/// normally runs unelevated (TokenElevationTypeFull: the elevated half of a
/// split token): then whatever it starts (Steam, and through it the game)
/// would be elevated too, unlike everything else the player runs. With UAC off,
/// or on the built-in Administrator account, every process has the full token
/// (TokenElevationTypeDefault) and this is false: nothing to step down to.
pub fn elevated() -> bool {
    use windows_sys::Win32::Security::{
        GetTokenInformation, TokenElevationType, TokenElevationTypeFull, TOKEN_ELEVATION_TYPE, TOKEN_QUERY,
    };
    use windows_sys::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};
    // SAFETY: our own token, query only; the out-buffer is the value's size.
    unsafe {
        let mut token: HANDLE = null_mut();
        if OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) == 0 {
            return false;
        }
        let mut kind: TOKEN_ELEVATION_TYPE = 0;
        let mut len = 0u32;
        let ok = GetTokenInformation(
            token,
            TokenElevationType,
            &mut kind as *mut TOKEN_ELEVATION_TYPE as *mut core::ffi::c_void,
            std::mem::size_of::<TOKEN_ELEVATION_TYPE>() as u32,
            &mut len,
        );
        CloseHandle(token);
        ok != 0 && kind == TokenElevationTypeFull
    }
}

// --- Game processes -----------------------------------------------------------

struct Proc {
    handle: HANDLE,
    pid: u32,
    created: i64,
}

impl Drop for Proc {
    fn drop(&mut self) {
        // SAFETY: a handle we opened and still own.
        unsafe { CloseHandle(self.handle) };
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Event {
    Idle,
    /// The game started (or was already running when the launcher did).
    Started { created: i64 },
    /// The last game process exited. `created`: when the session's first one started.
    Ended { exit_code: u32, created: i64 },
}

/// Follows `gmod.exe`/`hl2.exe` processes whose image is under the game's
/// folder. GMod's own Chromium helpers are `gmod.exe` too, with the same
/// image, so any process whose parent is a followed one with the same image
/// isn't followed. Steam's 32-bit root `gmod.exe` starts `bin\win64\gmod.exe`
/// and exits, so a session ends when the last followed process exits; its
/// exit code is the session's.
pub struct Watcher {
    roots: Vec<String>,
    procs: Vec<Proc>,
    skipped: Vec<u32>,
    active: bool,
    created: i64,
    last_exit: u32,
}

fn filetime_unix(ft: &FILETIME) -> i64 {
    let t = ((ft.dwHighDateTime as u64) << 32) | ft.dwLowDateTime as u64;
    (t as i64 - 116_444_736_000_000_000) / 10_000_000
}

fn image_path(handle: HANDLE) -> Option<String> {
    let mut buf = vec![0u16; 1024];
    let mut len = buf.len() as u32;
    // SAFETY: buffer and length match.
    let ok = unsafe { QueryFullProcessImageNameW(handle, PROCESS_NAME_WIN32, buf.as_mut_ptr(), &mut len) };
    (ok != 0).then(|| OsString::from_wide(&buf[..len as usize]).to_string_lossy().to_lowercase())
}

fn created(handle: HANDLE) -> i64 {
    let zero = FILETIME { dwLowDateTime: 0, dwHighDateTime: 0 };
    let (mut c, mut e, mut k, mut u) = (zero, zero, zero, zero);
    // SAFETY: four valid out-pointers.
    if unsafe { GetProcessTimes(handle, &mut c, &mut e, &mut k, &mut u) } != 0 {
        filetime_unix(&c)
    } else {
        0
    }
}

/// (pid, parent pid) of every process named gmod.exe or hl2.exe.
fn candidates() -> Vec<(u32, u32)> {
    let mut out = Vec::new();
    // SAFETY: a snapshot handle we close; the entry's size is set as required.
    unsafe {
        let snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if snap == INVALID_HANDLE_VALUE {
            return out;
        }
        let mut e: PROCESSENTRY32W = std::mem::zeroed();
        e.dwSize = std::mem::size_of::<PROCESSENTRY32W>() as u32;
        let mut more = Process32FirstW(snap, &mut e) != 0;
        while more {
            let len = e.szExeFile.iter().position(|&c| c == 0).unwrap_or(e.szExeFile.len());
            let name = String::from_utf16_lossy(&e.szExeFile[..len]).to_lowercase();
            if name == "gmod.exe" || name == "hl2.exe" {
                out.push((e.th32ProcessID, e.th32ParentProcessID));
            }
            more = Process32NextW(snap, &mut e) != 0;
        }
        CloseHandle(snap);
    }
    out
}

impl Watcher {
    pub fn new(game_root: &Path) -> Watcher {
        // Both forms of the folder: a game behind a junction reports its real path.
        let roots = crate::pipe::folder_forms(game_root);
        Watcher { roots, procs: Vec::new(), skipped: Vec::new(), active: false, created: 0, last_exit: 0 }
    }

    pub fn running(&self) -> bool {
        self.active
    }

    fn scan(&mut self) {
        let mut found: Vec<(Proc, u32, String)> = Vec::new();
        let known: Vec<u32> = self.procs.iter().map(|p| p.pid).chain(self.skipped.iter().copied()).collect();
        for (pid, parent) in candidates() {
            if known.contains(&pid) {
                continue;
            }
            // SAFETY: OpenProcess with wait-and-query rights only.
            let handle = unsafe { OpenProcess(GAME_ACCESS, 0, pid) };
            if handle.is_null() {
                continue;
            }
            let proc = Proc { handle, pid, created: created(handle) };
            match image_path(handle) {
                Some(path) if self.roots.iter().any(|r| path.starts_with(r)) => found.push((proc, parent, path)),
                _ => self.skipped.push(pid),
            }
        }
        // Paths of followed processes, to recognise Chromium helpers.
        let mut paths: Vec<(u32, String)> = Vec::new();
        for p in &self.procs {
            if let Some(path) = image_path(p.handle) {
                paths.push((p.pid, path));
            }
        }
        paths.extend(found.iter().map(|(p, _, path)| (p.pid, path.clone())));
        for (proc, parent, path) in found {
            if paths.iter().any(|(pid, p)| *pid == parent && *p == path) {
                self.skipped.push(proc.pid);
            } else {
                self.procs.push(proc);
            }
        }
    }

    /// Waits up to `timeout_ms` for something to happen.
    pub fn step(&mut self, timeout_ms: u32) -> Event {
        if self.procs.is_empty() {
            self.scan();
            if self.procs.is_empty() {
                std::thread::sleep(std::time::Duration::from_millis(timeout_ms as u64));
                return Event::Idle;
            }
        }
        if !self.active {
            self.active = true;
            self.created = self.procs.iter().map(|p| p.created).filter(|&c| c > 0).min().unwrap_or(0);
            return Event::Started { created: self.created };
        }
        let handles: Vec<HANDLE> = self.procs.iter().take(64).map(|p| p.handle).collect();
        // SAFETY: up to 64 valid handles we own.
        let r = unsafe { WaitForMultipleObjects(handles.len() as u32, handles.as_ptr(), 0, timeout_ms) };
        if r == WAIT_TIMEOUT {
            self.scan();
            return Event::Idle;
        }
        let i = r.wrapping_sub(WAIT_OBJECT_0) as usize;
        if i < handles.len() {
            let mut code = 0u32;
            // SAFETY: a signalled handle we own.
            unsafe { GetExitCodeProcess(self.procs[i].handle, &mut code) };
            self.last_exit = code;
            self.procs.remove(i);
        } else {
            // WAIT_FAILED: shouldn't happen; don't spin.
            std::thread::sleep(std::time::Duration::from_millis(timeout_ms as u64));
        }
        self.scan();
        if self.procs.is_empty() {
            // Steam's root gmod.exe may exit just before its child shows up.
            std::thread::sleep(std::time::Duration::from_secs(3));
            self.scan();
        }
        if self.procs.is_empty() {
            self.active = false;
            self.skipped.clear();
            return Event::Ended { exit_code: self.last_exit, created: self.created };
        }
        Event::Idle
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn commands() {
        let exe = Path::new("C:\\Users\\A B\\AppData\\Local\\MeowyClientPack\\launcher\\MeowyLauncher.exe");
        assert_eq!(protocol_command_for(exe), format!("\"{}\" \"%1\"", exe.display()));
        assert_eq!(command_exe(&protocol_command_for(exe)).as_deref(), Some(exe));
        assert_eq!(run_command_for(exe), format!("\"{}\" --tray", exe.display()));
        assert_eq!(command_exe("no quotes"), None);
    }

    #[test]
    fn name_watch_sees_a_change_made_before_waiting() {
        let dir = std::env::temp_dir().join(format!("meowy_launcher_watch_dir_{}", std::process::id()));
        std::fs::create_dir_all(dir.join("sub")).unwrap();
        let w = NameWatch::new(&dir).unwrap();
        assert!(!w.wait(50), "nothing yet");
        std::fs::write(dir.join("sub").join("deeper.txt"), "x").unwrap();
        assert!(!w.wait(200), "a change below the folder doesn't count");
        std::fs::write(dir.join("boot.lock"), "").unwrap();
        std::fs::remove_file(dir.join("boot.lock")).unwrap();
        assert!(w.wait(0), "a create+delete before waiting still counts");
        drop(w);
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn filetime() {
        // 2026-11-07 00:00:00 UTC
        let t: u64 = (1_793_923_200u64 * 10_000_000) + 116_444_736_000_000_000;
        let ft = FILETIME { dwLowDateTime: t as u32, dwHighDateTime: (t >> 32) as u32 };
        assert_eq!(filetime_unix(&ft), 1_793_923_200);
    }

    /// A real child process stands in for the game: the watcher sees it start
    /// and end, and reads its exit code with wait-and-query rights only.
    #[test]
    fn watches_a_process_by_folder() {
        let dir = std::env::temp_dir().join(format!("meowy_launcher_watch_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        // A copy of cmd.exe named gmod.exe, in a folder of our own.
        let sys = std::env::var("SystemRoot").unwrap_or_else(|_| "C:\\Windows".into());
        let fake = dir.join("gmod.exe");
        std::fs::copy(Path::new(&sys).join("System32\\cmd.exe"), &fake).unwrap();

        let mut w = Watcher::new(&dir);
        assert_eq!(w.step(10), Event::Idle);
        let mut child = std::process::Command::new(&fake).args(["/c", "ping -n 3 127.0.0.1 >nul & exit 7"]).spawn().unwrap();
        let mut started = false;
        let mut ended = None;
        for _ in 0..200 {
            match w.step(100) {
                Event::Started { created } => {
                    assert!(created > 1_700_000_000);
                    started = true;
                }
                Event::Ended { exit_code, .. } => {
                    ended = Some(exit_code);
                    break;
                }
                Event::Idle => {}
            }
        }
        let _ = child.wait();
        assert!(started, "saw it start");
        assert_eq!(ended, Some(7), "saw it end with its exit code");

        // A process with the same name outside the game folder is ignored.
        let mut other = Watcher::new(&dir.join("elsewhere"));
        let mut child = std::process::Command::new(&fake).args(["/c", "ping -n 2 127.0.0.1 >nul"]).spawn().unwrap();
        for _ in 0..10 {
            assert_eq!(other.step(50), Event::Idle);
        }
        let _ = child.wait();
        drop(w);
        drop(other);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
