//! One HTTPS GET through Windows' own WinHTTP (the OS's TLS and certificate
//! store, the system proxy; no third-party code), as the pack fetches its
//! policy: no redirects, a size cap, gzip taken, `If-None-Match` sent.

use std::ffi::OsStr;
use std::os::windows::ffi::OsStrExt;
use std::ptr::{null, null_mut};

use windows_sys::Win32::Networking::WinHttp::*;

const DECOMPRESSION_FLAG_ALL: u32 = 3; // gzip | deflate

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

/// GET `https://<host><path>`. A body over `max_bytes` is an error, as is a
/// redirect. `etag`: sent as If-None-Match (a 304 comes back with no body).
pub fn get(host: &str, path: &str, etag: Option<&str>, max_bytes: usize, timeout_ms: i32) -> Result<Response, String> {
    let (agent, host_w, path_w, verb) = (wide("MeowyLauncher"), wide(host), wide(path), wide("GET"));
    // SAFETY: plain WinHTTP calls on handles we own, with NUL-terminated strings and sized buffers.
    unsafe {
        let session = Handle(WinHttpOpen(agent.as_ptr(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, null(), null(), 0));
        if session.0.is_null() {
            return Err("WinHTTP isn't available".into());
        }
        WinHttpSetTimeouts(session.0, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
        let connect = Handle(WinHttpConnect(session.0, host_w.as_ptr(), INTERNET_DEFAULT_HTTPS_PORT, 0));
        if connect.0.is_null() {
            return Err("couldn't connect".into());
        }
        let request = Handle(WinHttpOpenRequest(connect.0, verb.as_ptr(), path_w.as_ptr(), null(), null(), null(), WINHTTP_FLAG_SECURE));
        if request.0.is_null() {
            return Err("couldn't open the request".into());
        }
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
                let mut avail: u32 = 0;
                if WinHttpQueryDataAvailable(request.0, &mut avail) == 0 || avail == 0 {
                    break;
                }
                if body.len() + avail as usize > max_bytes {
                    return Err("the answer is too big".into());
                }
                let start = body.len();
                body.resize(start + avail as usize, 0);
                let mut read: u32 = 0;
                if WinHttpReadData(request.0, body[start..].as_mut_ptr().cast(), avail, &mut read) == 0 || read == 0 {
                    body.truncate(start);
                    break;
                }
                body.truncate(start + read as usize);
            }
        }
        Ok(Response { status, body, etag: etag_out })
    }
}
