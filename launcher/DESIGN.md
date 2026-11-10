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
  - only when GMod isn't running and `StateFlags` = 4 (fully installed), and both files are unchanged
    across two polls (15 s).
  - *Measured 2026-11-07:* during a Verify, `StateFlags` in the `.acf` and Steam's
    `HKCU\...\Steam\Apps\4000\Updating` both stayed at "installed, not updating", so neither shows a
    Verify. Steam's validation took 21 s and *truncated* the line off `menu.lua` 6 s in; the launcher put
    it back 60 s in, after Steam's content log said validation had finished. A Verify checks each file
    once, so a heal after Steam has rewritten `menu.lua` isn't undone by the same Verify.
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

**The policy (built, 2026-11-07, after the pack's K1 merged):** `core/src/policy.rs` mirrors
`src/policy.cpp`: the same strict parse, the same keys (`core/build.rs` reads them out of `policy.cpp`, so
there's one copy; a test checks a policy signed with the production release key), BCrypt verification,
the same files in `state\` and the same rules for keeping one (higher `seq`; ops and recovery only
stricter). The launcher re-reads the kept policy every minute and, while the game is closed, fetches the
same `policy.txt`/`.sig` every 30 minutes, so the stub's `policy.bin` is current before the pack would load.
It obeys `off launcher` (all of its own features) and `off launcher_repair`, `launcher_fast_start`,
`launcher_links`, `launcher_servers`; with `pack_enabled 0` it doesn't repair or prepare the pack's start.
The policy's notice shows in the window. (2026-11-07 the URLs still return the website's page, so nothing
is kept yet.)

**The updater (built 2026-11-07, after the pack's U1 merged as #65):** `core/src/release.rs` and
`core/src/updater.rs` mirror `src/updater.cpp`, agreed with the main session:
- the same document (`/pack/v1/release-stable.txt` + `.sig`), taken only if signed by the **release** key;
- the same `ShouldTake` checks, in the same order and words: the `release_seq_<arch>.txt` floor (one per
  architecture since 1.7.1; the same document is taken again only when this architecture's staged copy is
  missing or doesn't match), `expires`, newer
  than what runs, not in `bad_<arch>.txt`, the policy's `VersionAllowed`, the stub version, the rollout slot
  (SHA-256 of `install_token` + salt, the token made the same way and never sent), a file for this arch;
- the same staging: exactly `size` bytes from meowynetwork.com without redirects, into
  `bin\<version>.partial\`, SHA-256 checked, this architecture's DLL moved into `bin\<version>` (the other
  architecture's file there stays), then `pending_<arch>.txt` written atomically, then
  `release_seq_<arch>.txt`; folders no pointer of either architecture names cleaned up. `current`,
  `previous` and `bad` are the stub's: the launcher never writes them;
- only while the game is closed (the pack stages while it runs), a minute after start and every 6 hours;
  the architecture the player runs (`bin\win64\gmod.exe` present → win64).

From outside the game two facts need care:
- **The running version:** the stub's choice (current, then previous, if hash, bad list and policy allow),
  else the installed copy in `lua\bin`, by its own version resource (FileVersion from the pack's
  VERSIONINFO, `src/version.h`, the pack's #68; read with `GetFileVersionInfoW`/`VerQueryValueW`). A copy
  without one (installed before #68) is "not known": nothing is staged, and the pack still updates itself in
  the game. The DLL carries its own version, so no file next to it can go stale after a Steam check, a
  manual copy or an install. A wrong guess could make the stub prefer an older update over a newer install,
  so "not known" never stages.
- **The stub's version:** 1 if `gmsv_meowy_boot_<arch>.dll`'s export table (read from the file, never
  loaded) names `meowy_stub_version`, else 0 (an old stub that can't load updates).

The window's Updates panel shows the running version, a staged one ("starts next time"), previous, bad,
and the last check's reason with the release seq.

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

**Update 2026-11-07: half of #9 needs no early entry.**
- *Measured:* with `garrysmod\cache\workshop\resource\fonts` (344 stale extracted fonts, 117 MB) moved
  aside, game → pack start-up was **1.23 s faster** (median of 5 interleaved pairs, −0.5 to −3.3 s) and
  much steadier (3.8–4.1 s vs 4.6–7.4 s).
- *Lazy fonts by folder swap (agreed, `core/src/fonts.rs`):* for a launch the launcher starts with Faster
  start on and the start-up line in place, the folder is renamed aside and put back as soon as the pack's
  stub makes its first file-name change in `%LOCALAPPDATA%\MeowyClientPack` (it always writes
  `recover.lock` first, even in safe mode, and runs after the start-up font pass). Fallbacks: 45 s, the
  game's exit, the launcher's next start. GMod registers those fonts again at every map load, so servers
  still get them. File operations only: no game memory, never a link.
- *What's left for injection:* a missing-file cache from the first instruction, estimated at 0.5–1.5 s,
  under the 2 s gate. Agreed to still prototype it (direct, suspended launch as a measurement harness)
  and decide on the measurement.

**Live, 2026-11-08 (this PC):**
- *Lazy fonts:* Play from the tray set the folder aside; the game process appeared at 3.8 s and the fonts
  were back at 7.7 s on the stub's signal ("the pack started after 8.7 s"), not the 45 s fallback. After a
  join to dev, `meowy_fast_fonts_status` reported "last font pass 424 files", so servers get the fonts.
- *Start-up* (`measure_startup.ps1`, 5 rounds, medians): game → pack 3.67 s with Faster start on, 5.11 s
  off, 5.15 s from Steam; click → pack 5.47 s vs 7.16 s from Steam (about 1.7 s, −24%). Same-round pairs,
  on minus off: −1.34 s median.
- *Injection (branch `proto/early-entry`):* −0.86 s more (median of 4 pairs) on top of lazy fonts and
  `-disableluarefresh`, and 1 of 5 early starts never reached the menu. **Shelved**; results in that branch's
  `launcher/tools/early_entry_results.md`.

## 10. v2

Agreed order (2026-11-07): servers while GMod is closed → discovery from the index → the pipe. Then the
signed policy (after the pack's K1 lands) and the early-entry prototype (§9).

**Servers while GMod is closed (done):**
- The window lists Meowy Network first, then the signed-in Steam account's favourites and its 12 most
  recent servers, from Steam's own `userdata\<ActiveUser>\7\remote\serverbrowser_hist.vdf`.
- That file is shared by every Source game (entries say `appid 0`), so each server is asked over A2S
  (`core/src/a2s.rs`, the pack's rules: public addresses only, two challenge rounds, one resend, a
  bounded reply). A recent server shows only if it answers as Garry's Mod (app 4000); Meowy and
  favourites show even when not answering.
- Asked when the window opens, on Refresh and every 30 s while it's visible (at most 32 servers, ~3 s).
  **Join** only accepts an address from the list the launcher itself last returned. A server with a
  password has no Join (the launcher never handles passwords).
- **"Tell me when Meowy Network is back up"** (off by default): while the game is closed, one A2S to
  Meowy every 30 s; two misses in a row mean down; the first answer after that shows a card with
  **Join**.

**Find servers, from the index (done):**
- `GET https://meowynetwork.com/api/pack/index` (MR3, the website backend): every GMod server Steam lists,
  rebuilt each minute, with the honest list's verdicts. Fetched through WinHTTP (the OS's TLS and proxy,
  no redirects, gzip, a 16 MB cap) only while the window is used, at most once a minute, with
  `If-None-Match` (a 304 keeps the copy in memory). Measured: 837 KB in 0.18 s, 5,834 servers.
- Columns are read by name (`a n d m p x b pw h r l`), so the backend may add or reorder them; `v` must
  be 1. Text is cleaned and capped; only public addresses are kept.
- Search as you type over name, gamemode, map and address; Meowy first, then by people (Steam's count,
  which already leaves bots out, or a fake's real count), password servers last; 25 shown.
- Rows show the honest list's verdict ("FAKE: 128 shown", "verified"). **Join** accepts an address only
  from the index or the server list the launcher itself holds.

**The pipe (built; live check pending):**
- `\\.\pipe\meowy-launcher-<SID>`, made by the launcher first (`FILE_FLAG_FIRST_PIPE_INSTANCE`), with a DACL
  for the current user only, local clients only. The launcher keeps a client only if it's
  `gmod.exe`/`hl2.exe` in the game folder (query-only rights); the pack keeps the connection only if the
  pipe's server is the installed `MeowyLauncher.exe`.
- Lines of JSON, at most 512 bytes. Pack → launcher: `hello` (its version), `state` (menu, joining,
  ingame) with the engine's own address from `server_gate`, never one from Lua. Launcher → pack: exactly
  `{"t":"join","a":"a.b.c.d:port"}`, ours or public, which the menu Lua runs as `connect`. Nothing else is
  taken: no Lua, no commands, no passwords.
- Each end does all its I/O on one thread (`PeekNamedPipe` before reads), so a read never blocks a write.
  While the launcher isn't there the pack looks again after 5 s, then 10, 20, 40 and every 60 s; once a
  connection ends it starts again from 5 s.
- Used for "In the game" in the window, and for joins while the game runs (falling back to
  `steam://connect` when the pack isn't connected).

**Later:**
- Notifications from the pack through the pipe; "event starting" notifications; MR2 content preparation while GMod is closed.
- "Still loading?" for a hung window (`IsHungAppWindow`).

## 11. Measured claims

Every speed claim in the PR comes from runs on this PC (`launcher/tools/measure_startup.ps1`), game
closed between runs, the three modes interleaved, n≥4 each, medians:

1. **Launcher overhead:** Steam `-applaunch` direct vs the launcher's Play (`--play`, the tray instance
   doing the launch) with the flag off. Expected: no difference beyond noise (±0.8 s).
2. **`-disableluarefresh`:** launcher's Play with the flag on vs off.
3. **Memory at rest:** tray only, no window (private bytes).

The marker is the pack's menu Lua running: it removes `on_server.txt` once per start, so the script plants
one the pack can't parse (no prompt) and times its removal. No `-condebug` is needed, so the launcher's
own command line is measured unchanged. Steam's hop (click → game process) varies by seconds on its own,
so game process → pack is reported apart from click → pack.

**Results (2026-11-07, this PC, n=5 per mode, all runs valid, medians in seconds):**

| Mode | click → game | game → pack | click → pack |
|---|---|---|---|
| Steam `-applaunch` | 1.75 | 6.31 | 8.01 |
| Launcher, Faster start off | 1.50 | 7.09 | 8.57 |
| Launcher, Faster start on | 1.59 | 5.77 | 7.90 |

- Run-to-run noise is large: game → pack ranged 5.7–8.6 s within one mode, and Steam's own hop
  1.3–8.6 s. So the conclusions come from **same-round pairs**:
  - **Launcher overhead:** click → game, launcher off minus Steam: +0.18, +0.10, 0.00 s in the three
    rounds where Steam's hop wasn't an outlier. Nothing measurable. (Game → pack can't differ: same
    command line; its pairs scatter −0.2 to +2.3 s, which is noise.)
  - **`-disableluarefresh`:** game → pack, on minus off: −0.68, −0.79, −0.70, −0.91, −3.54 s (median
    **−0.79 s**). An earlier, interrupted run gave −1.86, −2.59, −1.01 s (n=3). In line with
    startup-menu-speed #1's 0.8–1.6 s.
- **Memory:** tray only 2.3 MB private / 18 MB working set (no WebView2). Window open: +153 MB in
  WebView2's processes; closing it ends them (launcher 5.4 MB private after).

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
