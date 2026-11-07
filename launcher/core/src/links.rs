//! `meowy://` links. The only one so far:
//!
//! `meowy://connect/<a.b.c.d>:<port>` (one trailing `/` allowed)
//!
//! A link can only ever name a public server to join. Never a password,
//! launch option or console command: anything beyond the grammar is refused,
//! not cleaned up.

use crate::address;

pub const SCHEME: &str = "meowy";
const MAX_LEN: usize = 96;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Link {
    /// Join `address` (canonical form). `ours`: one of Meowy Network's
    /// servers, joined without asking; any other is confirmed first.
    Connect { address: String, ours: bool },
}

/// Is this command-line argument a `meowy:` link at all (to refuse or follow)?
pub fn is_link(arg: &str) -> bool {
    let prefix = SCHEME.len() + 1;
    arg.len() > prefix && arg.get(..prefix).is_some_and(|p| p.eq_ignore_ascii_case("meowy:"))
}

/// Why a link was refused, in words for the player.
pub fn parse(url: &str) -> Result<Link, &'static str> {
    if url.len() > MAX_LEN {
        return Err("the link is too long");
    }
    if !url.is_ascii() || url.bytes().any(|b| b.is_ascii_control() || b == b' ') {
        return Err("the link has characters a server address doesn't");
    }
    let lower = url.to_ascii_lowercase();
    let rest = lower.strip_prefix("meowy://").ok_or("not a meowy:// link")?;
    if rest.contains(['?', '#', '@', '%', '\\']) {
        return Err("the link carries more than a server address");
    }
    let target = rest.strip_prefix("connect/").ok_or("the launcher only follows meowy://connect/ links")?;
    let target = target.strip_suffix('/').unwrap_or(target);
    if target.contains('/') {
        return Err("the link carries more than a server address (a password?)");
    }
    let canonical = address::public(target).ok_or("that isn't a public server address (a.b.c.d:port)")?;
    Ok(Link::Connect { ours: address::is_ours(&canonical), address: canonical })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn connects() {
        assert_eq!(
            parse("meowy://connect/138.201.62.52:27015"),
            Ok(Link::Connect { address: "138.201.62.52:27015".into(), ours: true })
        );
        assert_eq!(
            parse("MEOWY://Connect/1.2.3.4:27015/"),
            Ok(Link::Connect { address: "1.2.3.4:27015".into(), ours: false }),
            "case and one trailing slash"
        );
        assert_eq!(
            parse("meowy://connect/138.201.62.052:27015"),
            Ok(Link::Connect { address: "138.201.62.52:27015".into(), ours: true }),
            "canonical form decides ours"
        );
    }

    #[test]
    fn refuses() {
        for bad in [
            "meowy://connect/1.2.3.4:27015/secret",       // a password, steam://connect style
            "meowy://connect/1.2.3.4:27015//",
            "meowy://connect/1.2.3.4:27015?x=1",
            "meowy://connect/1.2.3.4:27015#x",
            "meowy://user@connect/1.2.3.4:27015",
            "meowy://connect/1.2.3.4%3A27015",
            "meowy://connect/192.168.1.10:27015",          // not public
            "meowy://connect/127.0.0.1:27015",
            "meowy://connect/example.com:27015",           // no host names
            "meowy://connect/[::1]:27015",                 // no IPv6
            "meowy://connect/1.2.3.4",                     // no port
            "meowy://connect/1.2.3.4:27015 +exec x",       // no launch options
            "meowy://connect/1.2.3.4:27015\n+exec",
            "meowy://run/4000//+exec x",
            "meowy:connect/1.2.3.4:27015",
            "meowy://connect/",
            "meowy://connect/１.2.3.4:27015",
            "steam://connect/1.2.3.4:27015",
        ] {
            assert!(parse(bad).is_err(), "{bad:?}");
        }
        assert!(parse(&format!("meowy://connect/1.2.3.4:27015/{}", "a".repeat(100))).is_err());
    }

    #[test]
    fn spots_links() {
        assert!(is_link("meowy://connect/1.2.3.4:1"));
        assert!(is_link("MEOWY:x"));
        assert!(!is_link("--tray") && !is_link("meowy") && !is_link("meowy:"));
    }
}
