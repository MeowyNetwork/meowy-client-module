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
use launcher_core::{address, line, links, log, session, steam, updater, win};
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
}

struct Inner {
    steam_exe: Option<PathBuf>,
    game: Option<steam::Game>,
    settings: Settings,
    running: bool,
    last_repair: String,
    card: Option<Card>,
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
        }
        None => {
            inner.steam_exe = None;
            inner.game = None;
        }
    }
}

/// Puts the pack's start-up line back if Steam took it away. Only the line,
/// only when the pack's start-up stubs are installed, and never while the
/// game runs.
fn repair(inner: &mut Inner, why: &str) -> String {
    let Some(game) = inner.game.clone() else { return "Garry's Mod wasn't found in Steam's libraries.".into() };
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
    let state = app.state::<AppState>();
    let mut inner = state.0.lock().unwrap();
    locate(&mut inner);
    if inner.running {
        let Some(address) = connect else { return Ok("Garry's Mod is already running.".into()) };
        let url = steam::connect_url(&address).ok_or("That isn't a public server address.")?;
        log::line(&format!("join in the running game: {address}"));
        return if win::shell_open(&url) { Ok(format!("Joining {address}...")) } else { Err("Steam didn't take the join.".into()) };
    }
    let exe = inner.steam_exe.clone().ok_or("Steam wasn't found. Is it installed?")?;
    if inner.game.is_some() {
        // Never blocks the launch: a failed repair is reported, the game still starts.
        repair(&mut inner, "launch");
    }
    let args = steam::launch_args(inner.settings.disable_lua_refresh, connect.as_deref());
    drop(inner);
    std::process::Command::new(&exe).args(&args).spawn().map_err(|e| format!("Couldn't start Steam: {e}"))?;
    log::line(&format!("launch: steam {}", args.join(" ")));
    let _ = app.emit("status", ());
    Ok(match connect {
        Some(a) => format!("Starting Garry's Mod and joining {a}..."),
        None => "Starting Garry's Mod...".into(),
    })
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
        .inner_size(440.0, 600.0)
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
                app.state::<AppState>().0.lock().unwrap().running = false;
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
        let sig = vec![stamp(game.manifest.clone()), stamp(game.garrysmod.join("lua\\includes\\menu.lua"))];
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
        drop(inner);
        // The repair itself changed menu.lua: the next stable stamp is checked once more (cheaply).
        let _ = app.emit("status", ());
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
    start_at_sign_in: bool,
    last_repair: String,
    updates: &'static str,
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
        version: env!("CARGO_PKG_VERSION"),
        steam: inner.steam_exe.is_some(),
        game_found: inner.game.is_some(),
        garrysmod: inner.game.as_ref().map(|g| g.garrysmod.display().to_string()).unwrap_or_default(),
        build_id: manifest.as_ref().map(|m| m.build_id.clone()).unwrap_or_default(),
        steam_busy: manifest.as_ref().map(|m| !m.settled()).unwrap_or(false),
        line,
        missing_modules: inner.game.as_ref().map(|g| g.missing_modules()).unwrap_or_default(),
        running: inner.running,
        disable_lua_refresh: inner.settings.disable_lua_refresh,
        start_at_sign_in: win::start_at_sign_in(),
        last_repair: inner.last_repair.clone(),
        updates: updater::status(),
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
        ("join", Some(Card::Crash { address, .. })) | ("join", Some(Card::Confirm { address })) => {
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
    let mut inner = Inner { steam_exe: None, game: None, settings, running: false, last_repair: String::new(), card: None };
    locate(&mut inner);
    // Links aren't logged as given: a refused one may carry a password.
    let args: Vec<String> =
        std::env::args().skip(1).map(|a| if links::is_link(&a) { "<link>".into() } else { a }).collect();
    log::line(&format!(
        "start {} ({:?}); game: {}",
        env!("CARGO_PKG_VERSION"),
        args,
        inner.game.as_ref().map(|g| g.garrysmod.display().to_string()).unwrap_or_else(|| "not found".into())
    ));

    let app = tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, args, _cwd| handle_args(app, &args)))
        .manage(AppState(Mutex::new(inner)))
        .invoke_handler(tauri::generate_handler![status, play, repair_now, set_lua_refresh, set_sign_in, card, card_answer])
        .setup(|app| {
            let handle = app.handle().clone();
            tray(&handle)?;
            keep_protocol();
            {
                let state = handle.state::<AppState>();
                let mut inner = state.0.lock().unwrap();
                repair(&mut inner, "start");
            }
            let h = handle.clone();
            std::thread::spawn(move || watch_game(h));
            let h = handle.clone();
            std::thread::spawn(move || watch_steam(h));
            handle_args(&handle, &std::env::args().collect::<Vec<_>>());
            Ok(())
        })
        .build(tauri::generate_context!())
        .expect("the launcher's window system");

    app.run(|_app, event| {
        // Closing the last window keeps the launcher in the tray; only Quit exits.
        if let RunEvent::ExitRequested { code: None, api, .. } = event {
            api.prevent_exit();
        }
    });
}
