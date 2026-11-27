//! Meowy Launcher: the tray app around `launcher_core`. See `launcher/DESIGN.md`.
//!
//! Arguments: none (show the window), `--tray` (start in the tray, from the
//! Run value), `--play` (start the game, as the tray's Play), `--quit` (the
//! installer asks a running launcher to exit), or a `meowy://` link. A second
//! start hands its arguments to the running one.
//!
//! Threads: the window system's own thread (tray, windows, quick commands)
//! never reads files, hashes or starts programs, and nothing does any of that
//! while holding the state's lock: values are copied out, the lock dropped,
//! the work done, and the result put back in a second short lock. Commands
//! that touch the disk run on the async runtime's blocking pool; the tray's
//! Play, Join and Repair and a link's launch get a thread of their own. The
//! background loops each run on a named thread that's started again if it
//! panics, wait on the process-wide stop event instead of sleeping, and are
//! joined (for about 2 s at most) when the launcher quits.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::any::Any;
use std::panic::AssertUnwindSafe;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard, OnceLock};
use std::thread::JoinHandle;
use std::time::{Duration, Instant, SystemTime};

use launcher_core::links::Link;
use launcher_core::settings::{self, Settings};
use launcher_core::{a2s, address, fonts, http, index, line, links, log, pipe, policy, release, servers, session, steam, updater, win};
use serde::Serialize;
use tauri::menu::{Menu, MenuItem, PredefinedMenuItem};
use tauri::tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent};
use tauri::{AppHandle, Emitter, Manager, RunEvent, WebviewUrl, WebviewWindow, WebviewWindowBuilder, WindowEvent};

/// What the small card window shows.
#[derive(Debug, Clone, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
enum Card {
    /// The game ended unexpectedly on a server.
    Crash { address: String, name: String, report: bool },
    /// A link asks to join a server that isn't Meowy Network's.
    Confirm { address: String },
    /// Something the player should know (a refused link, Steam not found).
    Notice { title: String, text: String },
    /// Meowy Network answers again after being down (opt-in).
    BackUp { address: String, name: String },
}

struct Inner {
    steam_exe: Option<PathBuf>,
    steam_path: Option<PathBuf>,
    game: Option<steam::Game>,
    settings: Settings,
    running: bool,
    last_repair: String,
    card: Option<Card>,
    /// The server list last shown: with the index, the only addresses the window may join.
    servers: Vec<servers::Row>,
    /// Meowy's server index and its ETag, while the main window is open.
    index: Option<(Arc<index::Index>, Option<String>)>,
    /// When the index was last asked for (one try a minute, failed or not),
    /// and why that failed if it did.
    index_tried: Option<(Instant, Option<String>)>,
    /// Lines to the pack over the pipe, and what it last said (None: not connected).
    pipe_tx: Option<std::sync::mpsc::Sender<String>>,
    pack: Option<PackState>,
    /// The pack's signed policy as kept in its state folder (None: none, all on).
    policy: Option<policy::Policy>,
    /// What the last update check said (core/src/updater.rs).
    update_check: String,
}

impl Inner {
    fn allows(&self, feature: &str) -> bool {
        policy::launcher_allows(self.policy.as_ref(), feature)
    }

    fn take(&mut self, found: Located) {
        self.steam_exe = found.steam_exe;
        self.steam_path = found.steam_path;
        self.game = found.game;
    }
}

#[derive(Debug, Clone, Default, Serialize)]
struct PackState {
    version: String,
    /// "menu", "joining" or "ingame".
    state: String,
    address: Option<String>,
}

struct AppState(Mutex<Inner>);

/// Held around find_servers' staleness check, fetch and store, so keystrokes
/// in a row wait for one fetch instead of each starting their own.
struct IndexFetch(tauri::async_runtime::Mutex<()>);

/// The launcher's state. A thread that panicked while holding it leaves its
/// fields as they were (each is a value of its own), so the lock is taken
/// anyway instead of every other thread panicking after it.
fn inner(app: &AppHandle) -> MutexGuard<'_, Inner> {
    app.state::<AppState>().inner().0.lock().unwrap_or_else(|p| p.into_inner())
}

fn launcher_dir() -> PathBuf {
    settings::launcher_dir().unwrap_or_else(|| {
        std::env::current_exe().ok().and_then(|e| e.parent().map(|p| p.to_path_buf())).unwrap_or_default()
    })
}

fn data_dir() -> PathBuf {
    settings::pack_data_dir().unwrap_or_else(launcher_dir)
}

/// Steam and the game as Steam's own files say now.
struct Located {
    steam_exe: Option<PathBuf>,
    steam_path: Option<PathBuf>,
    game: Option<steam::Game>,
}

/// Reads the registry, libraryfolders.vdf and each library's appmanifest:
/// never with the state's lock held.
fn locate() -> Located {
    match win::steam_install() {
        Some((exe, path)) => Located { steam_exe: Some(exe), game: steam::find_game(&path), steam_path: Some(path) },
        None => Located { steam_exe: None, steam_path: None, game: None },
    }
}

