//! `cargo run -p meowy-launcher-core --example index_live [query]`: fetch the
//! live server index and search it, as the launcher's Find servers does.
fn main() {
    use launcher_core::{http, index};
    let q = std::env::args().nth(1).unwrap_or_default();
    let t = std::time::Instant::now();
    let r = http::get(index::HOST, index::PATH, None, index::MAX_BYTES, 15000).expect("fetch");
    println!("HTTP {} {} bytes etag {:?} in {:.2} s", r.status, r.body.len(), r.etag, t.elapsed().as_secs_f64());
    let idx = index::parse(&r.body).expect("parse");
    println!("{} servers, built at {}", idx.servers.len(), idx.at);
    for s in index::search(&idx, &q, 10) {
        println!("{:<22} {:>3}/{:<3} {:<8} {:<22} {}", s.address, s.people(), s.max_players, s.honest, s.map, s.name);
    }
    let again = http::get(index::HOST, index::PATH, r.etag.as_deref(), index::MAX_BYTES, 15000).expect("refetch");
    println!("with If-None-Match: HTTP {} {} bytes", again.status, again.body.len());
}
