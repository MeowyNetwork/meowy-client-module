//! The Windows calls: the current user's registry, opening a URL or folder,
//! watching the game's processes, and the events the launcher's threads wait on.
//!
//! The watcher opens game processes with `PROCESS_SYNCHRONIZE |
//! PROCESS_QUERY_LIMITED_INFORMATION` and nothing else: enough to wait for
//! the exit, read the exit code, the image path and the start time. Never
//! memory, threads or handles inside the game.

use std::ffi::OsString;
use std::os::windows::ffi::{OsStrExt, OsStringExt};
use std::path::{Path, PathBuf};
use std::ptr::{null, null_mut};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::OnceLock;
use std::time::{Duration, Instant};

use windows_sys::Win32::Foundation::{CloseHandle, FILETIME, HANDLE, INVALID_HANDLE_VALUE, WAIT_OBJECT_0, WAIT_TIMEOUT};
use windows_sys::Win32::System::Diagnostics::ToolHelp::{
    CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W, TH32CS_SNAPPROCESS,
};
use windows_sys::Win32::System::Registry::{
    RegDeleteKeyValueW, RegDeleteTreeW, RegGetValueW, RegSetKeyValueW, HKEY_CURRENT_USER, REG_SZ, RRF_RT_REG_DWORD,
    RRF_RT_REG_SZ,
};
use windows_sys::Win32::System::Threading::{
    CreateEventW, GetExitCodeProcess, GetProcessTimes, OpenProcess, QueryFullProcessImageNameW, SetEvent, WaitForMultipleObjects,
    WaitForSingleObject, INFINITE, PROCESS_NAME_WIN32, PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_SYNCHRONIZE,
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

// --- Events the launcher's threads wait on -------------------------------------

/// A Win32 event. A manual-reset one also keeps a flag, so `is_set` needs no
/// wait and works even if Windows couldn't make the event (then a wait is a
/// plain sleep).
pub struct Signal {
    handle: HANDLE,
    manual: bool,
    flag: AtomicBool,
}

// SAFETY: an event handle may be set and waited on from any thread.
unsafe impl Send for Signal {}
unsafe impl Sync for Signal {}

fn sleep_ms(ms: u32) {
    std::thread::sleep(Duration::from_millis(if ms == INFINITE { 1000 } else { ms as u64 }));
}

impl Signal {
    fn make(manual: bool) -> Signal {
        // SAFETY: an unnamed event with default security; closed in Drop.
        let handle = unsafe { CreateEventW(null(), manual as i32, 0, null()) };
        Signal { handle, manual, flag: AtomicBool::new(false) }
    }

    /// Stays set once set: a stop signal.
    pub fn manual() -> Signal {
        Signal::make(true)
    }

    /// Wakes one wait, then resets itself: a nudge.
    pub fn auto() -> Signal {
        Signal::make(false)
    }

    pub fn set(&self) {
        self.flag.store(true, Ordering::SeqCst);
        if !self.handle.is_null() {
            // SAFETY: our own handle.
            unsafe { SetEvent(self.handle) };
        }
    }

    pub fn reset(&self) {
        self.flag.store(false, Ordering::SeqCst);
        if !self.handle.is_null() {
            // SAFETY: our own handle.
            unsafe { windows_sys::Win32::System::Threading::ResetEvent(self.handle) };
        }
    }

    /// Whether a manual-reset event was set (always false for an auto one).
    pub fn is_set(&self) -> bool {
        self.manual && self.flag.load(Ordering::SeqCst)
    }

    /// Waits up to `ms` (INFINITE: no limit) for the event. True if it came.
    pub fn wait(&self, ms: u32) -> bool {
        if self.is_set() {
            return true;
        }
        if self.handle.is_null() {
            sleep_ms(ms);
            return self.is_set();
        }
        // SAFETY: our own handle.
        let r = unsafe { WaitForSingleObject(self.handle, ms) };
        r == WAIT_OBJECT_0 || self.is_set()
    }

    /// The handle, for a wait on several things at once (null if Windows
    /// couldn't make it; `wait_any` leaves those out).
    pub fn handle(&self) -> HANDLE {
        self.handle
    }
}

impl Drop for Signal {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            // SAFETY: our own handle.
            unsafe { CloseHandle(self.handle) };
        }
    }
}