/// Puts the pack's start-up line back if Steam took it away. Only the line,
/// only when the pack's start-up stubs are installed, and never while the
/// game runs. `relocate`: find Steam and the game again first. The files
/// are read and written without the state's lock.
fn repair(app: &AppHandle, why: &str, relocate: bool) -> String {
    if relocate {
        let found = locate();
        inner(app).take(found);
    }
    let (game, allowed, running) = {
        let inner = inner(app);
        (inner.game.clone(), inner.allows("repair"), inner.running)
    };
    let Some(game) = game else { return "Garry's Mod wasn't found in Steam's libraries.".into() };
    if !allowed {
        return "Repairs are switched off for now by Meowy Network.".into();
    }
    if running {
        return "Garry's Mod is running; the launcher repairs when it's closed.".into();
    }
    let missing = game.missing_modules();
    let said = if missing.iter().any(|m| m.starts_with("gmsv_meowy_boot")) {
        format!("The Meowy Client Pack's files are missing ({}). Run MeowyClientPack.exe again.", missing.join(", "))
    } else {
        match line::heal(&game.garrysmod, &data_dir()) {
            line::Heal::Present => "Nothing to repair.".into(),
            line::Heal::Off => "\"Start with the game\" is off, so the start-up line stays out. Run MeowyClientPack.exe again with it ticked to turn it back on.".into(),
            line::Heal::Added => "Put the pack's start-up line back (Steam had replaced menu.lua).".into(),
            line::Heal::Failed(why) => format!("Couldn't repair: {why}."),
        }
    };
    log::line(&format!("repair ({why}): {said}"));
    inner(app).last_repair = said.clone();
    said
}

/// Starts the game through Steam (repairing first), or asks the running game
/// to join `connect`. Reads files and may start programs: never call it on
/// the window system's thread (see `launch_later`) or a runtime worker.
fn launch(app: &AppHandle, connect: Option<String>) -> Result<String, String> {
    // Started as administrator (an installer run that way): Steam and the game
    // would get that token too, with Steam's path from a value any program can change.
    if win::elevated() {
        log::line("launch refused: the launcher runs as administrator");
        return Err("The Meowy Launcher is running as administrator, so it won't start Steam or the game. \
            Quit it from the tray and open it again from the Start menu."
            .into());
    }
    let found = locate();
    let (running, pack_tx, exe, game, faster) = {
        let mut inner = inner(app);
        inner.take(found);
        let pack_tx = inner.pack.as_ref().and(inner.pipe_tx.clone());
        let faster = inner.settings.disable_lua_refresh && inner.allows("fast_start");
        (inner.running, pack_tx, inner.steam_exe.clone(), inner.game.clone(), faster)
    };
    if running {
        let Some(address) = connect else { return Ok("Garry's Mod is already running.".into()) };
        // The pack is connected: it joins itself (and checks the address again).
        if let Some(tx) = pack_tx {
            if let Some(line) = pipe::join_line(&address) {
                if tx.send(line).is_ok() {
                    log::line(&format!("join through the pack: {address}"));
                    return Ok(format!("Joining {address}..."));
                }
            }
        }
        let url = steam::connect_url(&address).ok_or("That isn't a public server address.")?;
        log::line(&format!("join in the running game: {address}"));
        return if win::shell_open(&url) { Ok(format!("Joining {address}...")) } else { Err("Steam didn't take the join.".into()) };
    }
    let exe = exe.ok_or("Steam wasn't found. Is it installed?")?;
    if game.is_some() {
        // Never blocks the launch: a failed repair is reported, the game still starts.
        repair(app, "launch", false);
    }
    let args = steam::launch_args(faster, connect.as_deref());
    if let Some(garrysmod) = game.map(|g| g.garrysmod).filter(|g| faster && line::present(g)) {
        lazy_fonts(garrysmod);
    }
    std::process::Command::new(&exe).args(&args).spawn().map_err(|e| format!("Couldn't start Steam: {e}"))?;
    // The watcher looks every 500 ms for the next minute, not every 5 s.
    win::wake_watcher();
    log::line(&format!("launch: steam {}", args.join(" ")));
    let _ = app.emit("status", ());
    Ok(match connect {
        Some(a) => format!("Starting Garry's Mod and joining {a}..."),
        None => "Starting Garry's Mod...".into(),
    })
}

/// `launch` on a thread of its own, for the tray and links (handled on the
/// window system's thread): a failure shows a card titled `title`.
fn launch_later(app: &AppHandle, connect: Option<String>, title: &'static str) {
    let app = app.clone();
    keep("launch", move || {
        if let Err(e) = launch(&app, connect) {
            notice(&app, title, &e);
        }
    });
}

/// Bumped by each launch that sets the fonts aside: an older launch's timer
/// then leaves them to the newer one.
static FONTS_LAUNCH: AtomicU64 = AtomicU64::new(0);

/// Lazy fonts (core/src/fonts.rs): the Workshop font cache goes aside for this
/// start and comes back as soon as the pack's start-up stub runs, which is
/// after the start-up font pass. Only when the start-up line is in place, so
/// the stub will run: its first file-name change in the pack's data folder
/// (recover.lock, even in safe mode) is the signal. Otherwise 45 s, the
/// game's exit or the launcher's next start puts it back. A second Play
/// within the 45 s takes over: the first one's timer then does nothing.
fn lazy_fonts(garrysmod: PathBuf) {
    let this = FONTS_LAUNCH.fetch_add(1, Ordering::SeqCst) + 1;
    match fonts::set_aside(&garrysmod) {
        Ok(true) => {}
        Ok(false) => return,
        Err(e) => {
            log::line(&format!("lazy fonts: couldn't set them aside ({e})"));
            return;
        }
    }
    let data = data_dir();
    // Armed before the game starts, so the stub's first change can't be missed.
    let watch = win::NameWatch::new(&data);
    keep("lazy-fonts", move || {
        let run = || {
            let start = Instant::now();
            let stop = win::shutdown();
            let signal = match watch {
                Some(w) => w.wait_or(stop, 45_000),
                None if stop.wait(45_000) => None,
                None => Some(false),
            };
            // Quitting: the launcher's exit puts them back, once.
            let Some(signal) = signal else { return };
            let why = if signal { format!("the pack started after {:.1} s", start.elapsed().as_secs_f64()) } else { "45 s".into() };
            match fonts::restore_unless(&garrysmod, || FONTS_LAUNCH.load(Ordering::SeqCst) != this) {
                None => log::line("lazy fonts: a newer start set them aside; its own timer puts them back"),
                Some(Ok(n)) => log::line(&format!("lazy fonts: {n} put back ({why})")),
                Some(Err(e)) => log::line(&format!("lazy fonts: couldn't put them back yet ({e}); retried at exit and next start")),
            }
        };
        // One-shot: a panic is logged, and the exit or next start puts them back.
        if let Err(p) = std::panic::catch_unwind(AssertUnwindSafe(run)) {
            log::line(&format!("lazy-fonts died: {}", panic_text(&*p)));
        }
    });
}

