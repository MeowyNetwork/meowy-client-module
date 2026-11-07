//! The servers the launcher shows while Garry's Mod is closed: Meowy Network
//! first, then the player's own Steam favourites and recent servers
//! (`userdata\<account>\7\remote\serverbrowser_hist.vdf`, Steam's file for
//! every game's server browser), each asked directly over A2S.
//!
//! Steam's list is shared by every Source game (entries say `appid 0`), so a
//! recent server is only shown when it answers as Garry's Mod. A favourite
//! that doesn't answer is still shown (as not answering): it's the player's.

use crate::a2s::{self, Info};
use crate::address;
use crate::steam::{parse_kv, Kv};
use serde::Serialize;
use std::path::{Path, PathBuf};
use std::time::Duration;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    pub address: String,
    pub favorite: bool,
    pub last_played: i64,
}

pub fn history_file(steam_path: &Path, account_id: u32) -> PathBuf {
    steam_path.join("userdata").join(account_id.to_string()).join("7\\remote\\serverbrowser_hist.vdf")
}

/// Favourites (file order), then history (most recent first). Public
/// addresses only, each once.
pub fn parse_history(vdf: &str) -> Vec<Entry> {
    let root = parse_kv(vdf);
    let filters = root.get("filters").unwrap_or(&root);
    let mut out: Vec<Entry> = Vec::new();
    let push = |section: Option<&Kv>, favorite: bool, out: &mut Vec<Entry>| {
        let mut items: Vec<Entry> = section
            .map(|s| s.children())
            .unwrap_or(&[])
            .iter()
            .filter_map(|(_, e)| {
                let address = address::public(e.str("address")?)?;
                let last_played = e.str("LastPlayed").and_then(|t| t.parse().ok()).unwrap_or(0);
                Some(Entry { address, favorite, last_played })
            })
            .collect();
        if !favorite {
            items.sort_by(|a, b| b.last_played.cmp(&a.last_played));
        }
        for e in items {
            if !out.iter().any(|o| o.address == e.address) {
                out.push(e);
            }
        }
    };
    push(filters.get("favorites"), true, &mut out);
    push(filters.get("history"), false, &mut out);
    out
}

