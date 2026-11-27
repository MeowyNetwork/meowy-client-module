//! One HTTPS GET through Windows' own WinHTTP (the OS's TLS and certificate
//! store, the system proxy; no third-party code), as the pack fetches its
//! policy: no redirects, a size cap, gzip taken, `If-None-Match` sent.
//!
//! Every call shares one WinHTTP session, opened at the first and never
//! closed (it ends with the process), so a second fetch to the same host
//! reuses the first one's connection instead of a new TLS handshake. HTTP/2
//! is asked for where Windows has it. Each call has a total deadline as well
//! as WinHTTP's per-step timeouts, so a server that trickles bytes can't hold
//! a thread for ever, and a body cut off in the middle is an error, not a
//! short answer.

use std::ffi::OsStr;
use std::os::windows::ffi::OsStrExt;
use std::ptr::{null, null_mut};
use std::sync::OnceLock;
use std::time::{Duration, Instant};

use windows_sys::Win32::Networking::WinHttp::*;

const DECOMPRESSION_FLAG_ALL: u32 = 3; // gzip | deflate

/// The slowest download rate `get_file` waits for, on top of its 2× timeout.
const MIN_BYTES_PER_SEC: u64 = 32 * 1024;

pub struct Response {
    pub status: u32,
    pub body: Vec<u8>,
    pub etag: Option<String>,
}

fn wide(s: &str) -> Vec<u16> {
    OsStr::new(s).encode_wide().chain(Some(0)).collect()
}

struct Handle(*mut core::ffi::c_void);
impl Drop for Handle {
    fn drop(&mut self) {
        if !self.0.is_null() {
            // SAFETY: a WinHTTP handle we opened.
            unsafe { WinHttpCloseHandle(self.0) };
        }
    }
}

/// The process's WinHTTP session (as an address: raw pointers aren't Sync).
/// Kept open on purpose: closing it at exit would race threads still fetching.
static SESSION: OnceLock<usize> = OnceLock::new();

