//! Server addresses: Meowy Network's, and the public IPv4 rule the pack uses
//! (`server_rep::IsPublicAddress`) for any other server.

/// Meowy Network's servers (`src/server_gate.cpp` `kAllowed`). Only these
/// ports: the same host also runs throwaway test servers.
pub const OURS: [&str; 1] = ["138.201.62.52:27015"];

/// The address Join Meowy Network connects to.
pub const MEOWY: &str = OURS[0];

/// `a.b.c.d:port` with each octet 0..=255 (1-3 digits) and port 1..=65535,
/// nothing else. Returns the address in canonical form.
pub fn parse(address: &str) -> Option<([u8; 4], u16)> {
    let (host, port) = address.split_once(':')?;
    let digits = |s: &str, max: usize| !s.is_empty() && s.len() <= max && s.bytes().all(|b| b.is_ascii_digit());
    if !digits(port, 5) {
        return None;
    }
    let port: u32 = port.parse().ok()?;
    if port == 0 || port > 65535 {
        return None;
    }
    let mut ip = [0u8; 4];
    let mut parts = host.split('.');
    for octet in ip.iter_mut() {
        let p = parts.next()?;
        if !digits(p, 3) {
            return None;
        }
        *octet = u8::try_from(p.parse::<u32>().ok()?).ok()?;
    }
    if parts.next().is_some() {
        return None;
    }
    Some((ip, port as u16))
}

pub fn format(ip: [u8; 4], port: u16) -> String {
    format!("{}.{}.{}.{}:{}", ip[0], ip[1], ip[2], ip[3], port)
}

/// Same exclusions as `server_rep::ParsePublicAddress`: no 0/8, 10/8, 127/8,
/// 169.254/16, 172.16/12, 192.168/16, 100.64/10 or 224 and up.
pub fn is_public_ip(ip: [u8; 4]) -> bool {
    let [a, b, _, _] = ip;
    !(a == 10
        || a == 127
        || a == 0
        || a >= 224
        || (a == 169 && b == 254)
        || (a == 172 && (16..=31).contains(&b))
        || (a == 192 && b == 168)
        || (a == 100 && (64..=127).contains(&b)))
}

/// A public server address in canonical form, or None.
pub fn public(address: &str) -> Option<String> {
    let (ip, port) = parse(address)?;
    is_public_ip(ip).then(|| format(ip, port))
}

pub fn is_ours(address: &str) -> bool {
    parse(address).map(|(ip, port)| OURS.contains(&format(ip, port).as_str())).unwrap_or(false)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ours_matches_server_gate() {
        let cpp = include_str!("../../../src/server_gate.cpp");
        for a in OURS {
            let (ip, port) = a.split_once(':').unwrap();
            assert!(cpp.contains(&format!("{{ \"{ip}\", {port} }}")), "{a} is in server_gate.cpp kAllowed");
        }
        assert!(is_ours("138.201.62.52:27015"));
        assert!(!is_ours("138.201.62.52:27016"), "same host, test server port");
        assert!(!is_ours("138.201.62.52"));
        assert!(!is_ours("1.2.3.4:27015"));
    }

    #[test]
    fn public_rule() {
        assert_eq!(public("1.2.3.4:27015").as_deref(), Some("1.2.3.4:27015"));
        assert_eq!(public("001.2.3.4:27015").as_deref(), Some("1.2.3.4:27015"), "canonical");
        for bad in [
            "10.0.0.1:27015", "127.0.0.1:27015", "0.1.2.3:1", "224.0.0.1:1", "255.255.255.255:1",
            "169.254.1.1:1", "172.16.0.1:1", "172.31.255.255:1", "192.168.1.1:1", "100.64.0.1:1", "100.127.0.1:1",
        ] {
            assert_eq!(public(bad), None, "{bad}");
        }
        assert!(public("172.15.0.1:1").is_some() && public("172.32.0.1:1").is_some());
        assert!(public("100.63.0.1:1").is_some() && public("100.128.0.1:1").is_some());
    }

    #[test]
    fn strict_grammar() {
        for bad in [
            "", "1.2.3.4", "1.2.3.4:", "1.2.3.4:0", "1.2.3.4:65536", "1.2.3.4:123456", "1.2.3:27015",
            "1.2.3.4.5:27015", "256.1.1.1:27015", "1.2.3.4:27015x", " 1.2.3.4:27015", "1.2.3.4:+27015",
            "1.2.3.-4:27015", "1.2.3.4:27015:1", "a.b.c.d:27015", "1.2.3.4:27 015", "1111.2.3.4:1",
        ] {
            assert_eq!(parse(bad), None, "{bad:?}");
        }
        assert_eq!(parse("8.8.8.8:65535"), Some(([8, 8, 8, 8], 65535)));
    }
}