/// Set once when the launcher quits: every background loop waits on it
/// instead of sleeping, so it stops at once.
pub fn shutdown() -> &'static Signal {
    static SHUTDOWN: OnceLock<Signal> = OnceLock::new();
    SHUTDOWN.get_or_init(Signal::manual)
}

fn wake_event() -> &'static Signal {
    static WAKE: OnceLock<Signal> = OnceLock::new();
    WAKE.get_or_init(Signal::auto)
}

/// The launcher just started the game: the watcher looks every 500 ms for the
/// next minute instead of every 5 s.
pub fn wake_watcher() {
    wake_event().set();
}

/// Waits up to `ms` on `handles` (null ones left out, at most 64). Some(i):
/// the index in `handles` of the one that came; None: the time ran out (or the
/// wait failed, after a pause so a caller's loop doesn't spin).
pub fn wait_any(handles: &[HANDLE], ms: u32) -> Option<usize> {
    let live: Vec<(usize, HANDLE)> = handles.iter().copied().enumerate().filter(|(_, h)| !h.is_null()).take(64).collect();
    if live.is_empty() {
        sleep_ms(ms);
        return None;
    }
    let hs: Vec<HANDLE> = live.iter().map(|(_, h)| *h).collect();
    // SAFETY: up to 64 valid handles the caller owns.
    let r = unsafe { WaitForMultipleObjects(hs.len() as u32, hs.as_ptr(), 0, ms) };
    let i = r.wrapping_sub(WAIT_OBJECT_0) as usize;
    if i < hs.len() {
        return Some(live[i].0);
    }
    if r != WAIT_TIMEOUT {
        sleep_ms(ms.min(1000)); // WAIT_FAILED: shouldn't happen
    }
    None
}

// --- Priority -------------------------------------------------------------------

