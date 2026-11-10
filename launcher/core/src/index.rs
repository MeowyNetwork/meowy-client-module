//! Meowy's server index (roadmap MR3, built by the website backend):
//! `GET https://meowynetwork.com/api/pack/index` →
//! `{ v: 1, at, cols: [...], s: [[...], ...] }`, every Garry's Mod server Steam
//! lists, refreshed each minute, with the honest list's verdicts. Columns are
//! read by name, so the backend may add or reorder them.
//!
//! Everything in it is someone else's server text: shown as text only,
//! cleaned and capped here; addresses are only kept if public.

use serde::Serialize;

pub const HOST: &str = "meowynetwork.com";
pub const PATH: &str = "/api/pack/index";
/// Larger than any real index (~0.9 MB for ~6,000 servers today).
pub const MAX_BYTES: usize = 16 * 1024 * 1024;
const MAX_ROWS: usize = 50_000;

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Server {
    pub address: String,
    pub name: String,
    /// The gamemode's title (A2S description), e.g. "DarkRP".
    pub gamemode: String,
    pub map: String,
    /// Steam's count: people only (bots are in `bots`).
    pub players: u32,
    pub max_players: u32,
    pub bots: u32,
    /// 1 password, 0 none, -1 unknown.
    pub password: i8,
    /// "fake", "verified", "unverified" or "".
    pub honest: String,
    /// A fake's real player count, -1 otherwise.
    pub real: i32,
    /// sv_location (country code), or "".
    pub location: String,
    /// Name, gamemode, map and address in lowercase, made once when parsed,
    /// so a search allocates nothing per server.
    #[serde(skip)]
    pub hay: String,
    /// One of Meowy's servers (sorted first), also worked out once.
    #[serde(skip)]
    pub ours: bool,
}

impl Server {
    /// The players to show and sort by: `players` (Steam's list already leaves
    /// bots out, unlike A2S), or a fake's real count.
    pub fn people(&self) -> u32 {
        if self.honest == "fake" && self.real >= 0 {
            self.real as u32
        } else {
            self.players
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Index {
    pub at: i64,
    pub servers: Vec<Server>,
}

fn text(v: Option<&serde_json::Value>, max: usize) -> String {
    let s = v.and_then(|v| v.as_str()).unwrap_or("");
    let mut out: String = s.chars().filter(|c| !c.is_control()).collect();
    if out.len() > max {
        let mut cut = max;
        while !out.is_char_boundary(cut) {
            cut -= 1;
        }
        out.truncate(cut);
    }
    out
}

fn int(v: Option<&serde_json::Value>) -> i64 {
    v.and_then(|v| v.as_i64()).unwrap_or(-1)
}

pub fn parse(json: &[u8]) -> Result<Index, String> {
    let v: serde_json::Value = serde_json::from_slice(json).map_err(|_| "the index isn't JSON")?;
    if v.get("v").and_then(|x| x.as_i64()) != Some(1) {
        return Err("an index version this launcher doesn't know".into());
    }
    let cols: Vec<&str> = v.get("cols").and_then(|c| c.as_array()).ok_or("no columns")?.iter().filter_map(|c| c.as_str()).collect();
    let col = |name: &str| cols.iter().position(|c| *c == name);
    let (a, n, d, m, p, x, b, pw, h, r, l) =
        (col("a").ok_or("no address column")?, col("n"), col("d"), col("m"), col("p"), col("x"), col("b"), col("pw"), col("h"), col("r"), col("l"));
    let rows = v.get("s").and_then(|s| s.as_array()).ok_or("no servers")?;
    let mut servers = Vec::with_capacity(rows.len().min(MAX_ROWS));
    for row in rows.iter().take(MAX_ROWS) {
        let Some(row) = row.as_array() else { continue };
        let get = |i: Option<usize>| i.and_then(|i| row.get(i));
        let Some(address) = row.get(a).and_then(|v| v.as_str()).and_then(crate::address::public) else { continue };
        let count = |i| int(get(i)).clamp(0, 1000) as u32;
        let (name, gamemode, map) = (text(get(n), 96), text(get(d), 64), text(get(m), 64));
        let hay = format!("{name} {gamemode} {map} {address}").to_lowercase();
        let ours = crate::address::is_ours(&address);
        servers.push(Server {
            address,
            name,
            gamemode,
            map,
            hay,
            ours,
            players: count(p),
            max_players: count(x),
            bots: count(b),
            password: int(get(pw)).clamp(-1, 1) as i8,
            honest: match get(h).and_then(|v| v.as_str()) {
                Some(s @ ("fake" | "verified" | "unverified")) => s.into(),
                _ => String::new(),
            },
            real: int(get(r)).clamp(-1, 1000) as i32,
            location: text(get(l), 8),
        });
    }
    Ok(Index { at: v.get("at").and_then(|t| t.as_i64()).unwrap_or(0), servers })
}

/// Servers whose name, gamemode, map or address contain every word of
/// `query` (case-insensitive), most people first, at most `limit`. Servers
/// that need a password come last; Meowy's first.
pub fn search<'a>(index: &'a Index, query: &str, limit: usize) -> Vec<&'a Server> {
    let words: Vec<String> = query.to_lowercase().split_whitespace().take(8).map(|w| w.chars().take(40).collect()).collect();
    let mut hits: Vec<&Server> = index
        .servers
        .iter()
        .filter(|s| words.iter().all(|w| s.hay.contains(w.as_str())))
        .collect();
    hits.sort_by(|a, b| {
        (!a.ours, a.password == 1, std::cmp::Reverse(a.people()), &a.name).cmp(&(!b.ours, b.password == 1, std::cmp::Reverse(b.people()), &b.name))
    });
    hits.truncate(limit);
    hits
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Two real rows from the live index (2026-11-07) and some bad ones.
    const SAMPLE: &str = r#"{"v":1,"at":1794084688,
      "cols":["a","n","d","m","g","w","c","l","p","x","b","v","pw","q","h","r","la","lo"],
      "s":[
        ["194.69.160.130:27015","MUTINY #1 Hiring|Purge|M9K|Drugs|Gambling|","DarkRP","rp_downtown_mutiny_v1","darkrp","248302805","rp","gb",145,128,0,20260429,0,1,"",-1,515,0],
        ["138.201.62.52:27015","Meowy Network | In Dev | Whitelist Only","DarkRP","rp_downtown_meowy","darkrp","248302805","rp","gb",1,31,0,20260429,0,1,"",-1,505,124],
        ["1.2.3.4:27015","Totally Full\u0007 RP","DarkRP","rp_x","darkrp","","rp","",128,128,0,1,0,1,"fake",3,null,null],
        ["5.6.7.8:27015","Locked","Sandbox","gm_construct","sandbox","","","",4,16,2,1,1,1,"verified",-1,null,null],
        ["192.168.0.1:27015","LAN","","","","","","",9,9,0,1,0,1,"",-1,null,null],
        ["not an address","x"],
        "not a row"
      ]}"#;

