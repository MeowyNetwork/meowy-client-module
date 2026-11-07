# Meowy Launcher: design (roadmap MR1)

A small Windows app that runs **outside** Garry's Mod. It launches the game through Steam, repairs the
pack after Steam undoes it, offers Rejoin after a crash, and turns `meowy://` links into joins. It never
opens the game's memory.

Research: `GMODEnhanced\review_2026-11-06\major-reworks.md` §MR1, `startup-menu-speed.md` #1, #8–#10.

## 1. Decisions (agreed 2026-11-07)

| Question | Choice |
|---|---|
| Where it lives | **Always in the tray**, started at Windows sign-in (installer checkbox, on by default; a tray toggle) |
| Stack | **Tauri 2** (Rust core + the OS WebView2) |
| Early entry point (start-up #9) | **Later opt-in, designed now** (§9). Not in the MVP |
| `-disableluarefresh` | **On, with an opt-out** in settings |
| `meowy://` links | **Any public server; Meowy's join directly, others ask first.** No passwords, launch options or commands |
| Crash card | **A small launcher window** (not a toast: Do Not Disturb can't hide it) |
| Install | **Inside `MeowyClientPack.exe`**: one download, as today |
| Updates | **The pack's signed channel** (main session, K1/U1). No second channel; a marked stub until it lands (§8) |

## 2. Shape

```
MeowyLauncher.exe  (x64, per user, no admin; %LOCALAPPDATA%\MeowyClientPack\launcher\)
 ├─ tray      Play · Join Meowy Network · Repair now · Settings · Quit
 ├─ steam     find Steam, the library holding app 4000, buildid, StateFlags; launch via -applaunch
 ├─ repair    menu.lua start-up line (same text and rules as src/startup_hook.cpp), the pack's DLLs
 ├─ watch     game processes (SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION only), exit code
 ├─ session   on_server.txt + GarrysMod\crashes\ → "crashed on <server>" → card
 ├─ links     meowy://connect/<a.b.c.d>:<port> → validate → (confirm) → join
 ├─ updater   STUB until the pack's signed release format lands (§8)
 └─ windows   main (status, settings) and card (crash); both destroyed when closed
```

- **One instance.** A second start (a clicked link, the Start-menu shortcut) hands its arguments to the
  running one and exits (`tauri-plugin-single-instance`).
- **Light at rest.** Windows are destroyed on close, not hidden, so WebView2's processes exit; only the
  Rust core and the tray stay (target ≤20 MB, measured in the PR). The app keeps running with no window.
- **WebView2 profile** in `%LOCALAPPDATA%\MeowyClientPack\launcher\webview`, so uninstall removes it.
- **UI** is static HTML/JS bundled into the exe (no bundler, no remote pages, strict CSP). Pages call a
  handful of typed Rust commands; nothing else is exposed.
- **Name.** "Meowy Launcher". Never "Garry's Mod" as the product name or icon (Facepunch's mark).

## 3. Launching GMod through Steam

- **Command:** `steam.exe -applaunch 4000 [-disableluarefresh] [+connect <a.b.c.d:port>]`, as an argument
  array (no shell). Steam appends these to the player's own launch options. `steam.exe` comes from
  `HKCU\Software\Valve\Steam\SteamExe`; if Steam isn't running it starts first, which is slower but works.
- **The library:** `SteamPath` + `steamapps\libraryfolders.vdf` → the one with `appmanifest_4000.acf`
  (the installer's `FindGarrysMod`, ported).
- **Before every launch:** repair (§4). If repair can't fix something, the launch still goes ahead: the
  game must never be blocked by the launcher.
- **Verified 2026-11-07:** `-applaunch` passes our arguments straight to `bin\win64\gmod.exe`, before the
  player's own options, with no Steam dialog (game process 0.5–2 s after the call).
- **GMod already running** and a join is asked for: `steam://connect/<address>`. Verified: Steam passes it
  to the running game, which joins.
- **Launch flags:** `-disableluarefresh` (measured 0.8–1.6 s per start, start-up #1). Opt-out in settings.
  No other flags in the MVP. `-nojoy` is left out: it would break controllers.

## 4. Repair

What Steam can undo, and what the launcher does about it:

| Steam event | Effect on the pack | Launcher |
|---|---|---|
| GMod update (`buildid` changes) | stock `lua\includes\menu.lua` back, start-up line gone | heals the line once `StateFlags` says the update finished |
| Verify integrity of files | same, but `buildid` doesn't change | sees `menu.lua` change, heals it |
| Reinstall GMod | `lua\bin` DLLs gone too | says so, offers the installer (§8 makes this a re-download) |

- **The line** is the pack's (`src/startup_hook.cpp` `kLine`/`kMarker`), with the same rules: append CRLF,
  replace through a temp file, never write while the player has "Start with the game" off
  (`startup_off.txt`). A Rust test reads `startup_hook.cpp` and fails if the two texts drift.
- **When:** at launcher start, before every launch, and when Steam's files say it's needed:
  - a 15 s poll of two files' metadata (`appmanifest_4000.acf`, `lua\includes\menu.lua`): a stat each,
    no reads unless one changed;
  - only when GMod isn't running and `StateFlags` = 4 (fully installed: no update or verify running), and
    both files are unchanged across two polls (15 s).
- **Never** while the game runs, and never anything outside the pack's line and the pack's own files.
- The pack keeps healing the line itself when it loads from our server (unchanged), so a player without
  the launcher is no worse off.

## 5. Crash recovery

**Watching.** Every 2 s while no game runs, one process snapshot looks for `gmod.exe`/`hl2.exe` whose image
is inside the GarrysMod folder (`QueryFullProcessImageNameW` with `PROCESS_QUERY_LIMITED_INFORMATION`).
Each is opened with `SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION` and waited on. Steam's 32-bit root
`gmod.exe` exits after starting `bin\win64\gmod.exe`, so a session ends when the last one exits, and that
one's exit code counts. GMod's own Chromium helpers are `bin\win64\gmod.exe` too (`--type=renderer`, ...);
they're recognised without reading their command line (that would need memory access): a process whose
parent is a followed process with the same image isn't followed. No other rights are ever asked for: no
memory read or write, no thread, no injection.

**Deciding it crashed.** The pack already writes `%LOCALAPPDATA%\MeowyClientPack\on_server.txt`
(`<address> <unix time>\r\n<name>\r\n`) while connected to our server or a public one, and leaves it when
the game ends on a server. When the session ends and that file is there:

| Crash report in `GarrysMod\crashes\` since the file's time | Exit code | Verdict |
|---|---|---|
| yes | any | crashed: card |
| no | not 0 (killed, hung and ended in Task Manager) | crashed: card |
| no | 0 | quit on purpose: nothing |

*Measured 2026-11-07:* a normal quit (`quit`) exits with 0; ending the game in Task Manager while on our
server exits with `0xFFFFFFFF`, writes no crash report, and leaves `on_server.txt`. So the pack alone
(which needs a crash report with a Lua section) doesn't offer Rejoin after a kill or a hang; the launcher does.

**The card** (small window, bottom-right, on top): "Garry's Mod closed unexpectedly while you were on
<name> (<address>). **[Rejoin] [Details] [Not now]**".
- **Rejoin** launches with `+connect <address>` (§3). Details opens the crash report folder.
- The launcher **consumes** `on_server.txt` when it shows the card, so the pack doesn't ask again at the
  next start. Without the launcher, the pack's own prompt works as today.
- The address is checked again (Meowy's, or public per `server_rep::IsPublicAddress`) before anything
  is offered. Never a password.

## 6. `meowy://` deep links

- **Registered** by the installer under `HKCU\Software\Classes\meowy` (no admin), pointing at
  `MeowyLauncher.exe "%1"`; the launcher re-registers it at start if it's missing or points elsewhere.
- **Grammar (MVP):** `meowy://connect/<a.b.c.d>:<port>[/]`, nothing else. Anything with a password,
  query, fragment, user info, host name, IPv6 or extra path is refused and shown as refused.
- **Public IPv4 only**, the pack's rule (no 0/8, 10/8, 127/8, 169.254/16, 172.16/12, 192.168/16,
  100.64/10, ≥224).
- **Meowy's servers** (`138.201.62.52:27015`, the list in `src/server_gate.cpp`) join directly. Any other
  server shows "Join <address>?" in the launcher window first.
- Later: `meowy://play` (open the game) and `meowy://server/<id>` once MR3's index has IDs.

## 7. Lifecycle and install

- **Install** (`MeowyClientPack.exe`, which embeds `MeowyLauncher.exe`):
  - `%LOCALAPPDATA%\MeowyClientPack\launcher\MeowyLauncher.exe`;
  - `meowy://` under `HKCU\Software\Classes`;
  - "Start the Meowy Launcher when I sign in" (checked): `HKCU\...\Run\MeowyLauncher` = `"<exe>" --tray`;
  - the "Meowy Client Pack" Start-menu shortcut now starts the launcher (it keeps its AppUserModelID, so
    the pack's toasts are unchanged). Opening it shows the main window and the launcher stays in the tray;
  - starts the launcher in the tray when done.
- **Uninstall** asks the running launcher to quit (`--quit`, handed over by single-instance), then removes
  the Run value, the `meowy` key, the folder and the shortcut, as it removes the pack's other files.
- **Settings** (`launcher\settings.json`): `disable_lua_refresh` (true), `start_at_sign_in` (mirrors the
  Run value). That's all in the MVP.

## 8. Updates: the pack's signed channel, not a new one

The main session is building the signed update format and the remote kill switch inside the pack (U1/K1:
versioned folders plus a forwarder; an ops key that can only turn things off). The launcher will use that
format and keys as they are:

- **Fetch** the same signed release and policy files, outside the game, and verify them with the same
  public key (BCrypt ECDSA P-256 / SHA-256, as `notify_catalogue` does) before anything is written.
- **Stage** a new version where the pack's own updater would (`bin\<ver>\` + `pending.json`); the stub's
  verify/promote/rollback stays the only code that switches versions. The in-game download then becomes
  the fallback for players without the launcher.
- **The launcher itself** is one more entry in the same release, swapped while no window is open.
- **Kill switch:** the launcher reads the same policy and obeys its off-switches for its own features too
  (e.g. stop healing the line, stop adding `-disableluarefresh`).
- **Repair** of missing DLLs (§4, reinstall) then re-stages the current verified version.

**Until that lands** `updater.rs` is a stub marked `UPDATER-STUB`: it reports "updates come with the
pack" and does no network I/O. Its interface (check → stage → report) is fixed so the real one drops in.
Before writing update code, check the main session's PR in the repo.

## 9. Early entry point (start-up #9): a later opt-in, designed now

The roadmap's ~10 s → ~6 s needs patches active **before** the engine initialises (lazy fonts, a persistent
negative file-index cache). That requires code in `gmod.exe` from its first instruction. It's the one thing
here that would touch the game, so it's kept apart:

- **Tier C ("Experimental"), off by default**, its own toggle with a plain-words warning; the kill switch
  can turn it off remotely.
- **How:** Steam must start the launcher instead of the game, so the launcher can create `gmod.exe`
  suspended: the `%command%` launch-option wrapper (start-up #8), verified on Windows first. The launcher
  then loads a small early stub (remote `LoadLibraryW`) and resumes. Any failure falls back to a plain,
  unpatched launch.
- **Gate:** built only if a prototype measures ≥2 s off click → menu on the probe (`StartupProbe`, n≥4,
  unsampled), and ships only behind the toggle.
- **Lazy Workshop mounting** (start-up #10) needs the same persisted index; it comes after #9 or not at all.
- Nothing in the MVP depends on this, and nothing in the MVP opens the game with more than
  `SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION`.

## 10. Later (v2)

- A pipe `\\.\pipe\meowy-<user SID>` (typed JSON, size caps; never Lua or commands): join while the game
  runs, status, notifications.
- "Server back up" / "event starting" notifications while GMod is closed.
- MR3 discovery and favourites with live A2S; MR2 content preparation while GMod is closed.
- "Still loading?" for a hung window (`IsHungAppWindow`).

## 11. Measured claims

Every speed claim in the PR comes from runs on this PC, `-condebug`, game closed between runs, n≥4 each:

1. **Launcher overhead:** click → `[Meowy Client Pack]` console line, Steam `-applaunch` direct vs the
   launcher's Play with the flag off. Expected: no difference beyond noise (±0.8 s).
2. **`-disableluarefresh`:** launcher's Play with the flag on vs off.
3. **Memory at rest:** tray only, no window (private bytes).

The ~10 s → ~6 s figure is **not** claimed: it belongs to §9.

## 12. Tests

- **Rust unit tests** (`cargo test`, in CI on `windows-latest`):
  - the start-up line rules (the cases of `tests/startup_hook_smoke.cpp`), and the drift check against
    `src/startup_hook.cpp`;
  - `libraryfolders.vdf` / `appmanifest_4000.acf` parsing (buildid, StateFlags, installdir);
  - deep-link grammar and the public-address rule;
  - `on_server.txt` parsing and the crash verdict table;
  - the launch argument list;
  - repair on a temporary `garrysmod` folder (heal, off, already present, read-only file).
- **Manual, on this PC** (results in the PR): Steam Verify then the tray heals the line; kill `gmod.exe`
  on a server → card → Rejoin joins; a link with GMod closed and with it open; the measurements in §11.
