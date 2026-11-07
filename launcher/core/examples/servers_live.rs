//! `cargo run -p meowy-launcher-core --example servers_live`: the launcher's
//! server list on this PC (Steam's history for the signed-in account + A2S).
fn main() {
    use launcher_core::{servers, win};
    let (_, steam) = win::steam_install().expect("Steam");
    let account = win::steam_active_account().expect("signed in to Steam");
    let file = servers::history_file(&steam, account);
    let entries = servers::parse_history(&std::fs::read_to_string(&file).unwrap_or_default());
    let candidates = servers::candidates(&entries, 12);
    println!("{} entries in {}, asking {}", entries.len(), file.display(), candidates.len());
    let t = std::time::Instant::now();
    for r in servers::query_all(&candidates, std::time::Duration::from_millis(1500)) {
        let fav = if r.favorite { "  [fav]" } else { "" };
        match &r.info {
            Some(i) => println!(
                "{:<22} {:>4} ms {:>3}/{:<3} {:<24} {}{fav}",
                r.address, i.ping_ms, i.players, i.max_players, i.map, i.name
            ),
            None => println!("{:<22} no answer{fav}", r.address),
        }
    }
    println!("{:.2} s", t.elapsed().as_secs_f64());
}