/// What to ask: Meowy's servers, every favourite, and the `recent` most recent others.
pub fn candidates(entries: &[Entry], recent: usize) -> Vec<Entry> {
    let mut out: Vec<Entry> = address::OURS
        .iter()
        .map(|a| {
            let known = entries.iter().find(|e| e.address == *a);
            Entry { address: a.to_string(), favorite: known.is_some_and(|e| e.favorite), last_played: known.map_or(0, |e| e.last_played) }
        })
        .collect();
    for e in entries.iter().filter(|e| e.favorite) {
        if !out.iter().any(|o| o.address == e.address) {
            out.push(e.clone());
        }
    }
    for e in entries.iter().filter(|e| !e.favorite).take(recent) {
        if !out.iter().any(|o| o.address == e.address) {
            out.push(e.clone());
        }
    }
    out
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Row {
    pub address: String,
    pub ours: bool,
    pub favorite: bool,
    pub last_played: i64,
    /// None: no answer.
    pub info: Option<Info>,
}

/// Which rows to show, in order: ours, then favourites, then recent ones that
/// answered as Garry's Mod.
pub fn keep(rows: Vec<Row>) -> Vec<Row> {
    rows.into_iter()
        .filter(|r| match &r.info {
            Some(i) => i.is_gmod(),
            None => r.ours || r.favorite,
        })
        .collect()
}

/// Asks every candidate at once (one thread each, at most 32).
pub fn query_all(candidates: &[Entry], timeout: Duration) -> Vec<Row> {
    let handles: Vec<_> = candidates
        .iter()
        .take(32)
        .cloned()
        .map(|e| std::thread::spawn(move || {
            let info = a2s::query(&e.address, timeout).ok();
            Row { ours: address::is_ours(&e.address), address: e.address, favorite: e.favorite, last_played: e.last_played, info }
        }))
        .collect();
    keep(handles.into_iter().filter_map(|h| h.join().ok()).collect())
}

/// Tracks whether Meowy Network is up, for "it's back up": two missed
/// answers in a row mean down, and the first answer after that is "back up".
#[derive(Debug, Default)]
pub struct BackUp {
    misses: u32,
    down: bool,
}

impl BackUp {
    /// Returns true when the server just came back after being down.
    pub fn observe(&mut self, answered: bool) -> bool {
        if answered {
            let back = self.down;
            self.misses = 0;
            self.down = false;
            back
        } else {
            self.misses += 1;
            if self.misses >= 2 {
                self.down = true;
            }
            false
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const HIST: &str = r#""filters"
{
	"favorites"
	{
		"1" { "name" "x" "address" "81.19.214.35:28025" "LastPlayed" "1793176557" "appid" "0" }
		"2" { "address" "192.168.1.5:27015" "LastPlayed" "5" }
	}
	"history"
	{
		"1" { "address" "208.103.169.209:27015" "LastPlayed" "100" }
		"2" { "address" "81.19.214.35:28025" "LastPlayed" "300" }
		"3" { "address" "138.201.62.52:27015" "LastPlayed" "200" }
		"4" { "address" "not an address" "LastPlayed" "999" }
	}
}"#;

    #[test]
    fn reads_steams_file() {
        let e = parse_history(HIST);
        let a: Vec<(&str, bool)> = e.iter().map(|e| (e.address.as_str(), e.favorite)).collect();
        assert_eq!(
            a,
            vec![("81.19.214.35:28025", true), ("138.201.62.52:27015", false), ("208.103.169.209:27015", false)],
            "favourites first, private and bad addresses out, history by LastPlayed, each once"
        );
        assert_eq!(parse_history(""), vec![]);
        assert_eq!(parse_history("garbage { \"x\""), vec![]);
    }

    #[test]
    fn meowy_first_then_favourites_then_recent() {
        let e = parse_history(HIST);
        let c: Vec<String> = candidates(&e, 1).into_iter().map(|e| e.address).collect();
        assert_eq!(c, vec!["138.201.62.52:27015", "81.19.214.35:28025"], "recent capped at 1 (Meowy's already in)");
        let c: Vec<String> = candidates(&[], 5).into_iter().map(|e| e.address).collect();
        assert_eq!(c, vec!["138.201.62.52:27015"], "Meowy even with no history");
    }

    #[test]
    fn shows_gmod_servers_and_the_players_own() {
        let info = |app| Some(Info {
            name: "n".into(), map: "m".into(), gamemode: "g".into(), app_id: app,
            players: 1, max_players: 2, bots: 0, password: false, vac: true, ping_ms: 30,
        });
        let row = |a: &str, ours, favorite, info| Row { address: a.into(), ours, favorite, last_played: 0, info };
        let kept = keep(vec![
            row("138.201.62.52:27015", true, false, None),   // ours, down: shown
            row("1.1.1.1:1", false, true, None),              // favourite, no answer: shown
            row("2.2.2.2:2", false, false, None),             // recent, no answer: hidden
            row("3.3.3.3:3", false, false, info(440)),        // recent TF2: hidden
            row("4.4.4.4:4", false, true, info(730)),         // favourite CS: hidden
            row("5.5.5.5:5", false, false, info(4000)),       // recent GMod: shown
        ]);
        let a: Vec<&str> = kept.iter().map(|r| r.address.as_str()).collect();
        assert_eq!(a, vec!["138.201.62.52:27015", "1.1.1.1:1", "5.5.5.5:5"]);
    }

    #[test]
    fn back_up() {
        let mut b = BackUp::default();
        assert!(!b.observe(true));
        assert!(!b.observe(false), "one miss isn't down");
        assert!(!b.observe(true), "...and coming back from it isn't news");
        assert!(!b.observe(false));
        assert!(!b.observe(false));
        assert!(!b.observe(false));
        assert!(b.observe(true), "back after being down");
        assert!(!b.observe(true), "said once");
    }

    #[test]
    fn kv_tree() {
        let kv = parse_kv("\"a\" { \"b\" \"c\" \"d\" { \"e\" \"f\" } }");
        assert_eq!(kv.get("A").and_then(|a| a.str("b")), Some("c"));
        assert_eq!(kv.get("a").and_then(|a| a.get("d")).and_then(|d| d.str("e")), Some("f"));
        let deep = "\"k\" {".repeat(40);
        let _ = parse_kv(&deep); // no stack overflow on deep nesting
    }
}
