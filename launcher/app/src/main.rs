//! Meowy Launcher: the tray app around `launcher_core`. See `launcher/DESIGN.md`.
//!
//! Arguments: none (show the window), `--tray` (start in the tray, from the
//! Run value), `--play` (start the game, as the tray's Play), `--quit` (the
//! installer asks a running launcher to exit), or a `meowy://` link. A second
//! start hands its arguments to the running one.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::path::PathBuf;
use std::sync::Mutex;
use std::time::{Duration, SystemTime};

use launcher_core::links::Link;
use launcher_core::settings::{self, Settings};
use launcher_core::{a2s, address, fonts, http, index, line, links, log, pipe, policy, servers, session, steam, updater, win};
use serde::Serialize;
use tauri::menu::{Menu, MenuItem, PredefinedMenuItem};
use tauri::tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent};
use tauri::{AppHandle, Emitter, Manager, RunEvent, WebviewUrl, WebviewWindow, WebviewWindowBuilder};

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
    /// Meowy's server index, when it was fetched, and its ETag.
    index: Option<(index::Index, std::time::Instant, Option<String>)>,
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
}

#[derive(Debug, Clone, Default, Serialize)]
struct PackState {
    version: String,
    /// "menu", "joining" or "ingame".
    state: String,
    address: Option<String>,
}

struct AppState(Mutex<Inner>);

fn launcher_dir() -> PathBuf {
    settings::launcher_dir().unwrap_or_else(|| {
        std::env::current_exe().ok().and_then(|e| e.parent().map(|p| p.to_path_buf())).unwrap_or_default()
    })
}

fn data_dir() -> PathBuf {
    settings::pack_data_dir().unwrap_or_else(launcher_dir)
}

fn locate(inner: &mut Inner) {
    match win::steam_install() {
        Some((exe, path)) => {
            inner.steam_exe = Some(exe);
            inner.game = steam::find_game(&path);
            inner.steam_path = Some(path);
        }
        None => {
            inner.steam_exe = None;
            inner.steam_path = None;
            inner.game = None;
        }
    }
}

/// Puts the pack's start-up line back if Steam took it away. Only the line,
/// only when the pack's start-up stubs are installed, and never while the
/// game runs.
fn repair(inner: &mut Inner, why: &str) -> String {
    let Some(game) = inner.game.clone() else { return "Garry's Mod wasn't found in Steam's libraries.".into() };
    if !inner.allows("repair") {
        return "Repairs are switched off for now by Meowy Network.".into();
    }
    if inner.running {
        return "Garry's Mod is running; the launcher repairs when it's closed.".into();
    }
    let missing = game.missing_modules();
    let said = if missing.iter().any(|m| m.starts_with("gmsv_meowy_boot")) {
        format!("The Meowy Client Pack's files are missing ({}). Run MeowyClientPack.exe again.", missing.join(", "))
    } else {
        match line::heal(&game.garrysmod, &data_dir()) {
            line::Heal::Present => "Nothing to repair.".into(),
            line::Heal::Off => "\"Start with the game\" is off in Meowy settings, so the start-up line stays out.".into(),
            line::Heal::Added => "Put the pack's start-up line back (Steam had replaced menu.lua).".into(),
            line::Heal::Failed(why) => format!("Couldn't repair: {why}."),
        }
    };
    log::line(&format!("repair ({why}): {said}"));
    inner.last_repair = said.clone();
    said
}