/// EcoQoS for the launcher's own process while the game runs, as the pack's
/// module.cpp sets it for its own threads: Windows runs the launcher slowly
/// and, on hybrid CPUs, on efficiency cores, so it never competes with the
/// game. Off: back to Windows' own choice.
pub fn set_eco_qos(on: bool) -> bool {
    use windows_sys::Win32::System::Threading::{
        GetCurrentProcess, ProcessPowerThrottling, SetProcessInformation, PROCESS_POWER_THROTTLING_CURRENT_VERSION,
        PROCESS_POWER_THROTTLING_EXECUTION_SPEED, PROCESS_POWER_THROTTLING_STATE,
    };
    let mask = if on { PROCESS_POWER_THROTTLING_EXECUTION_SPEED } else { 0 };
    let state = PROCESS_POWER_THROTTLING_STATE { Version: PROCESS_POWER_THROTTLING_CURRENT_VERSION, ControlMask: mask, StateMask: mask };
    // SAFETY: our own process; the struct is the size Windows expects.
    unsafe {
        SetProcessInformation(
            GetCurrentProcess(),
            ProcessPowerThrottling,
            (&state as *const PROCESS_POWER_THROTTLING_STATE).cast(),
            std::mem::size_of::<PROCESS_POWER_THROTTLING_STATE>() as u32,
        ) != 0
    }
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
        // SAFETY: our own handle.
        unsafe { WaitForSingleObject(self.0, timeout_ms) == WAIT_OBJECT_0 }
    }

    /// As `wait`, but returns None as soon as `stop` is set.
    pub fn wait_or(&self, stop: &Signal, timeout_ms: u32) -> Option<bool> {
        let r = wait_any(&[stop.handle(), self.0], timeout_ms);
        if stop.is_set() {
            return None;
        }
        Some(r == Some(1))
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
///
/// While no game runs it looks every `idle_ms` (5 s in the launcher), or
/// every 500 ms for a minute after `wake_watcher` (the launcher started the
/// game). In a session it looks again for new processes only in the first
/// minute (when Steam's root starts its child); after that it just waits for
/// one to exit, and rescans then.
pub struct Watcher {
    roots: Vec<String>,
    procs: Vec<Proc>,
    /// Same-named processes that aren't the game (other Source games, GMod's
    /// helpers), while they still exist: a PID Windows reuses is looked at again.
    skipped: Vec<u32>,
    active: bool,
    created: i64,
    last_exit: u32,
    /// When this session was first seen.
    since: Option<Instant>,
    /// Quick idle scans until then (after a wake).
    quick_until: Option<Instant>,
}

/// How long after a wake the idle scan is quick, and how often it looks then.
const QUICK_FOR: Duration = Duration::from_secs(60);
const QUICK_MS: u32 = 500;
/// How long into a session new processes are looked for, and how often.
const EARLY_FOR: Duration = Duration::from_secs(60);
const EARLY_MS: u32 = 2000;

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

/// An exe name from a process snapshot (UTF-16, NUL-ended) is `want`
/// (lowercase ASCII), compared without allocating.
fn exe_is(name: &[u16], want: &str) -> bool {
    let len = name.iter().position(|&c| c == 0).unwrap_or(name.len());
    len == want.len()
        && name[..len].iter().zip(want.bytes()).all(|(&c, w)| c < 128 && (c as u8).to_ascii_lowercase() == w)
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
            if exe_is(&e.szExeFile, "gmod.exe") || exe_is(&e.szExeFile, "hl2.exe") {
                out.push((e.th32ProcessID, e.th32ParentProcessID));
            }
            more = Process32NextW(snap, &mut e) != 0;
        }
        CloseHandle(snap);
    }
    out
}

/// Whether any gmod.exe/hl2.exe whose image is under `game_root` runs now:
/// one fresh snapshot, for a check that can't wait for the watcher (the
/// updater, just before it writes its pointer).
pub fn game_process_running(game_root: &Path) -> bool {
    let roots = crate::pipe::folder_forms(game_root);
    candidates().into_iter().any(|(pid, _)| {
        // SAFETY: OpenProcess with wait-and-query rights only; closed by Proc's Drop.
        let handle = unsafe { OpenProcess(GAME_ACCESS, 0, pid) };
        if handle.is_null() {
            return false;
        }
        let proc = Proc { handle, pid, created: 0 };
        image_path(proc.handle).is_some_and(|path| roots.iter().any(|r| path.starts_with(r)))
    })
}

impl Watcher {
    pub fn new(game_root: &Path) -> Watcher {
        // Both forms of the folder: a game behind a junction reports its real path.
        let roots = crate::pipe::folder_forms(game_root);
        Watcher {
            roots,
            procs: Vec::new(),
            skipped: Vec::new(),
            active: false,
            created: 0,
            last_exit: 0,
            since: None,
            quick_until: None,
        }
    }

    pub fn running(&self) -> bool {
        self.active
    }

