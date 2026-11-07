//! A2S_INFO over plain UDP (Valve's server query protocol), as the pack's
//! `resilience_net.cpp` and `server_rep.cpp` do it: one request, up to two
//! challenge rounds and one resend, a bounded reply, and only to public
//! addresses. Nothing in a reply is trusted beyond being shown as text.

use std::net::{SocketAddrV4, UdpSocket};
use std::time::{Duration, Instant};

const HEADER: [u8; 4] = [0xFF; 4];
const MAX_REPLY: usize = 1400;
/// Garry's Mod's app id, as servers report it.
pub const GMOD_APP: u64 = 4000;

#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
pub struct Info {
    pub name: String,
    pub map: String,
    pub gamemode: String,
    /// The game the server runs (64-bit game id from the EDF, else the 16-bit id).
    pub app_id: u64,
    pub players: u8,
    pub max_players: u8,
    pub bots: u8,
    pub password: bool,
    pub vac: bool,
    pub ping_ms: u32,
}

impl Info {
    pub fn is_gmod(&self) -> bool {
        self.app_id == GMOD_APP
    }
}

/// Text from a server: lossy UTF-8, no control characters, at most 64 bytes.
fn text(bytes: &[u8]) -> String {
    crate::session::clean_name(&String::from_utf8_lossy(bytes))
}

struct Reader<'a> {
    buf: &'a [u8],
    pos: usize,
}

impl<'a> Reader<'a> {
    fn u8(&mut self) -> Option<u8> {
        let b = *self.buf.get(self.pos)?;
        self.pos += 1;
        Some(b)
    }
    fn u16(&mut self) -> Option<u16> {
        Some(u16::from_le_bytes([self.u8()?, self.u8()?]))
    }
    fn u64(&mut self) -> Option<u64> {
        let s = self.buf.get(self.pos..self.pos + 8)?;
        self.pos += 8;
        Some(u64::from_le_bytes(s.try_into().ok()?))
    }
    fn cstr(&mut self) -> Option<&'a [u8]> {
        let rest = self.buf.get(self.pos..)?;
        let end = rest.iter().position(|&b| b == 0)?;
        self.pos += end + 1;
        Some(&rest[..end])
    }
}

/// An `I` reply (header included). `ping_ms` is left 0.
pub fn parse_info(buf: &[u8]) -> Option<Info> {
    if buf.len() < 6 || buf[..4] != HEADER || buf[4] != b'I' {
        return None;
    }
    let mut r = Reader { buf, pos: 6 };
    let name = text(r.cstr()?);
    let map = text(r.cstr()?);
    let _folder = r.cstr()?;
    let gamemode = text(r.cstr()?);
    let id = r.u16()?;
    let players = r.u8()?;
    let max_players = r.u8()?;
    let bots = r.u8()?;
    let _server_type = r.u8()?;
    let _environment = r.u8()?;
    let password = r.u8()? != 0;
    let vac = r.u8()? != 0;
    let _version = r.cstr();
    let mut app_id = id as u64;
    // Extra Data Flag: port (0x80), SteamID (0x10), SourceTV (0x40), keywords (0x20), game id (0x01).
    if let Some(edf) = r.u8() {
        let full = (|| {
            if edf & 0x80 != 0 {
                r.u16()?;
            }
            if edf & 0x10 != 0 {
                r.u64()?;
            }
            if edf & 0x40 != 0 {
                r.u16()?;
                r.cstr()?;
            }
            if edf & 0x20 != 0 {
                r.cstr()?;
            }
            if edf & 0x01 != 0 {
                return r.u64().map(|g| g & 0x00FF_FFFF);
            }
            None
        })();
        if let Some(g) = full {
            app_id = g;
        }
    }
    Some(Info { name, map, gamemode, app_id, players, max_players, bots, password, vac, ping_ms: 0 })
}

fn request(challenge: Option<[u8; 4]>) -> Vec<u8> {
    let mut req = HEADER.to_vec();
    req.push(b'T');
    req.extend_from_slice(b"Source Engine Query\0");
    if let Some(c) = challenge {
        req.extend_from_slice(&c);
    }
    req
}

/// One A2S_INFO exchange with `address` (public `a.b.c.d:port` only).
pub fn query(address: &str, timeout: Duration) -> Result<Info, String> {
    let canonical = crate::address::public(address).ok_or("not a public server address")?;
    query_any(&canonical, timeout)
}