/// Starts the game through Steam (repairing first), or asks the running game
/// to join `connect`.
fn launch(app: &AppHandle, connect: Option<String>) -> Result<String, String> {
    // Started as administrator (an installer run that way): Steam and the game
    // would get that token too, with Steam's path from a value any program can change.
    if win::elevated() {
        log::line("launch refused: the launcher runs as administrator");
        return Err("The Meowy Launcher is running as administrator, so it won't start Steam or the game. \
            Quit it from the tray and open it again from the Start menu."
            .into());
    }
    let state = app.state::<AppState>();
    let mut inner = state.0.lock().unwrap();
    locate(&mut inner);
    if inner.running {
        let Some(address) = connect else { return Ok("Garry's Mod is already running.".into()) };
        // The pack is connected: it joins itself (and checks the address again).
        if let (Some(tx), Some(_)) = (inner.pipe_tx.as_ref(), inner.pack.as_ref()) {
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
    let exe = inner.steam_exe.clone().ok_or("Steam wasn't found. Is it installed?")?;
    if inner.game.is_some() {
        // Never blocks the launch: a failed repair is reported, the game still starts.
        repair(&mut inner, "launch");
    }
    let faster = inner.settings.disable_lua_refresh && inner.allows("fast_start");
    let args = steam::launch_args(faster, connect.as_deref());
    let garrysmod = inner.game.as_ref().map(|g| g.garrysmod.clone());
    drop(inner);
    if let Some(garrysmod) = garrysmod.filter(|g| faster && line::present(g)) {
        lazy_fonts(garrysmod);
    }
    std::process::Command::new(&exe).args(&args).spawn().map_err(|e| format!("Couldn't start Steam: {e}"))?;
    log::line(&format!("launch: steam {}", args.join(" ")));
    let _ = app.emit("status", ());
    Ok(match connect {
        Some(a) => format!("Starting Garry's Mod and joining {a}..."),
        None => "Starting Garry's Mod...".into(),
    })
}

/// Lazy fonts (core/src/fonts.rs): the Workshop font cache goes aside for this
/// start and comes back as soon as the pack's start-up stub runs, which is
/// after the start-up font pass. Only when the start-up line is in place, so
/// the stub will run: its first file-name change in the pack's data folder
/// (recover.lock, even in safe mode) is the signal. Otherwise 45 s, the
/// game's exit or the launcher's next start puts it back.
fn lazy_fonts(garrysmod: PathBuf) {
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
    std::thread::spawn(move || {
        let start = std::time::Instant::now();
        let signal = watch.map(|w| w.wait(45_000)).unwrap_or(false);
        let why = if signal { format!("the pack started after {:.1} s", start.elapsed().as_secs_f64()) } else { "45 s".into() };
        match fonts::restore(&garrysmod) {
            Ok(n) => log::line(&format!("lazy fonts: {n} put back ({why})")),
            Err(e) => log::line(&format!("lazy fonts: couldn't put them back yet ({e}); retried at exit and next start")),
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
    app.state::<AppState>().0.lock().unwrap().card = Some(card);
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
        if !app.state::<AppState>().0.lock().unwrap().allows("links") {
            notice(app, "Link not followed", "meowy:// links are switched off for now by Meowy Network.");
            return;
        }
        match links::parse(url) {
            Ok(Link::Connect { address, ours: true }) => {
                log::line(&format!("link: join {address}"));
                if let Err(e) = launch(app, Some(address)) {
                    notice(app, "Couldn't join", &e);
                }
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
        if let Err(e) = launch(app, None) {
            notice(app, "Couldn't start Garry's Mod", &e);
        }
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

fn watch_game(app: AppHandle) {
    let mut watcher: Option<(PathBuf, win::Watcher)> = None;
    loop {
        let root = app.state::<AppState>().0.lock().unwrap().game.as_ref().map(|g| g.root.clone());
        let Some(root) = root else {
            std::thread::sleep(Duration::from_secs(5));
            continue;
        };
        if watcher.as_ref().map(|(r, _)| r != &root).unwrap_or(true) {
            watcher = Some((root.clone(), win::Watcher::new(&root)));
        }
        let (_, w) = watcher.as_mut().unwrap();
        match w.step(2000) {
            win::Event::Idle => {}
            win::Event::Started { created } => {
                log::line(&format!("game started ({}s ago)", now() - created));
                app.state::<AppState>().0.lock().unwrap().running = true;
                let _ = app.emit("status", ());
            }
            win::Event::Ended { exit_code, created } => {
                let game = {
                    let state = app.state::<AppState>();
                    let mut inner = state.0.lock().unwrap();
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
                    show_card(&app, Card::Crash { address, name, report });
                }
                let _ = app.emit("status", ());
            }
        }
    }
}

/// "Meowy Network is back up": while the setting is on and the game is
/// closed, one A2S_INFO to Meowy every 30 s; two missed answers in a row mean
/// down, and the first answer after that shows the card.
fn watch_back_up(app: AppHandle) {
    let mut tracker = servers::BackUp::default();
    loop {
        std::thread::sleep(Duration::from_secs(30));
        let (on, running) = {
            let state = app.state::<AppState>();
            let inner = state.0.lock().unwrap();
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
            show_card(&app, Card::BackUp { address: address::MEOWY.into(), name });
        }
    }
}

/// The pack's signed policy (core/src/policy.rs): re-read the kept one each
/// minute (the pack may have kept a newer one), and while the game is closed
/// fetch the same files the pack does every 30 minutes, so the start-up
/// stub's policy.bin is current before the pack would load. Only with the
/// pack's production keys (read from src/policy.cpp at build time).
fn watch_policy(app: AppHandle) {
    let keys = policy::Keys::production();
    let Some(dir) = policy::state_dir() else { return };
    let mut last_fetch: Option<std::time::Instant> = None;
    let mut last_said = String::new();
    // Let the game watcher see a game that's already running before the first fetch.
    std::thread::sleep(Duration::from_secs(10));
    if keys.any() && policy::heal_stub(&keys, &dir) {
        log::line("policy: the start-up stub's copy written again from the kept policy");
    }
    loop {
        let running = app.state::<AppState>().0.lock().unwrap().running;
        if keys.any() && !running && last_fetch.map_or(true, |t| t.elapsed() >= Duration::from_secs(30 * 60)) {
            last_fetch = Some(std::time::Instant::now());
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
        let kept = policy::current(&keys, &dir).map(|(p, _)| p);
        {
            let state = app.state::<AppState>();
            let mut inner = state.0.lock().unwrap();
            if inner.policy != kept {
                log::line(&format!("policy in force: {}", kept.as_ref().map_or("none".into(), |p| format!("seq {}", p.seq))));
                inner.policy = kept;
                drop(inner);
                let _ = app.emit("status", ());
            }
        }
        std::thread::sleep(Duration::from_secs(60));
    }
}

/// The pack's signed updater (core/src/updater.rs, the same document and
/// checks as src/updater.cpp): a minute after start, then every 6 hours, and
/// only staged while the game is closed (the pack does it while it runs).
/// The start-up stub takes a staged version at the next start.
fn watch_updates(app: AppHandle) {
    let keys = policy::Keys::production();
    std::thread::sleep(Duration::from_secs(60));
    loop {
        let (game, pol) = {
            let state = app.state::<AppState>();
            let inner = state.0.lock().unwrap();
            (inner.game.clone(), inner.policy.clone())
        };
        let said = match game {
            None => "Garry's Mod wasn't found".to_string(),
            Some(_) if !keys.any() => "no release key in this build".to_string(),
            Some(g) => {
                let now = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
                let a = app.clone();
                let running = move || a.state::<AppState>().0.lock().unwrap().running;
                let fetch = |path: &str, cap: u64| -> Result<Vec<u8>, String> {
                    let r = http::get(updater::HOST, path, None, cap as usize, 30_000)?;
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
        {
            let state = app.state::<AppState>();
            let mut inner = state.0.lock().unwrap();
            if inner.update_check != said {
                log::line(&format!("update check: {said}"));
                inner.update_check = said;
            }
        }
        let _ = app.emit("status", ());
        std::thread::sleep(Duration::from_secs(6 * 60 * 60));
    }
}

/// Every 15 s, one stat each of `appmanifest_4000.acf` and `menu.lua`. When
/// either changed and then stayed the same for a whole interval, with the
/// game closed and Steam done with it, the start-up line is checked.
fn watch_steam(app: AppHandle) {
    let mut last: Option<Vec<Option<SystemTime>>> = None;
    let mut checked: Option<Vec<Option<SystemTime>>> = None;
    loop {
        std::thread::sleep(Duration::from_secs(15));
        let state = app.state::<AppState>();
        let mut inner = state.0.lock().unwrap();
        if inner.game.is_none() {
            locate(&mut inner);
        }
        let Some(game) = inner.game.clone() else { continue };
        let stamp = |p: PathBuf| std::fs::metadata(p).and_then(|m| m.modified()).ok();
        // The player's "Start with the game" choice too, so turning it back on is seen.
        let sig = vec![
            stamp(game.manifest.clone()),
            stamp(game.garrysmod.join("lua\\includes\\menu.lua")),
            stamp(data_dir().join("startup_off.txt")),
        ];
        let stable = last.as_ref() == Some(&sig);
        last = Some(sig.clone());
        if !stable || checked.as_ref() == Some(&sig) || inner.running {
            continue;
        }
        if !game.read_manifest().map(|m| m.settled()).unwrap_or(false) {
            continue; // Steam is updating or verifying: wait until it's done.
        }
        if line::present(&game.garrysmod) {
            checked = Some(sig);
            continue;
        }
        repair(&mut inner, "Steam changed the game's files");
        // A repair that put the line back changed menu.lua: the next stable stamp
        // is checked once more (cheaply). One that didn't ("Start with the game"
        // is off, the pack's files are missing, the write failed) isn't tried
        // again until Steam or the player changes these files.
        if !line::present(&game.garrysmod) {
            checked = Some(sig);
        }
        drop(inner);
        let _ = app.emit("status", ());
    }
}

/// The pipe to the pack in the game (core/src/pipe.rs). Only the game's own
/// gmod.exe may connect; what it says shows in the window.
fn serve_pipe(app: AppHandle) {
    let (tx, rx) = std::sync::mpsc::channel::<String>();
    app.state::<AppState>().0.lock().unwrap().pipe_tx = Some(tx);
    let a = app.clone();
    let check: pipe::ClientCheck = Box::new(move |image| {
        let root = a.state::<AppState>().0.lock().unwrap().game.as_ref().map(|g| g.root.clone());
        root.is_some_and(|r| pipe::is_game_client(image, &r))
    });
    let b = app.clone();
    let result = pipe::serve(check, rx, move |msg| {
        {
            let state = b.state::<AppState>();
            let mut inner = state.0.lock().unwrap();
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
    });
    if let Err(e) = result {
        log::line(&format!("pipe: {e}"));
        app.state::<AppState>().0.lock().unwrap().pipe_tx = None;
    }
}

// --- Commands for the pages -------------------------------------------------------

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
fn status(app: AppHandle) -> Status {
    let state = app.state::<AppState>();
    let mut inner = state.0.lock().unwrap();
    locate(&mut inner);
    let manifest = inner.game.as_ref().and_then(|g| g.read_manifest());
    let line = match &inner.game {
        None => "unknown",
        Some(g) if line::present(&g.garrysmod) => "present",
        Some(_) if line::off(&data_dir()) => "off",
        Some(_) => "missing",
    };
    Status {
        version: env!("MEOWY_PACK_VERSION"),
        steam: inner.steam_exe.is_some(),
        game_found: inner.game.is_some(),
        garrysmod: inner.game.as_ref().map(|g| g.garrysmod.display().to_string()).unwrap_or_default(),
        build_id: manifest.as_ref().map(|m| m.build_id.clone()).unwrap_or_default(),
        steam_busy: manifest.as_ref().map(|m| !m.settled()).unwrap_or(false),
        line,
        missing_modules: inner.game.as_ref().map(|g| g.missing_modules()).unwrap_or_default(),
        running: inner.running,
        disable_lua_refresh: inner.settings.disable_lua_refresh,
        notify_back_up: inner.settings.notify_back_up,
        start_at_sign_in: win::start_at_sign_in(),
        last_repair: inner.last_repair.clone(),
        pack: inner.pack.clone(),
        updates: {
            let game = inner.game.as_ref();
            let arch = game.map_or("win64", |g| g.arch());
            updater::status(&data_dir(), game.map(|g| g.garrysmod.as_path()), arch, inner.policy.as_ref(), &inner.update_check)
        },
        notice: inner.policy.as_ref().map(|p| p.notice.clone()).unwrap_or_default(),
    }
}

#[tauri::command]
async fn play(app: AppHandle, join_meowy: bool) -> Result<String, String> {
    launch(&app, join_meowy.then(|| address::MEOWY.to_string()))
}

#[tauri::command]
fn repair_now(app: AppHandle) -> String {
    let state = app.state::<AppState>();
    let mut inner = state.0.lock().unwrap();
    locate(&mut inner);
    repair(&mut inner, "asked")
}

#[tauri::command]
fn set_lua_refresh(app: AppHandle, on: bool) -> Result<(), String> {
    let state = app.state::<AppState>();
    let mut inner = state.0.lock().unwrap();
    inner.settings.disable_lua_refresh = on;
    log::line(&format!("setting: disable_lua_refresh = {on}"));
    inner.settings.save(&launcher_dir()).map_err(|e| e.to_string())
}

#[tauri::command]
fn set_back_up(app: AppHandle, on: bool) -> Result<(), String> {
    let state = app.state::<AppState>();
    let mut inner = state.0.lock().unwrap();
    inner.settings.notify_back_up = on;
    log::line(&format!("setting: notify_back_up = {on}"));
    inner.settings.save(&launcher_dir()).map_err(|e| e.to_string())
}

/// Meowy Network, the player's Steam favourites and recent Garry's Mod
/// servers, each asked over A2S (takes up to ~3 s).
#[tauri::command]
async fn server_list(app: AppHandle) -> Vec<servers::Row> {
    let steam_path = {
        let state = app.state::<AppState>();
        let mut inner = state.0.lock().unwrap();
        if !inner.allows("servers") {
            return Vec::new();
        }
        if inner.steam_path.is_none() {
            locate(&mut inner);
        }
        inner.steam_path.clone()
    };
    let rows = tauri::async_runtime::spawn_blocking(move || {
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
    app.state::<AppState>().0.lock().unwrap().servers = rows.clone();
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
/// a minute, with If-None-Match; it's only fetched while the window is used).
#[tauri::command]
async fn find_servers(app: AppHandle, query: String) -> Result<Found, String> {
    if !app.state::<AppState>().0.lock().unwrap().allows("servers") {
        return Err("Finding servers is switched off for now by Meowy Network.".into());
    }
    let (stale, etag) = {
        let state = app.state::<AppState>();
        let inner = state.0.lock().unwrap();
        match &inner.index {
            Some((_, at, etag)) => (at.elapsed() > Duration::from_secs(60), etag.clone()),
            None => (true, None),
        }
    };
    if stale {
        let fetched = tauri::async_runtime::spawn_blocking(move || -> Result<Option<(index::Index, Option<String>)>, String> {
            let r = http::get(index::HOST, index::PATH, etag.as_deref(), index::MAX_BYTES, 15000)?;
            match r.status {
                304 => Ok(None),
                200 => Ok(Some((index::parse(&r.body)?, r.etag))),
                s => Err(format!("the server index answered {s}")),
            }
        })
        .await
        .map_err(|e| e.to_string())?;
        let state = app.state::<AppState>();
        let mut inner = state.0.lock().unwrap();
        match fetched {
            Ok(Some((idx, etag))) => inner.index = Some((idx, std::time::Instant::now(), etag)),
            Ok(None) => {
                if let Some(entry) = inner.index.as_mut() {
                    entry.1 = std::time::Instant::now();
                }
            }
            // Keep showing an older copy if there is one.
            Err(e) if inner.index.is_none() => return Err(format!("Couldn't get the server list: {e}")),
            Err(e) => log::line(&format!("index: {e}; showing the older copy")),
        }
    }
    let state = app.state::<AppState>();
    let inner = state.0.lock().unwrap();
    let (idx, _, _) = inner.index.as_ref().ok_or("No server list yet.")?;
    let query: String = query.chars().take(100).collect();
    Ok(Found { at: idx.at, total: idx.servers.len(), servers: index::search(idx, &query, 25).into_iter().cloned().collect() })
}

/// Joins a server from the list or the index last shown (the page can't name any other).
#[tauri::command]
async fn join_server(app: AppHandle, address: String) -> Result<String, String> {
    let listed = {
        let state = app.state::<AppState>();
        let inner = state.0.lock().unwrap();
        inner.servers.iter().any(|r| r.address == address)
            || inner.index.as_ref().is_some_and(|(idx, _, _)| idx.servers.iter().any(|s| s.address == address))
    };
    if !listed {
        return Err("That server isn't in the list.".into());
    }
    log::line(&format!("window: join {address}"));
    launch(&app, Some(address))
}

#[tauri::command]
fn set_sign_in(on: bool) -> Result<(), String> {
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    log::line(&format!("setting: start at sign-in = {on}"));
    if win::set_start_at_sign_in(&exe, on) { Ok(()) } else { Err("Windows didn't take the change.".into()) }
}

#[tauri::command]
fn card(app: AppHandle) -> Option<Card> {
    app.state::<AppState>().0.lock().unwrap().card.clone()
}

/// The card's buttons. The address always comes from the launcher's own
/// state, never from the page.
#[tauri::command]
async fn card_answer(app: AppHandle, window: WebviewWindow, action: String) -> Result<String, String> {
    let card = app.state::<AppState>().0.lock().unwrap().card.clone();
    let close = |app: &AppHandle| {
        app.state::<AppState>().0.lock().unwrap().card = None;
        let _ = window.destroy();
    };
    match (action.as_str(), card) {
        ("join", Some(Card::Crash { address, .. }))
        | ("join", Some(Card::Confirm { address }))
        | ("join", Some(Card::BackUp { address, .. })) => {
            log::line(&format!("card: join {address}"));
            close(&app);
            launch(&app, Some(address))
        }
        ("details", _) => {
            let root = app.state::<AppState>().0.lock().unwrap().game.as_ref().map(|g| g.root.join("crashes"));
            match root {
                Some(dir) if dir.is_dir() && win::shell_open(&dir.display().to_string()) => Ok(String::new()),
                _ => Err("No crash reports folder.".into()),
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
        .on_menu_event(|app, event| match event.id().as_ref() {
            "open" => show_main(app),
            "play" | "join" => {
                let connect = (event.id().as_ref() == "join").then(|| address::MEOWY.to_string());
                if let Err(e) = launch(app, connect) {
                    notice(app, "Couldn't start Garry's Mod", &e);
                }
            }
            "repair" => {
                let state = app.state::<AppState>();
                let said = {
                    let mut inner = state.0.lock().unwrap();
                    locate(&mut inner);
                    repair(&mut inner, "tray")
                };
                notice(app, "Repair", &said);
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

fn main() {
    let dir = launcher_dir();
    log::init(&dir);
    let settings = Settings::load(&dir);
    let mut inner = Inner {
        steam_exe: None,
        steam_path: None,
        game: None,
        settings,
        running: false,
        last_repair: String::new(),
        card: None,
        servers: Vec::new(),
        index: None,
        pipe_tx: None,
        pack: None,
        policy: None,
        update_check: "not checked yet".into(),
    };
    locate(&mut inner);
    // Links aren't logged as given: a refused one may carry a password.
    let args: Vec<String> =
        std::env::args().skip(1).map(|a| if links::is_link(&a) { "<link>".into() } else { a }).collect();
    log::line(&format!(
        "start {} ({:?}); game: {}",
        env!("MEOWY_PACK_VERSION"),
        args,
        inner.game.as_ref().map(|g| g.garrysmod.display().to_string()).unwrap_or_else(|| "not found".into())
    ));

    let app = tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, args, _cwd| handle_args(app, &args)))
        .manage(AppState(Mutex::new(inner)))
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
        .setup(|app| {
            let handle = app.handle().clone();
            tray(&handle)?;
            keep_protocol();
            {
                let state = handle.state::<AppState>();
                let mut inner = state.0.lock().unwrap();
                repair(&mut inner, "start");
                if !inner.running {
                    restore_fonts(inner.game.as_ref(), "launcher start");
                }
            }
            let h = handle.clone();
            std::thread::spawn(move || watch_game(h));
            let h = handle.clone();
            std::thread::spawn(move || serve_pipe(h));
            let h = handle.clone();
            std::thread::spawn(move || watch_steam(h));
            let h = handle.clone();
            std::thread::spawn(move || watch_back_up(h));
            let h = handle.clone();
            std::thread::spawn(move || watch_policy(h));
            let h = handle.clone();
            std::thread::spawn(move || watch_updates(h));
            handle_args(&handle, &std::env::args().collect::<Vec<_>>());
            Ok(())
        })
        .build(tauri::generate_context!())
        .expect("the launcher's window system");

    app.run(|app, event| match event {
        // Closing the last window keeps the launcher in the tray; only Quit exits.
        RunEvent::ExitRequested { code: None, api, .. } => api.prevent_exit(),
        // Quit (the tray, --quit, the installer): the Workshop fonts back first,
        // or nothing would put them back until the launcher's next start.
        RunEvent::Exit => {
            let game = app.state::<AppState>().0.lock().ok().and_then(|inner| inner.game.clone());
            restore_fonts(game.as_ref(), "the launcher quit");
        }
        _ => {}
    });
}