    fn scan(&mut self) {
        let mut found: Vec<(Proc, u32, String)> = Vec::new();
        let candidates = candidates();
        // A skipped PID that's gone may come back as the game: forget it.
        self.skipped.retain(|pid| candidates.iter().any(|(p, _)| p == pid));
        let known: Vec<u32> = self.procs.iter().map(|p| p.pid).chain(self.skipped.iter().copied()).collect();
        for (pid, parent) in candidates {
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

    /// Waits for something to happen: with no game, up to `idle_ms` (less
    /// after a wake); in a session, until a process exits (rescanning every
    /// 2 s, or `idle_ms` if shorter, during its first minute). Returns Idle at
    /// once when `shutdown()` is set.
    pub fn step(&mut self, idle_ms: u32) -> Event {
        let (wake, stop) = (wake_event(), shutdown());
        if self.procs.is_empty() {
            self.scan();
            if self.procs.is_empty() {
                let quick = self.quick_until.is_some_and(|t| Instant::now() < t);
                let ms = if quick { idle_ms.min(QUICK_MS) } else { idle_ms };
                if wait_any(&[wake.handle(), stop.handle()], ms) == Some(0) {
                    self.quick_until = Some(Instant::now() + QUICK_FOR);
                }
                return Event::Idle;
            }
        }
        if !self.active {
            self.active = true;
            self.since = Some(Instant::now());
            self.quick_until = None;
            self.created = self.procs.iter().map(|p| p.created).filter(|&c| c > 0).min().unwrap_or(0);
            return Event::Started { created: self.created };
        }
        let early = self.since.map_or(true, |t| t.elapsed() < EARLY_FOR);
        let timeout = if early { idle_ms.min(EARLY_MS) } else { INFINITE };
        // The game's processes, then the wake and stop events (64 at most in all).
        let mut handles: Vec<HANDLE> = self.procs.iter().take(62).map(|p| p.handle).collect();
        let n = handles.len();
        handles.push(wake.handle());
        handles.push(stop.handle());
        match wait_any(&handles, timeout) {
            None => {
                if early {
                    self.scan();
                }
                return Event::Idle;
            }
            Some(i) if i >= n => {
                if !stop.is_set() {
                    self.scan(); // a wake: look once more
                }
                return Event::Idle;
            }
            Some(i) => {
                let mut code = 0u32;
                // SAFETY: a signalled handle we own.
                unsafe { GetExitCodeProcess(self.procs[i].handle, &mut code) };
                self.last_exit = code;
                self.procs.remove(i);
            }
        }
        self.scan();
        if self.procs.is_empty() {
            // Steam's root gmod.exe may exit just before its child shows up.
            if stop.wait(3000) {
                return Event::Idle; // quitting: no verdict, no card
            }
            self.scan();
        }
        if self.procs.is_empty() {
            self.active = false;
            self.since = None;
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
    fn exe_names() {
        let name = |s: &str| s.encode_utf16().chain(std::iter::repeat(0).take(4)).collect::<Vec<u16>>();
        assert!(exe_is(&name("gmod.exe"), "gmod.exe"));
        assert!(exe_is(&name("GMod.EXE"), "gmod.exe"));
        assert!(exe_is(&"hl2.exe".encode_utf16().collect::<Vec<_>>(), "hl2.exe"), "no NUL at the end");
        assert!(!exe_is(&name("gmod.exe2"), "gmod.exe"));
        assert!(!exe_is(&name("gmod.ex"), "gmod.exe"));
        assert!(!exe_is(&name("gmod\u{130}exe"), "gmod.exe"));
    }

    #[test]
    fn events() {
        let stop = Signal::manual();
        assert!(!stop.is_set() && !stop.wait(10));
        stop.set();
        assert!(stop.is_set() && stop.wait(INFINITE), "stays set");
        let nudge = Signal::auto();
        nudge.set();
        assert_eq!(wait_any(&[stop.handle(), nudge.handle()], 0), Some(0), "the first that's set");
        assert_eq!(wait_any(&[null_mut(), nudge.handle()], 0), Some(1), "null left out, index kept");
        assert_eq!(wait_any(&[nudge.handle()], 0), None, "an auto event resets itself");
        assert!(!nudge.is_set());
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
        assert!(other.skipped.contains(&child.id()), "skipped while it runs");
        let _ = child.wait();
        assert_eq!(other.step(10), Event::Idle);
        assert!(!other.skipped.contains(&child.id()), "forgotten once gone, so a reused PID is looked at again");
        drop(w);
        drop(other);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