fn restore_fonts(game: Option<&steam::Game>, why: &str) {
    if let Some(g) = game {
        match fonts::restore(&g.garrysmod) {
            Ok(0) => {}
            Ok(n) => log::line(&format!("lazy fonts: {n} put back ({why})")),
            Err(e) => log::line(&format!("lazy fonts: couldn't put them back ({why}): {e}")),
        }
    }
}

// --- Threads ---------------------------------------------------------------------

/// The launcher's threads, joined (about 2 s at most in all) when it quits.
static THREADS: Mutex<Vec<JoinHandle<()>>> = Mutex::new(Vec::new());

fn threads() -> MutexGuard<'static, Vec<JoinHandle<()>>> {
    THREADS.lock().unwrap_or_else(|p| p.into_inner())
}

fn panic_text(p: &(dyn Any + Send)) -> String {
    p.downcast_ref::<&str>().map(|s| s.to_string()).or_else(|| p.downcast_ref::<String>().cloned()).unwrap_or_else(|| "a panic".into())
}

/// Starts `f` on a thread named `name` and keeps its handle for the exit.
fn keep(name: &str, f: impl FnOnce() + Send + 'static) {
    match std::thread::Builder::new().name(name.into()).spawn(f) {
        Ok(h) => {
            let mut all = threads();
            all.retain(|t| !t.is_finished());
            all.push(h);
        }
        Err(e) => log::line(&format!("{name}: couldn't start a thread ({e})")),
    }
}

/// A background loop on a thread of its own: if it panics, the log says so
/// and it starts again 5 s later. It ends when `body` returns (the launcher
/// quitting, or nothing for it to do).
fn supervise(app: &AppHandle, name: &'static str, body: fn(&AppHandle)) {
    let app = app.clone();
    keep(name, move || loop {
        match std::panic::catch_unwind(AssertUnwindSafe(|| body(&app))) {
            Ok(()) => return,
            Err(p) => {
                log::line(&format!("{name} died: {}; starting it again in 5 s", panic_text(&*p)));
                if win::shutdown().wait(5000) {
                    return;
                }
            }
        }
    });
}

/// Quitting: every loop is told to stop, and given up to `within` in all to
/// finish what it's doing (a rename, a log line). A thread still busy (a
/// fetch, an A2S query) is left to the process's exit.
fn stop_threads(within: Duration) {
    win::shutdown().set();
    let deadline = Instant::now() + within;
    let all = std::mem::take(&mut *threads());
    let mut busy = Vec::new();
    for t in all {
        while !t.is_finished() && Instant::now() < deadline {
            std::thread::sleep(Duration::from_millis(10));
        }
        if t.is_finished() {
            let _ = t.join();
        } else {
            busy.push(t.thread().name().unwrap_or("?").to_string());
        }
    }
    if !busy.is_empty() {
        log::line(&format!("quit: not waiting for {}", busy.join(", ")));
    }
}

// --- Windows ------------------------------------------------------------------

fn show_main(app: &AppHandle) {
    if let Some(w) = app.get_webview_window("main") {
        let _ = w.unminimize();
        let _ = w.show();
        let _ = w.set_focus();
        return;
    }
    let built = WebviewWindowBuilder::new(app, "main", WebviewUrl::App("index.html".into()))
        .title("Meowy Launcher")
        .inner_size(460.0, 720.0)
        .resizable(false)
        .maximizable(false)
        .data_directory(launcher_dir().join("webview"))
        .build();
    if let Err(e) = built {
        log::line(&format!("main window: {e}"));
    }
}

fn show_card(app: &AppHandle, card: Card) {
    inner(app).card = Some(card);
    if let Some(w) = app.get_webview_window("card") {
        let _ = w.emit("card", ());
        let _ = w.show();
        let _ = w.set_focus();
        return;
    }
    let (width, height) = (420.0, 210.0);
    let mut b = WebviewWindowBuilder::new(app, "card", WebviewUrl::App("card.html".into()))
        .title("Meowy Launcher")
        .inner_size(width, height)
        .resizable(false)
        .maximizable(false)
        .minimizable(false)
        .always_on_top(true)
        .focused(true)
        .data_directory(launcher_dir().join("webview"));
    // Bottom-right of the main screen, above the taskbar.
    if let Ok(Some(m)) = app.primary_monitor() {
        let s = m.scale_factor();
        let area = m.work_area();
        let x = (area.position.x as f64 + area.size.width as f64) / s - width - 16.0;
        let y = (area.position.y as f64 + area.size.height as f64) / s - height - 48.0;
        b = b.position(x.max(0.0), y.max(0.0));
    }
    if let Err(e) = b.build() {
        log::line(&format!("card window: {e}"));
    }
}

fn notice(app: &AppHandle, title: &str, text: &str) {
    show_card(app, Card::Notice { title: title.into(), text: text.into() });
}

// --- Arguments and links --------------------------------------------------------