/// The same without the public-address rule (for tests against a local server).
pub fn query_any(address: &str, timeout: Duration) -> Result<Info, String> {
    let to: SocketAddrV4 = address.parse().map_err(|_| "bad address")?;
    let socket = UdpSocket::bind("0.0.0.0:0").map_err(|e| e.to_string())?;
    // Connected: datagrams from anyone but the server are dropped.
    socket.connect(to).map_err(|e| e.to_string())?;
    socket.set_read_timeout(Some(timeout)).map_err(|e| e.to_string())?;
    let mut challenge = None;
    let (mut challenges, mut resends) = (0, 0);
    let mut buf = [0u8; MAX_REPLY];
    while challenges + resends < 4 {
        let sent = Instant::now();
        socket.send(&request(challenge)).map_err(|e| e.to_string())?;
        let n = match socket.recv(&mut buf) {
            Ok(n) => n,
            Err(_) => {
                resends += 1;
                if resends > 1 {
                    return Err("no answer".into());
                }
                continue;
            }
        };
        let reply = &buf[..n];
        if n >= 9 && reply[..4] == HEADER && reply[4] == b'A' {
            challenges += 1;
            if challenges > 2 {
                break;
            }
            challenge = Some([reply[5], reply[6], reply[7], reply[8]]);
            continue;
        }
        let mut info = parse_info(reply).ok_or("not an A2S_INFO reply")?;
        info.ping_ms = sent.elapsed().as_millis().min(9999) as u32;
        return Ok(info);
    }
    Err("no answer".into())
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    /// A GMod server's reply, as Meowy's sends it (name and map shortened).
    pub fn reply(challenge_needed: bool, name: &str) -> Vec<u8> {
        let _ = challenge_needed;
        let mut b = HEADER.to_vec();
        b.extend_from_slice(b"I\x11");
        for s in [name, "rp_downtown_tits_v2", "garrysmod", "DarkRP"] {
            b.extend_from_slice(s.as_bytes());
            b.push(0);
        }
        b.extend_from_slice(&4000u16.to_le_bytes());
        b.extend_from_slice(&[12, 64, 1, b'd', b'l', 0, 1]); // players, max, bots, type, env, password, vac
        b.extend_from_slice(b"2023.06.28\0");
        b.push(0x80 | 0x10 | 0x20 | 0x01); // port, steamid, keywords, game id
        b.extend_from_slice(&27015u16.to_le_bytes());
        b.extend_from_slice(&90071992547409920u64.to_le_bytes());
        b.extend_from_slice(b"gm:darkrp\0");
        b.extend_from_slice(&4000u64.to_le_bytes());
        b
    }

    #[test]
    fn parses_a_gmod_reply() {
        let info = parse_info(&reply(false, "Meowy Network | DarkRP")).unwrap();
        assert_eq!(info.name, "Meowy Network | DarkRP");
        assert_eq!(info.map, "rp_downtown_tits_v2");
        assert_eq!(info.gamemode, "DarkRP");
        assert_eq!((info.players, info.max_players, info.bots), (12, 64, 1));
        assert!(info.vac && !info.password && info.is_gmod());
    }

    #[test]
    fn refuses_garbage_and_cleans_text() {
        assert_eq!(parse_info(b""), None);
        assert_eq!(parse_info(b"\xFF\xFF\xFF\xFFI"), None);
        let mut r = reply(false, "x");
        r.truncate(20);
        assert_eq!(parse_info(&r), None, "cut short");
        let info = parse_info(&reply(false, "a\nb\u{7}c")).unwrap();
        assert_eq!(info.name, "abc", "control characters dropped");
        let long = "é".repeat(40);
        assert!(parse_info(&reply(false, &long)).unwrap().name.len() <= 64);
        // A TF2 reply (no EDF game id): the 16-bit id stands.
        let mut tf2 = reply(false, "tf");
        let at = tf2.windows(2).position(|w| w == 4000u16.to_le_bytes()).unwrap();
        tf2[at..at + 2].copy_from_slice(&440u16.to_le_bytes());
        tf2.truncate(tf2.len() - 8 - 10 - 8 - 2 - 1); // drop the EDF
        assert_eq!(parse_info(&tf2).unwrap().app_id, 440);
    }

    /// A local fake server that asks for a challenge first, like current Source servers.
    #[test]
    fn queries_with_a_challenge_round() {
        let server = UdpSocket::bind("127.0.0.1:0").unwrap();
        let addr = server.local_addr().unwrap().to_string();
        let t = std::thread::spawn(move || {
            let mut buf = [0u8; 1400];
            let (n, from) = server.recv_from(&mut buf).unwrap();
            assert_eq!(&buf[..n], request(None).as_slice());
            server.send_to(b"\xFF\xFF\xFF\xFFA\x01\x02\x03\x04", from).unwrap();
            let (n, from) = server.recv_from(&mut buf).unwrap();
            assert_eq!(&buf[..n], request(Some([1, 2, 3, 4])).as_slice(), "the challenge is echoed");
            server.send_to(&reply(true, "fake"), from).unwrap();
        });
        let info = query_any(&addr, Duration::from_secs(2)).unwrap();
        assert_eq!(info.name, "fake");
        t.join().unwrap();
        assert!(query(&addr, Duration::from_millis(100)).is_err(), "query() only asks public addresses");
    }
}