    #[test]
    fn parses_by_column_name() {
        let idx = parse(SAMPLE.as_bytes()).unwrap();
        assert_eq!(idx.at, 1794084688);
        assert_eq!(idx.servers.len(), 4, "private and malformed rows dropped");
        let m = &idx.servers[0];
        assert_eq!((m.players, m.max_players, m.people()), (145, 128, 145));
        assert!(idx.servers[1].ours && !m.ours, "Meowy's worked out once");
        assert_eq!(idx.servers[3].hay, "locked sandbox gm_construct 5.6.7.8:27015");
        let shown = serde_json::to_string(m).unwrap();
        assert!(!shown.contains("hay") && !shown.contains("ours"), "the search fields stay out of the page: {shown}");
        let fake = &idx.servers[2];
        assert_eq!(fake.name, "Totally Full RP", "control characters dropped");
        assert_eq!((fake.honest.as_str(), fake.people()), ("fake", 3), "a fake shows its real count");
        assert_eq!(idx.servers[3].people(), 4, "Steam's count already leaves the 2 bots out");

        // Reordered columns, an extra one, and a newer version.
        let moved = r#"{"v":1,"cols":["n","new","a","p"],"s":[["A","?","9.9.9.9:1",7]]}"#;
        let s = &parse(moved.as_bytes()).unwrap().servers[0];
        assert_eq!((s.name.as_str(), s.address.as_str(), s.players, s.honest.as_str()), ("A", "9.9.9.9:1", 7, ""));
        assert!(parse(br#"{"v":2,"cols":["a"],"s":[]}"#).is_err());
        assert!(parse(b"<html>").is_err());
        assert!(parse(br#"{"v":1,"cols":["n"],"s":[]}"#).is_err(), "no address column");
    }

    #[test]
    fn searches() {
        let idx = parse(SAMPLE.as_bytes()).unwrap();
        let names = |q: &str| search(&idx, q, 10).iter().map(|s| s.address.clone()).collect::<Vec<_>>();
        assert_eq!(
            names(""),
            vec!["138.201.62.52:27015", "194.69.160.130:27015", "1.2.3.4:27015", "5.6.7.8:27015"],
            "Meowy first, then by real people, password last"
        );
        assert_eq!(names("DOWNTOWN rp"), vec!["138.201.62.52:27015", "194.69.160.130:27015"]);
        assert_eq!(names("construct"), vec!["5.6.7.8:27015"]);
        assert_eq!(names("5.6.7"), vec!["5.6.7.8:27015"]);
        assert!(names("nothing matches this").is_empty());
        assert_eq!(search(&idx, "", 2).len(), 2);
    }
}