fn handle_args(app: &AppHandle, args: &[String]) {
    let args = args.get(1..).unwrap_or(&[]);
    if args.iter().any(|a| a == "--quit") {
        log::line("quit (asked by another start)");
        app.exit(0);
        return;
    }
    if let Some(url) = args.iter().find(|a| links::is_link(a)) {
        if !inner(app).allows("links") {
            notice(app, "Link not followed", "meowy:// links are switched off for now by Meowy Network.");
            return;
        }
        match links::parse(url) {
            Ok(Link::Connect { address, ours: true }) => {
                log::line(&format!("link: join {address}"));
                launch_later(app, Some(address), "Couldn't join");
            }
            Ok(Link::Connect { address, ours: false }) => {
                log::line(&format!("link: confirm {address}"));
                show_card(app, Card::Confirm { address });
            }
            Err(reason) => {
                log::line(&format!("link refused: {reason}"));
                notice(app, "Link not followed", &format!("The launcher didn't follow this link: {reason}."));
            }
        }
        return;
    }
    if args.iter().any(|a| a == "--play") {
        launch_later(app, None, "Couldn't start Garry's Mod");
        return;
    }
    if args.iter().any(|a| a == "--tray") {
        return;
    }
    show_main(app);
}

// --- Background: the game's processes, and Steam's files -----------------------

fn now() -> i64 {
    SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}

/// EcoQoS back to Windows' choice when the game watcher ends, however it ends.
struct EcoQosOff;

impl Drop for EcoQosOff {
    fn drop(&mut self) {
        win::set_eco_qos(false);
    }
}

/// The game's processes (core/src/win.rs). While the game runs, the
/// launcher's own process runs with EcoQoS, so it never competes with it.
fn watch_game(app: &AppHandle) {
    let _eco_off = EcoQosOff;
    let stop = win::shutdown();
    let mut watcher: Option<(PathBuf, win::Watcher)> = None;
    while !stop.is_set() {
        let root = inner(app).game.as_ref().map(|g| g.root.clone());
        let Some(root) = root else {
            stop.wait(5000);
            continue;
        };
        if watcher.as_ref().map(|(r, _)| r != &root).unwrap_or(true) {
            watcher = Some((root.clone(), win::Watcher::new(&root)));
        }
        let Some((_, w)) = watcher.as_mut() else { continue };
        match w.step(5000) {
            win::Event::Idle => {}
            win::Event::Started { created } => {
                win::set_eco_qos(true);
                log::line(&format!("game started ({}s ago)", now() - created));
                inner(app).running = true;
                let _ = app.emit("status", ());
            }
            win::Event::Ended { exit_code, created } => {
                win::set_eco_qos(false);
                let game = {
                    let mut inner = inner(app);
                    inner.running = false;
                    inner.game.clone()
                };
                restore_fonts(game.as_ref(), "the game closed");
                let data = data_dir();
                let file = session::read_server_file(&data);
                let verdict = session::verdict(file, created, exit_code, |since| session::crash_report_since(&root, since));
                log::line(&format!("game ended: exit code 0x{exit_code:08X}, {verdict:?}"));
                if let session::Verdict::Crashed { address, name, report } = verdict {
                    // Shown here, so the pack doesn't ask again at the next start.
                    session::consume_server_file(&data);
                    show_card(app, Card::Crash { address, name, report });
                }
                let _ = app.emit("status", ());
            }
        }
    }
}

/// "Meowy Network is back up": while the setting is on and the game is
/// closed, one A2S_INFO to Meowy every 30 s; two missed answers in a row mean
/// down, and the first answer after that shows the card. Off, it only waits.
fn watch_back_up(app: &AppHandle) {
    let mut tracker = servers::BackUp::default();
    while !win::shutdown().wait(30_000) {
        let (on, running) = {
            let inner = inner(app);
            (inner.settings.notify_back_up, inner.running)
        };
        if !on || running {
            tracker = servers::BackUp::default();
            continue;
        }
        let reply = a2s::query(address::MEOWY, Duration::from_millis(1500)).ok();
        if tracker.observe(reply.is_some()) {
            let name = reply.map(|i| i.name).unwrap_or_default();
            log::line("Meowy Network is back up");
            show_card(app, Card::BackUp { address: address::MEOWY.into(), name });
        }
    }
}

/// The pack's signed policy (core/src/policy.rs): each minute, the kept one is
/// verified again if its files changed (the pack may have kept a newer one),
/// and while the game is closed the same files the pack fetches are fetched
/// every 30 minutes, so the start-up stub's policy.bin is current before the
/// pack would load. Only with the pack's production keys (read from
/// src/policy.cpp at build time).
fn watch_policy(app: &AppHandle) {
    let keys = policy::Keys::production();
    let Some(dir) = policy::state_dir() else { return };
    let stop = win::shutdown();
    let mut last_fetch: Option<Instant> = None;
    let mut last_said = String::new();
    // Size and write time of policy.txt and .sig at the last verify.
    let mut verified: Option<policy::Stamp> = None;
    // Let the game watcher see a game that's already running before the first fetch.
    if stop.wait(10_000) {
        return;
    }
    if keys.any() && policy::heal_stub(&keys, &dir) {
        log::line("policy: the start-up stub's copy written again from the kept policy");
    }
    loop {
        let running = inner(app).running;
        if keys.any() && !running && last_fetch.map_or(true, |t| t.elapsed() >= Duration::from_secs(30 * 60)) {
            last_fetch = Some(Instant::now());
            let said = match (
                http::get(policy::HOST, policy::TXT_PATH, None, policy::MAX_BODY + 1, 15000),
                http::get(policy::HOST, policy::SIG_PATH, None, 1024, 15000),
            ) {
                (Ok(txt), Ok(sig)) if txt.status == 200 && sig.status == 200 => {
                    match policy::offer(&keys, &dir, &txt.body, &String::from_utf8_lossy(&sig.body)) {
                        Ok(seq) => format!("policy {seq} kept"),
                        Err(e) => format!("policy not taken: {e}"),
                    }
                }
                (Ok(t), Ok(s)) => format!("policy: HTTP {} / {}", t.status, s.status),
                (Err(e), _) | (_, Err(e)) => format!("policy: {e}"),
            };
            if said != last_said {
                log::line(&said);
                last_said = said;
            }
        }
        // Stamped before reading, so a change while verifying is seen next time.
        let stamp = policy::kept_stamp(&dir);
        if verified.as_ref() != Some(&stamp) {
            verified = Some(stamp);
            let kept = policy::current(&keys, &dir).map(|(p, _)| p);
            let mut inner = inner(app);
            if inner.policy != kept {
                log::line(&format!("policy in force: {}", kept.as_ref().map_or("none".into(), |p| format!("seq {}", p.seq))));
                inner.policy = kept;
                drop(inner);
                let _ = app.emit("status", ());
            }
        }
        if stop.wait(60_000) {
            return;
        }
    }
}