fn session() -> Option<*mut core::ffi::c_void> {
    if let Some(&s) = SESSION.get() {
        return Some(s as *mut _);
    }
    let agent = wide("MeowyLauncher");
    // SAFETY: a valid agent string; the handle is kept for the process's life.
    let s = unsafe { WinHttpOpen(agent.as_ptr(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, null(), null(), 0) };
    if s.is_null() {
        return None; // tried again at the next call
    }
    // HTTP/2 where this Windows has it (1607 and later); HTTP/1.1 otherwise.
    let h2: u32 = WINHTTP_PROTOCOL_FLAG_HTTP2;
    // SAFETY: our session; a u32 option.
    unsafe { WinHttpSetOption(s, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, (&h2 as *const u32).cast(), 4) };
    match SESSION.set(s as usize) {
        Ok(()) => Some(s),
        Err(_) => {
            // Another thread opened one first: use that one, close ours.
            // SAFETY: ours, never handed out.
            unsafe { WinHttpCloseHandle(s) };
            SESSION.get().map(|&s| s as *mut _)
        }
    }
}

/// GET `https://<host><path>`, a document: a body over `max_bytes` is an
/// error, as is a redirect. `etag`: sent as If-None-Match (a 304 comes back
/// with no body). `timeout_ms` bounds each step; the whole call gets twice that.
pub fn get(host: &str, path: &str, etag: Option<&str>, max_bytes: usize, timeout_ms: i32) -> Result<Response, String> {
    let total = Duration::from_millis(2 * timeout_ms.max(1) as u64);
    get_within(host, path, etag, max_bytes, timeout_ms, total)
}

/// As `get`, for a file of up to `max_bytes` (the pack's DLL): the deadline
/// grows with the size, at 32 KB/s on top of twice the timeout.
pub fn get_file(host: &str, path: &str, max_bytes: usize, timeout_ms: i32) -> Result<Response, String> {
    let total = Duration::from_millis(2 * timeout_ms.max(1) as u64) + Duration::from_secs(max_bytes as u64 / MIN_BYTES_PER_SEC);
    get_within(host, path, None, max_bytes, timeout_ms, total)
}

fn get_within(host: &str, path: &str, etag: Option<&str>, max_bytes: usize, timeout_ms: i32, total: Duration) -> Result<Response, String> {
    let start = Instant::now();
    let (host_w, path_w, verb) = (wide(host), wide(path), wide("GET"));
    let session = session().ok_or("WinHTTP isn't available")?;
    // What's left of the deadline, as a WinHTTP timeout (never 0: that's "none").
    let left = || -> Option<i32> {
        let rest = total.checked_sub(start.elapsed())?;
        Some((rest.as_millis().min(timeout_ms.max(1) as u128) as i32).max(1))
    };
    // SAFETY: plain WinHTTP calls on handles we own, with NUL-terminated strings and sized buffers.
    unsafe {
        let connect = Handle(WinHttpConnect(session, host_w.as_ptr(), INTERNET_DEFAULT_HTTPS_PORT, 0));
        if connect.0.is_null() {
            return Err("couldn't connect".into());
        }
        let request = Handle(WinHttpOpenRequest(connect.0, verb.as_ptr(), path_w.as_ptr(), null(), null(), null(), WINHTTP_FLAG_SECURE));
        if request.0.is_null() {
            return Err("couldn't open the request".into());
        }
        WinHttpSetTimeouts(request.0, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
        let never: u32 = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request.0, WINHTTP_OPTION_REDIRECT_POLICY, (&never as *const u32).cast(), 4);
        let decompress: u32 = DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(request.0, WINHTTP_OPTION_DECOMPRESSION, (&decompress as *const u32).cast(), 4);
        let headers = match etag {
            Some(t) if t.len() < 200 && t.is_ascii() && !t.contains(['\r', '\n']) => wide(&format!("If-None-Match: {t}\r\n")),
            _ => wide(""),
        };
        let header_len = if headers.len() > 1 { u32::MAX } else { 0 };
        if WinHttpSendRequest(request.0, headers.as_ptr(), header_len, null(), 0, 0, 0) == 0
            || WinHttpReceiveResponse(request.0, null_mut()) == 0
        {
            return Err("no answer from the server".into());
        }
        let mut status: u32 = 0;
        let mut size: u32 = 4;
        WinHttpQueryHeaders(
            request.0,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            null(),
            (&mut status as *mut u32).cast(),
            &mut size,
            null_mut(),
        );
        let mut tag = [0u16; 256];
        let mut tag_size = (tag.len() * 2) as u32;
        let etag_out = (WinHttpQueryHeaders(request.0, WINHTTP_QUERY_ETAG, null(), tag.as_mut_ptr().cast(), &mut tag_size, null_mut()) != 0)
            .then(|| String::from_utf16_lossy(&tag[..(tag_size as usize / 2)]));
        let mut body = Vec::new();
        if status == 200 {
            loop {
                // Each wait for more of the body gets only what's left of the deadline.
                let Some(ms) = left() else { return Err("the answer took too long".into()) };
                WinHttpSetOption(request.0, WINHTTP_OPTION_RECEIVE_TIMEOUT, (&ms as *const i32).cast(), 4);
                let mut avail: u32 = 0;
                if WinHttpQueryDataAvailable(request.0, &mut avail) == 0 {
                    return Err(if left().is_none() { "the answer took too long" } else { "the answer was cut off" }.into());
                }
                if avail == 0 {
                    break; // the whole body
                }
                if body.len() + avail as usize > max_bytes {
                    return Err("the answer is too big".into());
                }
                let at = body.len();
                body.resize(at + avail as usize, 0);
                let mut read: u32 = 0;
                if WinHttpReadData(request.0, body[at..].as_mut_ptr().cast(), avail, &mut read) == 0 {
                    return Err("the answer was cut off".into());
                }
                body.truncate(at + read as usize);
                if read == 0 {
                    break;
                }
            }
        }
        Ok(Response { status, body, etag: etag_out })
    }
}