/// The pack's signed updater (core/src/updater.rs, the same document and
/// checks as src/updater.cpp): a minute after start, then every 6 hours, and
/// only staged while the game is closed (the pack does it while it runs).
/// The start-up stub takes a staged version at the next start. A download the
/// game's start overtook is staged again soon after the game closes.
fn watch_updates(app: &AppHandle) {
    let keys = policy::Keys::production();
    let stop = win::shutdown();
    if stop.wait(60_000) {
        return;
    }
    loop {
        let (game, pol) = {
            let inner = inner(app);
            (inner.game.clone(), inner.policy.clone())
        };
        let said = match game {
            None => "Garry's Mod wasn't found".to_string(),
            Some(_) if !keys.any() => "no release key in this build".to_string(),
            Some(g) => {
                let now = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
                let a = app.clone();
                let running = move || inner(&a).running;
                let fetch = |path: &str, cap: u64| -> Result<Vec<u8>, String> {
                    // The DLL gets a deadline by its size; documents twice the timeout.
                    let r = if cap > release::MAX_BODY as u64 {
                        http::get_file(updater::HOST, path, cap as usize, 30_000)?
                    } else {
                        http::get(updater::HOST, path, None, cap as usize, 30_000)?
                    };
                    if r.status == 200 {
                        Ok(r.body)
                    } else {
                        Err(format!("HTTP {}", r.status))
                    }
                };
                let (said, staged) =
                    updater::check_once(&keys, &data_dir(), &g.garrysmod, g.arch(), pol.as_ref(), now, &running, &fetch);
                if let Some(v) = staged {
                    log::line(&format!("update: staged {v} for the next start"));
                }
                said
            }
        };
        let again_soon = said.contains(updater::WILL_STAGE);
        {
            let mut inner = inner(app);
            if inner.update_check != said {
                log::line(&format!("update check: {said}"));
                inner.update_check = said;
            }
        }
        let _ = app.emit("status", ());
        if again_soon {
            // Each minute, until the game is closed (at most 6 hours).
            for _ in 0..6 * 60 {
                if stop.wait(60_000) {
                    return;
                }
                if !inner(app).running {
                    break;
                }
            }
        } else if stop.wait(6 * 60 * 60 * 1000) {
            return;
        }
    }
}

/// Every 15 s, one stat each of `appmanifest_4000.acf` and `menu.lua`. When
/// either changed and then stayed the same for a whole interval, with the
/// game closed and Steam done with it, the start-up line is checked.
fn watch_steam(app: &AppHandle) {
    let mut last: Option<Vec<Option<SystemTime>>> = None;
    let mut checked: Option<Vec<Option<SystemTime>>> = None;
    while !win::shutdown().wait(15_000) {
        let mut game = inner(app).game.clone();
        if game.is_none() {
            let found = locate();
            game = found.game.clone();
            inner(app).take(found);
        }
        let Some(game) = game else { continue };
        let stamp = |p: PathBuf| std::fs::metadata(p).and_then(|m| m.modified()).ok();
        // The player's "Start with the game" choice too, so turning it back on is seen.
        let sig = vec![
            stamp(game.manifest.clone()),
            stamp(game.garrysmod.join("lua\\includes\\menu.lua")),
            stamp(data_dir().join("startup_off.txt")),
        ];
        let stable = last.as_ref() == Some(&sig);
        last = Some(sig.clone());
        if !stable || checked.as_ref() == Some(&sig) || inner(app).running {
            continue;
        }
        if !game.read_manifest().map(|m| m.settled()).unwrap_or(false) {
            continue; // Steam is updating or verifying: wait until it's done.
        }
        if line::present(&game.garrysmod) {
            checked = Some(sig);
            continue;
        }
        repair(app, "Steam changed the game's files", false);
        // A repair that put the line back changed menu.lua: the next stable stamp
        // is checked once more (cheaply). One that didn't ("Start with the game"
        // is off, the pack's files are missing, the write failed) isn't tried
        // again until Steam or the player changes these files.
        if !line::present(&game.garrysmod) {
            checked = Some(sig);
        }
        let _ = app.emit("status", ());
    }
}

/// The pipe to the pack in the game (core/src/pipe.rs). Only the game's own
/// gmod.exe may connect; what it says shows in the window.
fn serve_pipe(app: &AppHandle) {
    let (tx, rx) = std::sync::mpsc::channel::<String>();
    inner(app).pipe_tx = Some(tx);
    let a = app.clone();
    let check: pipe::ClientCheck = Box::new(move |image| {
        let root = inner(&a).game.as_ref().map(|g| g.root.clone());
        root.is_some_and(|r| pipe::is_game_client(image, &r))
    });
    let b = app.clone();
    // Runs until the launcher quits; a pipe it can't make is logged and tried again.
    pipe::serve(
        check,
        rx,
        move |msg| {
            {
                let mut inner = inner(&b);
                match msg {
                    Some(pipe::Msg::Hello { version }) => {
                        log::line(&format!("pack {version} connected"));
                        inner.pack = Some(PackState { version, state: "menu".into(), address: None });
                    }
                    Some(pipe::Msg::State { state, address }) => {
                        let pack = inner.pack.get_or_insert_with(PackState::default);
                        pack.state = state;
                        pack.address = address;
                    }
                    None => {
                        log::line("pack disconnected");
                        inner.pack = None;
                    }
                }
            }
            let _ = b.emit("status", ());
        },
        win::shutdown(),
    );
}

// --- Commands for the pages -------------------------------------------------------

/// Runs `f` on the async runtime's blocking pool: for commands that read or
/// write files, start programs or wait on the network.
async fn blocking<T: Send + 'static>(f: impl FnOnce() -> T + Send + 'static) -> Result<T, String> {
    tauri::async_runtime::spawn_blocking(f).await.map_err(|e| e.to_string())
}

#[derive(Serialize)]
struct Status {
    version: &'static str,
    steam: bool,
    game_found: bool,
    garrysmod: String,
    build_id: String,
    steam_busy: bool,
    /// "present", "off", "missing" or "unknown".
    line: &'static str,
    missing_modules: Vec<&'static str>,
    running: bool,
    disable_lua_refresh: bool,
    notify_back_up: bool,
    start_at_sign_in: bool,
    last_repair: String,
    pack: Option<PackState>,
    updates: updater::Status,
    /// The policy's notice for players, if it has one.
    notice: String,
}

#[tauri::command]
async fn status(app: AppHandle) -> Result<Status, String> {
    blocking(move || status_now(&app)).await
}

/// What the window shows: the state copied out under the lock, then Steam's
/// files, the start-up line, the pack's files and versions read without it.
fn status_now(app: &AppHandle) -> Status {
    let (running, settings, last_repair, pack, policy, update_check) = {
        let inner = inner(app);
        (
            inner.running,
            inner.settings.clone(),
            inner.last_repair.clone(),
            inner.pack.clone(),
            inner.policy.clone(),
            inner.update_check.clone(),
        )
    };
    let found = locate();
    let (steam, game) = (found.steam_exe.is_some(), found.game.clone());
    inner(app).take(found);
    let manifest = game.as_ref().and_then(|g| g.read_manifest());
    let line = match &game {
        None => "unknown",
        Some(g) if line::present(&g.garrysmod) => "present",
        Some(_) if line::off(&data_dir()) => "off",
        Some(_) => "missing",
    };
    Status {
        version: env!("MEOWY_PACK_VERSION"),
        steam,
        game_found: game.is_some(),
        garrysmod: game.as_ref().map(|g| g.garrysmod.display().to_string()).unwrap_or_default(),
        build_id: manifest.as_ref().map(|m| m.build_id.clone()).unwrap_or_default(),
        steam_busy: manifest.as_ref().map(|m| !m.settled()).unwrap_or(false),
        line,
        missing_modules: game.as_ref().map(|g| g.missing_modules()).unwrap_or_default(),
        running,
        disable_lua_refresh: settings.disable_lua_refresh,
        notify_back_up: settings.notify_back_up,
        start_at_sign_in: win::start_at_sign_in(),
        last_repair,
        pack,
        updates: {
            let arch = game.as_ref().map_or("win64", |g| g.arch());
            updater::status(&data_dir(), game.as_ref().map(|g| g.garrysmod.as_path()), arch, policy.as_ref(), &update_check)
        },
        notice: policy.as_ref().map(|p| p.notice.clone()).unwrap_or_default(),
    }
}

#[tauri::command]
async fn play(app: AppHandle, join_meowy: bool) -> Result<String, String> {
    blocking(move || launch(&app, join_meowy.then(|| address::MEOWY.to_string()))).await?
}

#[tauri::command]
async fn repair_now(app: AppHandle) -> Result<String, String> {
    blocking(move || repair(&app, "asked", true)).await
}

/// Changes one setting under the lock and saves a copy without it.
fn change_setting(app: &AppHandle, name: &str, on: bool, set: impl FnOnce(&mut Settings)) -> Result<(), String> {
    let settings = {
        let mut inner = inner(app);
        set(&mut inner.settings);
        inner.settings.clone()
    };
    log::line(&format!("setting: {name} = {on}"));
    settings.save(&launcher_dir()).map_err(|e| e.to_string())
}

#[tauri::command]
async fn set_lua_refresh(app: AppHandle, on: bool) -> Result<(), String> {
    blocking(move || change_setting(&app, "disable_lua_refresh", on, |s| s.disable_lua_refresh = on)).await?
}

#[tauri::command]
async fn set_back_up(app: AppHandle, on: bool) -> Result<(), String> {
    blocking(move || change_setting(&app, "notify_back_up", on, |s| s.notify_back_up = on)).await?
}

/// Meowy Network, the player's Steam favourites and recent Garry's Mod
/// servers, each asked over A2S (takes up to ~3 s).
#[tauri::command]
async fn server_list(app: AppHandle) -> Vec<servers::Row> {
    let (allowed, steam_path) = {
        let inner = inner(&app);
        (inner.allows("servers"), inner.steam_path.clone())
    };
    if !allowed {
        return Vec::new();
    }
    let a = app.clone();
    let rows = tauri::async_runtime::spawn_blocking(move || {
        let steam_path = steam_path.or_else(|| {
            let found = locate();
            let path = found.steam_path.clone();
            inner(&a).take(found);
            path
        });
        let entries = match (steam_path, win::steam_active_account()) {
            (Some(path), Some(account)) => std::fs::read(servers::history_file(&path, account))
                .map(|b| servers::parse_history(&String::from_utf8_lossy(&b)))
                .unwrap_or_default(),
            _ => Vec::new(),
        };
        servers::query_all(&servers::candidates(&entries, 12), Duration::from_millis(1500))
    })
    .await
    .unwrap_or_default();
    inner(&app).servers = rows.clone();
    rows
}

#[derive(Serialize)]
struct Found {
    /// When the backend built the index (unix seconds).
    at: i64,
    total: usize,
    servers: Vec<index::Server>,
}

/// Searches Meowy's server index (fetched when the window asks, at most once
/// a minute, with If-None-Match; it's only fetched while the window is used,
/// and dropped when it closes). Keystrokes during a fetch wait for it.
#[tauri::command]
async fn find_servers(app: AppHandle, query: String) -> Result<Found, String> {
    if !inner(&app).allows("servers") {
        return Err("Finding servers is switched off for now by Meowy Network.".into());
    }
    let gate = app.state::<IndexFetch>();
    let idx = {
        let _one_fetch = gate.0.lock().await;
        let (stale, etag) = {
            let inner = inner(&app);
            (
                inner.index_tried.as_ref().map_or(true, |(t, _)| t.elapsed() > Duration::from_secs(60)),
                inner.index.as_ref().and_then(|(_, etag)| etag.clone()),
            )
        };
        if stale {
            // Taken before the fetch, so a failure waits a minute too.
            let tried = Instant::now();
            inner(&app).index_tried = Some((tried, None));
            let fetched = blocking(move || -> Result<Option<(index::Index, Option<String>)>, String> {
                let r = http::get(index::HOST, index::PATH, etag.as_deref(), index::MAX_BYTES, 15000)?;
                match r.status {
                    304 => Ok(None),
                    200 => Ok(Some((index::parse(&r.body)?, r.etag))),
                    s => Err(format!("the server index answered {s}")),
                }
            })
            .await?;
            let mut inner = inner(&app);
            match fetched {
                Ok(Some((idx, etag))) => inner.index = Some((Arc::new(idx), etag)),
                Ok(None) => {}
                Err(e) => {
                    // Keep showing an older copy if there is one.
                    if inner.index.is_some() {
                        log::line(&format!("index: {e}; showing the older copy"));
                    }
                    inner.index_tried = Some((tried, Some(e)));
                }
            }
        }
        let inner = inner(&app);
        match (&inner.index, &inner.index_tried) {
            (Some((idx, _)), _) => idx.clone(),
            (None, Some((_, Some(e)))) => return Err(format!("Couldn't get the server list: {e}")),
            _ => return Err("No server list yet.".into()),
        }
    };
    // The search runs on this copy, with no lock held.
    let query: String = query.chars().take(100).collect();
    Ok(Found { at: idx.at, total: idx.servers.len(), servers: index::search(&idx, &query, 25).into_iter().cloned().collect() })
}

/// Joins a server from the list or the index last shown (the page can't name any other).
#[tauri::command]
async fn join_server(app: AppHandle, address: String) -> Result<String, String> {
    let listed = {
        let inner = inner(&app);
        inner.servers.iter().any(|r| r.address == address)
            || inner.index.as_ref().is_some_and(|(idx, _)| idx.servers.iter().any(|s| s.address == address))
    };
    if !listed {
        return Err("That server isn't in the list.".into());
    }
    log::line(&format!("window: join {address}"));
    blocking(move || launch(&app, Some(address))).await?
}

#[tauri::command]
async fn set_sign_in(on: bool) -> Result<(), String> {
    blocking(move || {
        let exe = std::env::current_exe().map_err(|e| e.to_string())?;
        log::line(&format!("setting: start at sign-in = {on}"));
        if win::set_start_at_sign_in(&exe, on) {
            Ok(())
        } else {
            Err("Windows didn't take the change.".into())
        }
    })
    .await?
}

#[tauri::command]
fn card(app: AppHandle) -> Option<Card> {
    inner(&app).card.clone()
}

/// The card's buttons. The address always comes from the launcher's own
/// state, never from the page.
#[tauri::command]
async fn card_answer(app: AppHandle, window: WebviewWindow, action: String) -> Result<String, String> {
    let card = inner(&app).card.clone();
    let close = |app: &AppHandle| {
        inner(app).card = None;
        let _ = window.destroy();
    };
    match (action.as_str(), card) {
        ("join", Some(Card::Crash { address, .. }))
        | ("join", Some(Card::Confirm { address }))
        | ("join", Some(Card::BackUp { address, .. })) => {
            log::line(&format!("card: join {address}"));
            close(&app);
            blocking(move || launch(&app, Some(address))).await?
        }
        ("details", _) => {
            let root = inner(&app).game.as_ref().map(|g| g.root.join("crashes"));
            let opened = blocking(move || root.is_some_and(|dir| dir.is_dir() && win::shell_open(&dir.display().to_string()))).await?;
            if opened {
                Ok(String::new())
            } else {
                Err("No crash reports folder.".into())
            }
        }
        _ => {
            close(&app);
            Ok(String::new())
        }
    }
}

// --- Start ------------------------------------------------------------------------

fn tray(app: &AppHandle) -> tauri::Result<()> {
    let open = MenuItem::with_id(app, "open", "Open Meowy Launcher", true, None::<&str>)?;
    let play = MenuItem::with_id(app, "play", "Play Garry's Mod", true, None::<&str>)?;
    let join = MenuItem::with_id(app, "join", "Join Meowy Network", true, None::<&str>)?;
    let fix = MenuItem::with_id(app, "repair", "Repair now", true, None::<&str>)?;
    let quit = MenuItem::with_id(app, "quit", "Quit", true, None::<&str>)?;
    let menu = Menu::with_items(
        app,
        &[&open, &PredefinedMenuItem::separator(app)?, &play, &join, &fix, &PredefinedMenuItem::separator(app)?, &quit],
    )?;
    TrayIconBuilder::with_id("tray")
        .icon(app.default_window_icon().cloned().expect("the exe's icon"))
        .tooltip("Meowy Launcher")
        .menu(&menu)
        .show_menu_on_left_click(false)
        // On the window system's thread: anything that reads files or starts
        // programs goes to a thread of its own.
        .on_menu_event(|app, event| match event.id().as_ref() {
            "open" => show_main(app),
            "play" | "join" => {
                let connect = (event.id().as_ref() == "join").then(|| address::MEOWY.to_string());
                launch_later(app, connect, "Couldn't start Garry's Mod");
            }
            "repair" => {
                let app = app.clone();
                keep("repair", move || {
                    let said = repair(&app, "tray", true);
                    notice(&app, "Repair", &said);
                });
            }
            "quit" => app.exit(0),
            _ => {}
        })
        .on_tray_icon_event(|tray, event| {
            if let TrayIconEvent::Click { button: MouseButton::Left, button_state: MouseButtonState::Up, .. } = event {
                show_main(tray.app_handle());
            }
        })
        .build(app)?;
    Ok(())
}

/// `meowy://` points at this exe when it's the installed one (never a
/// development build somewhere else) and the key is missing or stale.
fn keep_protocol() {
    let Ok(exe) = std::env::current_exe() else { return };
    let installed = exe.parent().map(|p| p == launcher_dir()).unwrap_or(false);
    let current = win::protocol_command().and_then(|c| win::command_exe(&c)).filter(|p| p.is_file());
    if installed && current.is_none() && win::register_protocol(&exe) {
        log::line("registered meowy://");
    }
}

/// The async runtime Tauri runs commands on, kept for the process's life.
static RUNTIME: OnceLock<tokio::runtime::Runtime> = OnceLock::new();

/// Tauri's default runtime starts a worker per logical CPU (16-24 threads on
/// a gaming PC) for a handful of short commands. Two workers are plenty (one
/// stays free while another command runs), and the blocking pool, where the
/// file and network work goes, is capped at 8 instead of 512. If this one
/// can't be made, Tauri makes its default.
fn use_small_runtime() {
    let built = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .max_blocking_threads(8)
        .thread_name("launcher-rt")
        .enable_all()
        .build();
    match built {
        Ok(rt) => {
            let handle = rt.handle().clone();
            if RUNTIME.set(rt).is_ok() {
                tauri::async_runtime::set(handle);
            }
        }
        Err(e) => log::line(&format!("runtime: {e}; using Tauri's default")),
    }
}

fn main() {
    let dir = launcher_dir();
    log::init(&dir);
    // A panic anywhere goes to the log (a thread that dies says so there too).
    let default_hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        let thread = std::thread::current();
        log::try_line(&format!("panic on {}: {info}", thread.name().unwrap_or("a thread")));
        default_hook(info);
    }));
    use_small_runtime();
    let settings = Settings::load(&dir);
    let mut state = Inner {
        steam_exe: None,
        steam_path: None,
        game: None,
        settings,
        running: false,
        last_repair: String::new(),
        card: None,
        servers: Vec::new(),
        index: None,
        index_tried: None,
        pipe_tx: None,
        pack: None,
        policy: None,
        update_check: "not checked yet".into(),
    };
    state.take(locate());
    // Links aren't logged as given: a refused one may carry a password.
    let args: Vec<String> =
        std::env::args().skip(1).map(|a| if links::is_link(&a) { "<link>".into() } else { a }).collect();
    log::line(&format!(
        "start {} ({:?}); game: {}",
        env!("MEOWY_PACK_VERSION"),
        args,
        state.game.as_ref().map(|g| g.garrysmod.display().to_string()).unwrap_or_else(|| "not found".into())
    ));

    let app = tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, args, _cwd| handle_args(app, &args)))
        .manage(AppState(Mutex::new(state)))
        .manage(IndexFetch(tauri::async_runtime::Mutex::new(())))
        .invoke_handler(tauri::generate_handler![
            status,
            play,
            repair_now,
            set_lua_refresh,
            set_back_up,
            set_sign_in,
            server_list,
            find_servers,
            join_server,
            card,
            card_answer
        ])
        // The index is only for the window: ~2 MB given back when it closes.
        .on_window_event(|window, event| {
            if let WindowEvent::Destroyed = event {
                if window.label() == "main" {
                    let mut inner = inner(window.app_handle());
                    inner.index = None;
                    inner.index_tried = None;
                }
            }
        })
        .setup(|app| {
            let handle = app.handle().clone();
            tray(&handle)?;
            keep_protocol();
            repair(&handle, "start", false);
            let (running, game) = {
                let inner = inner(&handle);
                (inner.running, inner.game.clone())
            };
            if !running {
                restore_fonts(game.as_ref(), "launcher start");
            }
            supervise(&handle, "watch_game", watch_game);
            supervise(&handle, "serve_pipe", serve_pipe);
            supervise(&handle, "watch_steam", watch_steam);
            supervise(&handle, "watch_back_up", watch_back_up);
            supervise(&handle, "watch_policy", watch_policy);
            supervise(&handle, "watch_updates", watch_updates);
            handle_args(&handle, &std::env::args().collect::<Vec<_>>());
            Ok(())
        })
        .build(tauri::generate_context!())
        .expect("the launcher's window system");

    app.run(|app, event| match event {
        // Closing the last window keeps the launcher in the tray; only Quit exits.
        RunEvent::ExitRequested { code: None, api, .. } => api.prevent_exit(),
        // Quit (the tray, --quit, the installer): every thread told to stop and
        // given a moment, then the Workshop fonts back, once, or nothing would
        // put them back until the launcher's next start.
        RunEvent::Exit => {
            stop_threads(Duration::from_secs(2));
            let game = inner(app).game.clone();
            restore_fonts(game.as_ref(), "the launcher quit");
        }
        _ => {}
    });
}
