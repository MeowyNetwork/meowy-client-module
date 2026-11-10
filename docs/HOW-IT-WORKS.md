# How the Meowy Client Pack works

The [README](../README.md) says what the pack does in plain words. This page has the full detail:
how each feature works inside the game, exactly what it changes, what it reads and sends, and the
console setting that turns it off. It's for players who want to check, and for reviewers.

Developers: the Lua API and build steps are in [DEVELOPERS.md](DEVELOPERS.md).

## Installing, starting and uninstalling

The installer isn't code-signed yet, so Windows may show "Windows protected your PC". Click
**More info**, then **Run anyway**. You can check the files first: every release lists SHA256
hashes in `SHA256SUMS.txt`. `MeowyClientPack.exe`, the Meowy Launcher it installs and the four DLLs
(also the ones in the manual zip, once unzipped) carry a GitHub build attestation proving they were
built from this repository by its CI (`gh attestation verify MeowyClientPack.exe --repo MeowyNetwork/meowy-client-module`).
The zip itself and `SHA256SUMS.txt` have none: check the DLLs inside the zip, or the zip's hash in
`SHA256SUMS.txt`.

**Manual install:** unzip `meowy-client-module-manual.zip` into
`steamapps/common/GarrysMod/garrysmod/lua/bin/` (create `bin` if it doesn't exist). To have it
start with the game, also add this line at the end of `garrysmod/lua/includes/menu.lua`:
`pcall(require, "meowy_boot") -- meowy-client-pack v1`. Without that line the pack never updates
itself (see *Start with the game* below).

**Uninstall:** in Windows' Settings → Apps → Installed apps (or Programs and Features), uninstall
**Meowy Client Pack**; or run the installer and click **Uninstall** (or `MeowyClientPack.exe --uninstall`). It
first puts back the `garrysmod/data` files the pack kept apart (see *A separate `garrysmod/data` for
every server* below): your own files, from before the pack and from the main menu, go back into
`garrysmod/data`, never overwriting anything there (on a name clash both are kept, and the one coming
back gets a suffix such as `name.before-pack.txt`). Each server's own files go to
`Documents\Meowy Client Pack backup\<server>`, not into `data/`, where every server could read them.
Then it removes the pack's files, its start-up line, the launcher (its exe, its sign-in start, the
`meowy://` link handler and the Start-menu entry) and `%LOCALAPPDATA%\MeowyClientPack`; a vault
that couldn't be emptied is left in place and named. If the launcher had set the Workshop font
cache aside (below), it's put back.
- **Without Garry's Mod.** If Garry's Mod is gone, Uninstall still works: the launcher and the pack's
  own files go, and the vault is kept, since its files can't go back into a `garrysmod/data` that
  isn't there. The window says so before it starts. From the command line this needs
  `MeowyClientPack.exe --uninstall --without-game`, which refuses while a Garry's Mod that Steam knows
  still has the pack (run `--uninstall` without the flag then), so a mistyped path can't leave the pack
  in the real game. With Garry's Mod there but the pack's DLLs already deleted, Uninstall is still
  offered while the launcher or the pack's folder is left.
- **By hand:** deleting the four `gmcl_meowy_*` and `gmsv_meowy_boot_*` files from that folder and the
  `meowy-client-pack` line from `lua/includes/menu.lua` leaves the vault where it is, and
  `garrysmod/data` stays a link to the main menu's folder in it (it keeps working). To undo that too:
  with the game closed, delete the `garrysmod/data` link (deleting a link never deletes what it points
  to), make an empty `data` folder, and copy `vault\menu` into it (and `vault\before-sandbox`, if 1.7.0
  left one and no later version has started with the game since).
- **From Windows' list of apps.** Since 1.7.2 the installer adds **Meowy Client Pack** to Windows'
  installed apps, for your account only (`HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\MeowyClientPack`,
  no admin rights), and keeps a copy of itself in `%LOCALAPPDATA%\MeowyClientPack\uninstall`, since the
  one you downloaded may be gone. The entry shows the version that runs: the installer writes its own,
  and from 1.7.3 the pack writes its version there at every start, so an in-game update shows too (only
  when the entry exists; the pack never adds one). Uninstall there opens that copy's window and asks at once, with
  Garry's Mod or without it, as above; cancelled, the window stays so you can pick the `garrysmod`
  folder. Only running the installer adds the entry or updates the kept copy: in-game updates don't, so
  an install that only updated itself to 1.7.2 isn't listed until you run the 1.7.2 installer once. No
  version is shown there, as in-game updates would make it out of date. A program can't
  delete itself while it runs, so once the window closes a copy of it in `%TEMP%`
  (`MeowyClientPack-uninstall.exe`, left there for Windows to clean up) removes the `uninstall`
  folder, but only if the entry is gone: an uninstall that stopped partway keeps both, to try again.
  Every uninstall removes the entry. It also deletes a `garrysmod\cfg\meowy_restore.cfg` left
  by 1.7.0 or 1.7.1 (see *`config.cfg` backups*).

**Installing again** always makes the installer's own version the one that loads: versions the updater
downloaded, their pointers and both architectures' update floors (`state\release_seq_<arch>.txt`) are
cleared, so the updater can bring the current release back. An installer started with "Run as
administrator" doesn't start the launcher (it starts at your next sign-in, or from the Start menu), so
no elevated launcher is left in the tray; with UAC off, or on the built-in Administrator account,
nothing changes. The installer also replaces a running launcher: it asks it to quit, then writes the new
one. The pack's in-game updates never replace the launcher, so a launcher is as new as the last installer
you ran.

**`MeowyClientPack.exe --reset-policy`** (game closed) forgets the kept kill-switch policy
(`state\policy.txt`, `.sig` and `.bin`), so the pack and the launcher take whatever Meowy Network
publishes now, as on a fresh install. Only use it when Meowy Network tells you to: a bad policy that's
still published comes straight back.

**Start with the game** (on by default in the installer): the installer adds that one line to
`lua/includes/menu.lua`, so the pack is loaded at the main menu instead of only once a server's
Lua starts. This is what later join speed-ups, our own loading screen, the main menu's features and
**updates** need. Off, the pack loads only when a server's Lua asks for it (Meowy Network's does), and
that copy stays as installed: only the start-up stub that line loads switches to a downloaded version.
So with it off, the launcher downloads nothing (its Updates panel says why), and a pack loaded by a
join prints once per start that it can't update itself, naming any version that's waiting. Run the
installer again with it ticked to turn it back on. If the game ever fails to start
with it, hold **Shift** while Garry's Mod starts (or add `-meowy_safe` to its launch options) and the
pack isn't loaded at start-up. If the game closes while the pack is starting, the next start skips it
once by itself. A Steam "Verify integrity of game files" or a game update removes the line; the Meowy
Launcher puts it back (below), and so does running the installer again. The installer ticks "Start
with the game" from your own choice (`startup_off.txt`), not from whether the line is there right now,
so a line Steam removed shows ticked and Install puts it back.

## The Meowy Launcher

Source in `launcher/`, design in `launcher/DESIGN.md`. The installer also installs a
small tray app to `%LOCALAPPDATA%\MeowyClientPack\launcher` and, unless you untick it, starts it when
you sign in. Its version is the pack's (both come from `src/version.h`), and it's updated only by
running a newer installer, never by the pack's in-game updates. It never touches the running game's
memory. On disk it changes two things in the game's folder: the start-up line in
`lua/includes/menu.lua` (it puts it back after Steam removed it) and, for "lazy fonts", the name of the
Workshop font cache while the game starts (below). It starts Garry's Mod only through Steam, and not
at all while it runs elevated ("Run as administrator"; UAC off and the built-in Administrator account
are fine).
- **Play** and **Join Meowy Network** in the tray (and the Start-menu shortcut, the crash card's
  Rejoin and `meowy://` links) start Garry's Mod through Steam with `-disableluarefresh` and "lazy
  fonts". Both are on by default, and "Faster start" in its window turns both off. So a game started
  from the launcher has no Lua autorefresh (which only addon makers use); start the game from Steam,
  or turn "Faster start" off, to keep it. Lazy fonts: the extracted Workshop fonts in
  `cache\workshop\resource\fonts` are renamed to `fonts.meowy-lazy` while the game starts and put back
  as soon as the pack loads; the game registers them again at the first map load anyway. They're also
  put back when the launcher quits (tray Quit, `--quit`, the installer), and by Install and Uninstall.
- After a Steam update or Verify it puts the start-up line back, once Steam is done and the game is closed
  (never if you turned "Start with the game" off). A repair that can't add the line (start-up off, the
  pack's files missing) is tried once per change, not again and again.
- It finds the game by its folder both as configured and as Windows resolves it, so a game folder or
  Windows profile behind a junction works.
- If the game crashes, or is ended in Task Manager, while you're on a server, a small card offers
  **Rejoin**, and the pack doesn't ask again at the next start.
- `meowy://connect/<a.b.c.d>:<port>` links join a server: Meowy Network's directly, any other public
  server after you confirm. A link can't carry a password, launch options or commands.
- Its window lists Meowy Network, your Steam favourites and recent Garry's Mod servers with live players,
  map and ping, each with **Join**, and **Find servers** searches Meowy's server index (every Garry's Mod
  server Steam lists, with fake player counts marked). Optionally ("Tell me when Meowy Network is back up") it shows a card
  when our server answers again after a restart.
- Updates: while the game is closed, the launcher downloads a new pack version from the pack's own signed
  release (the same document and checks the pack uses in the game) and sets it up for the next start; the
  start-up stub switches to it. Only with "Start with the game" on: off, it downloads nothing and its
  Updates panel says updates need it. Its window shows the version you run and what's ready. It also
  follows Meowy Network's signed policy (the kill switch), and when it starts it rewrites
  `state\policy.bin` from the kept policy if that file differs (see *Meowy Network's kill switch*).

## Settings

**Meowy settings** (the main menu's list, after Options, or `meowy_settings` in the console) has
switches for: Stay connected through drops, Start with the game, Get servers ready before you join
(`meowy_prewarm`), Update a server's Workshop items before you join (`meowy_prewarm_workshop`), Garry's
Mod's server list: only servers with players (`meowy_fast_serverlist`), The console key opens the Meowy
console (`meowy_console_key`), Menu text size (`meowy_menu_scale`), Continue where you left off
(`meowy_menu_continue`), Your servers on the main menu (`meowy_menu_home`), Server card in the Escape menu
(`meowy_menu_escape`), Meowy home screen (`meowy_menu`), Discord status on Meowy Network
(`meowy_discord`), and the settings guard (`meowy_settings_guard`, with the servers whose changes are
always put back). Its other tab is the PC doctor.

Everything else is a console setting, named with its feature below: `meowy_party`,
`meowy_serverlist_reports`, `meowy_honest_serverlist`, `meowy_server_index`, `meowy_data_sandbox`,
`meowy_updates`, `meowy_loadingscreen`, `meowy_loadingscreen_everywhere`, `meowy_mouse_confine`,
`meowy_audio_follow`, `meowy_console_badge`, `meowy_console_overlay`, `meowy_idle_savings`,
`meowy_join_boost`, `meowy_pak_cache`, `meowy_fast_file_checks`, `meowy_fast_lua_lookup`,
`meowy_fast_lua_find`, `meowy_fast_fonts` and `meowy_skipmatreload_anyserver`. (`meowy_skipmatreload` is
made by Meowy Network's own server Lua, so it exists once you've joined us.) To type one, Garry's Mod's
console has to be on: **Options → Keyboard → Advanced → Enable developer console**. Then the console key
(under Esc) opens the Meowy console, or the game's own with `meowy_console_key 0`.

- **Where they're kept.** The game doesn't save the main menu's settings itself, so the pack keeps them in
  `%LOCALAPPDATA%\MeowyClientPack\menu_settings.txt`, one `name=value` line each, written within a second
  of a change. Only whole lines count (the first one for a name), and no value is saved with a control
  character in it, so one value can't hide a second setting.
- **Servers can't read them.** Every pack setting is created hidden (`FCVAR_HIDDEN`): a server's Lua can't
  read its value or tell it exists (`GetConVar`, `GetConVarString` and `ConVarExists` don't see it). They
  don't show in the game's `find`, `cvarlist` or its console's completion either; the Meowy console still
  completes them, and typing a name still shows or sets it. (Like any binary module, a server's Lua can
  still ask whether the pack's DLL is installed.)
- **Servers can't change the ones that guard you.** A server's Lua can run console commands on your game
  (`RunConsoleCommand`; Garry's Mod blocks only a fixed list of names). So for `meowy_data_sandbox`,
  `meowy_updates`, `meowy_settings_guard`, `meowy_party`, `meowy_serverlist_reports`, `meowy_discord`,
  `meowy_honest_serverlist`, `meowy_server_index`, `meowy_menu` and `meowy_menu_version`, the pack keeps
  the value it approved and puts any other change back through the console. Until then the setting
  reads as its approved value (also on the same frame), and only approved values are saved. The console
  says so once per setting per join. A change is approved when Meowy settings, the Meowy console or one
  of the pack's prompts makes it, or when it's made before you've joined any server in that start (your
  config files, launch options and the game's own console count then). After the first join, a change
  from anywhere else is put back, the game's console included: a server can queue commands behind a
  `disconnect`, so "typed at the main menu" can't be told apart from a server's leftovers. Use the Meowy
  console then. Only numbers are kept for these; a saved value with anything else in it starts from the
  default.
- **Meowy Network's kill switch** (below) can turn a setting off for everyone. It then reads as off,
  whatever you chose, and your own value comes back when the policy lets it.

## Feature by feature

Roughly in the order they were added. The first few (Discord status, crash reports, hardware) only work
on Meowy Network's servers.

- **Discord status.** It talks only to the Discord app on your PC, over Discord's local connection
  (`\\.\pipe\discord-ipc-N`), on its own thread so the game never waits on it. Our server sets your job,
  the player count, how long you've played and a Join button; Discord shows that to everyone who can see
  your Discord activity (people in servers you share, too), not only friends. Meowy settings →
  **Discord status on Meowy Network** (`meowy_discord 0`) turns it off: what Discord shows is cleared at
  once, and nothing the server asks for reaches Discord until it's back on (then the server's latest
  status shows again). Servers can't turn it back on (see *Settings*). Without "Start with the game" the
  pack reads the saved setting when you join. Discord's own activity setting turns it off too, for every game.
  Leaving a server (or switching it off) waits at most about a second for that thread, even when the
  Discord app hangs and stops reading: a write stuck on its pipe is cancelled, and a thread that still
  doesn't stop is left to finish on its own.
- Since 1.1 it also reads the game's own crash reports (`GarrysMod/crashes/*.txt`), so the
  server can see which addon crashed your game. Only the "Lua Stack Traces" section is read
  (at most 3 reports per join, 4 KB each, any `C:\...` paths masked). The rest of the report,
  which names your install path, never leaves your PC. Since 1.7.1 only reports of crashes that
  happened on Meowy Network are sent: at the next start the pack notes which reports were written
  while the game was on our server (`meowy_crashes.txt`, the newest 50; this needs "Start with the
  game"). Crashes on other servers, at the menu, in singleplayer or from before 1.7.1 are never sent.
  The pack lists the crash folder first, keeps only the reports noted as ours, and opens them
  newest first until it has enough, so a folder of hundreds of old reports costs nothing at a
  join, and newer crashes on other servers can't push ours out of the list.
- Since 1.2 each crash report also says where the game crashed, e.g.
  `engine.dll+0x200660 (0xC0000005)`, read from the game's own crash dump. Only the module's file
  name and the offset are used; the dump itself never leaves your PC.
- Since 1.2 it can describe your hardware: CPU and GPU model, thread count, RAM, video memory and
  Windows version. No serial numbers or user names. The server stores it with your Steam account
  ID, next to your frame-time stats, to see what players' machines struggle with. It's worked out
  once per game and reused at later joins.
- Since 1.3 it can make joins faster. At the end of loading a server's Lua, Garry's Mod throws
  away every material it has loaded and loads them all again; on a large map that is 5-15
  seconds of every join. When the server asks for it, the module skips the "throw away" step
  (anything new still loads). The game's materials stay exactly as they were loaded seconds
  earlier.
  - It changes 6 bytes of `client.dll`, in memory, for that session only. Nothing on disk
    changes, and no other game is affected.
  - It finds that code by an exact byte pattern and does nothing if anything differs (a
    Garry's Mod update, for example). The search runs once per game: later joins check that
    `client.dll` and those bytes are still the same, then change them without searching again.
  - Turn it off with `meowy_skipmatreload 0` in the console. A server can also turn it off for
    everyone.
  - It's switched on only while you join a Meowy Network server and put back as soon as you're
    in game, so other servers you visit in the same session are never affected.
  - With "Start with the game" on, `meowy_skipmatreload_anyserver 1` skips it on every server you
    join (off by default). On our map that's ~14 s of every join. The catch: a server whose
    downloads replace a material that's already loaded shows the old one until the next map.
- With "Start with the game" on, the first join of a session is much faster, on any server. While
  a server loads, Garry's Mod looks for each model and material in every search folder in turn, and
  most of those looks are for files that aren't there (91% of ~110,000 on our server). The pack reads
  each folder's list of files once and answers "not there" from it, without asking Windows to open the
  file. On our server the first join went from 70.6 s to 29-32 s; later joins are unchanged.
  - It points the file system object (`filesystem_stdio`) at a copy of its table of functions with one
    entry replaced, in memory; no code bytes change. The copy goes in at the main menu and stays for
    the session, but it answers from its lists only while a join loads; the rest of the time it passes
    every call straight on. Folders other things write to during a join (`data`, `download`, `cache`,
    `cfg`, Steam's workshop folder) are never listed, a write drops that folder's list, and a list kept
    from an earlier join is used again only if the folder hasn't changed since.
  - Turn it off with `meowy_fast_file_checks 0` (at the main menu it puts the game's own table back;
    `1` puts the copy back in, also in the same session). `meowy_fast_file_checks_status` shows the
    counts.
- The pack's own settings are kept in `%LOCALAPPDATA%\MeowyClientPack\menu_settings.txt`, hidden from
  servers, and the ones that guard you can't be changed by a server (1.7.1): see *Settings* above.
- With "Start with the game" on, every server's Lua loads faster, on any server. For each Lua
  file a server's code loads, Garry's Mod searches the server's file list once per search folder,
  comparing every name in it. That is ~4,000 searches per join, and on our server it was 16 s of
  every join. The pack has each search ask the list's own index instead. The answer is the same
  name, compared byte for byte, so every file resolves exactly as before.
  - It changes 15 bytes (64-bit) or 5 bytes (32-bit) of `client.dll` in memory, from the moment
    the game starts. Nothing on disk changes.
  - It does nothing unless the game's code, the list's type and the index all check out exactly;
    otherwise the game's own search runs.
  - Turn it off with `meowy_fast_lua_lookup 0` (it switches back at your next disconnect).
    `meowy_fast_lua_lookup_status` shows how many files it found.
- With "Start with the game" on, `file.Find` on a server's Lua is 35–90 times faster, on any server.
  Addon loaders call it a few hundred times while a server's Lua starts. For every search folder,
  Garry's Mod copies each name in the server's file list into a new string, tests it against the
  pattern and throws it away: 13–17 ms per call, even for a folder that doesn't exist. Only names that
  start with the pattern's fixed part (everything before the first `*` or `?`) can pass that test. So
  the pack hands the game's loop just those names, from a sorted copy of the list, in the same order,
  and the game's own code does the rest.
  - Measured on our dev server with its own Lua-find cache switched off (as on any other server): a
    call went from 14.4 to 0.38 ms, and a join from 21.7–22.1 s to 13.1–15.5 s. The results of 30
    patterns were identical byte for byte, with and without it.
  - It changes 31 + 34 bytes (64-bit) or 23 + 29 bytes (32-bit) of `client.dll` in memory. It does
    nothing unless the loop, its wildcard test and the matcher behind it are exactly the expected
    code and the list is the engine's own; otherwise the game's own loop runs.
  - The sorted copy follows the list: names added while you join are merged in, and every name is
    compared with the copy again only when a join or map change starts or the list is a new one.
  - Turn it off with `meowy_fast_lua_find 0` (it switches at your next disconnect).
    `meowy_fast_lua_find_status` shows the walks, the names skipped, and the names checked against
    the copy next to what checking every name at every search would have been.
- With "Start with the game" on, the font pass on every join is about twice as fast, on any
  server. At each map load Garry's Mod registers all ~400 custom font files again, and loads
  and frees `gdi32.dll` (through the Steam overlay) for every single one, although it's loaded
  the whole time. The pack hands it the `gdi32.dll` that's already loaded instead: 739-803 ms
  became 384-386 ms here. Every font is still registered exactly as before.
  - It changes two entries in `vguimatsurface.dll`'s import table, in memory. No code changes.
  - Turn it off with `meowy_fast_fonts 0`. `meowy_fast_fonts_status` shows the loads saved and
    how long the last font pass took.
- With "Start with the game" on, the pack gets a server ready before you join, on any server. A join
  reads the same big files every time (the map, its content, the biggest addons). If Windows has thrown
  them out of memory since, it reads them from disk while you wait on the loading screen. So:
  - **While you join,** the pack writes down which files of 1 MB or more the game opens, with their sizes
    (`%LOCALAPPDATA%\MeowyClientPack\joins\<address>.txt`, the last 60 servers). Nothing about the files
    themselves is kept.
  - **When you're likely to join that server again,** it reads them into Windows' memory in the background,
    at low priority: 3 s after the menu shows your Continue server, or when you hold the mouse over a
    server for 0.4 s on the home card or home screen. It reads at most 4 GB, stops the moment a join
    starts, and only on an SSD (on a hard drive the reads would get in the way of everything else).
  - Measured on our dev server, from a fresh start of the game with Windows' memory cleared: in game after
    46.2 s without it and 30.0 s with it. It had read 1,516 MB in 42 files, in 2.7 s, before the join.
  - The warming only reads files, and the game opens the same files as before. To see what a join reads,
    the pack changes two entries in `filesystem_stdio.dll`'s import table in memory (`ReadFile`, and
    `CloseHandle` to drop a closed file), no code. They go in at the first join with pre-warm on and
    stay for the session, but note anything only while a join loads. With both pre-warm switches off
    (or off by Meowy Network's policy), they're never put in and no `joins\<address>.txt` is written. A
    join you cancel keeps nothing.
  - It needs the fast file checks (`meowy_fast_file_checks`, on by default) to see which files a join
    opens. Turn it off in Meowy settings ("Get servers ready before you join") or with `meowy_prewarm 0`.
    `meowy_prewarm_status` shows what it has written down and read.
  - **Workshop items, too.** The same join notes which Workshop items the server had you mount (from the
    `.gma` files it opens, any size). At the same moments, the menu checks them, and an item that needs it
    downloads or unpacks on the main menu instead of on the loading screen.
    - **What needs it:** an item Steam says isn't installed, has an update or is mid-download (read
      through the game's own `steam_api`), a legacy item whose unpacked copy in `cache\workshop` is gone,
      or an item whose Workshop version (`steamworks.FileInfo`) is newer than the one installed.
    - **What happens:** only those go to the menu's own `steamworks.DownloadUGC`, one at a time, so Garry's
      Mod's download box shows only for real work. It stops asking when a join starts; anything already
      downloading carries on, and the join uses it.
    - **Servers you've never joined** (the content platform, `docs/CONTENT_PLATFORM.md`): a server can say
      which Workshop items its join will ask for, in a `meowy_content` rule the game's standard server query
      returns (Meowy Network's servers do). The pack reads it at the same moments, for any server, and adds
      what the last join learned.
      - Meowy Network's servers (a few seconds after the menu opens) and servers you've joined before are
        got ready without asking, up to 4 GB each: a first join
        then only checks the items instead of downloading them on the loading screen. Measured on our dev
        server with ~600 MB of Workshop content this PC didn't have: in game after 27.2 s instead of ~51.9 s.
        The content had downloaded at the main menu in 14.6 s, and each prepared item cost the join 0.03 s.
      - Any other server's row (the home card, the Meowy home screen's lists, and the server picked in
        Find servers) shows **Get ready · 1.3 GB** instead; nothing downloads until you click it. Over 4 GB
        always waits for the click. On the home screen, Get ready is its own button and doesn't join.
      - While it downloads, the row shows **Getting ready · 412 MB of 1.3 GB**. If Steam didn't download
        some items, it reads **Partly ready · 2 items didn't download · Try again** ("Not ready" if none
        did), and a click checks and asks the server again. `meowy_content_get_ready <address>` does the
        same from the console.
      - A server's answer is reused for 10 minutes and kept in memory only. A party's next server isn't
        asked before you follow it (see *Parties*).
      - The rule is only a list of Workshop IDs: all it can make the pack do is download Workshop items, which
        Steam checks and the join checks again. The query is the one the server browser already makes.
    - **Measured** on our dev server, fresh start, with one 94 MB legacy item's unpacked copy deleted: the
      loading screen's Workshop step took 4.7 s without the check and 2.1 s with it (it unpacked the item
      at the menu; checking all 29 items took 3.0 s). An update day's downloads move the same way.
    - **Switch:** its own, in Meowy settings ("Update a server's Workshop items before you join",
      `meowy_prewarm_workshop`, on by default). Turning it off (or Meowy Network's policy turning it off)
      stops what's queued at once. `meowy_prewarm_status` shows the last check.
- With "Start with the game" on, joins to a Meowy Network server get the pack's own loading
  screen instead of Garry's Mod's. It's a separate window over the game, drawn on its own thread,
  so it keeps moving while the game is busy (Garry's Mod's own screen freezes for most of a join).
  - It is our web page (meowynetwork.com/pack-loading.html) shown with WebView2, the browser engine
    built into Windows, in its own processes. It has the join's stages, a clock time for when
    you'll be in, games to play while you wait, a map of the city, and **Cancel** and **Game's
    screen** buttons. If WebView2 or the site isn't available, a simpler built-in screen is shown.
  - The page is loaded, hidden, when the game starts, so a join shows it at once. Between joins
    it waits hidden (WebView2's processes use some memory, outside the game).
  - It starts the moment the game says it's connecting to one of our servers, so Garry's Mod's own
    page never shows. The server's name, the map and your SteamID64 are passed to the page only once
    the game's real connection is confirmed to be ours. The page then sends your SteamID64 to
    meowynetwork.com (`/api/player-loading-info`) to show your name and Steam avatar. For any other
    server it doesn't appear, or closes.
  - Only when the game is windowed or borderless. It stays until the game runs smoothly after you
    spawn. `meowy_loadingscreen 0` turns it off; so does Meowy Network's kill switch, which then also
    keeps the hidden page from loading.
- **Honest server list.** Some servers fake their player count to climb Garry's Mod's server list: a
  rewritten query reply, bots hidden from the bot count, or idle accounts made only to sit there.
  The pack checks the counts in the in-game server list (Find Multiplayer Game):
  - It reads each populated server's player list twice, 45 seconds apart, the same query the
    browser makes when you click a server. A count far above the list, players whose time doesn't
    grow, or the same player joined at the same moment on two servers mark the server
    `[FAKE - N real]`; it then shows and ranks by N. A server that won't give its list shows
    `[LIST HIDDEN]`, with its count untouched. A list that comes in several packets is put together
    only from packets of the same reply (Source's split ID), so a late reply to the first ask can't
    mix with the answer to the second.
  - It also downloads Meowy Network's own verdicts (`meowynetwork.com/api/pack/servers`), from a
    crawler that adds Steam's own count and the count's history, and from an observer that joins
    servers: `[VERIFIED]` means it joined in the last 7 days and saw nothing fake; `[UNVERIFIED]`
    means the server refused it (a VPN or hosting block), which is not an accusation. Being full, or
    having a password, doesn't count as refusing when the crawler had seen the server near full or
    with a password, unless it turns the observer away like that three times since it last got in.
    Servers marked `[UNVERIFIED]` or `[LIST HIDDEN]` sit slightly lower in the list than
    otherwise-equal ones; none are hidden.
  - The tags and rank changes reach the list together, in one call to its page a frame. No check
    starts while a join loads or while you're in game; the rest wait for the main menu.
  - **What it sends:** on a server you join that isn't ours, it runs the game's `status` command
    twice, about 90 seconds in and four minutes later. It sends meowynetwork.com the server's
    address, its advertised count, the number of bots `status` lists, and for each other account
    its SteamID, ping, packet loss, state and connected time. Your own row is left out (if the pack
    can't tell which row is yours, it sends nothing). The backend turns the SteamIDs into salted
    hashes as they arrive, never stores or logs them readable, and keeps the hashes 14 days (the day
    they arrive and 13 more, UTC), to spot the same accounts on a server around the clock. Older
    hashes are deleted every hour, whether or not new reports come in. It judges behaviour only:
    hidden bots, flat pings, players stuck connecting, one account on two servers at once.
  - **Who sent it (1.7.2+):** each report also carries a Steam ticket the pack asks the Steam client
    for, made for Meowy Network only (`GetAuthTicketForWebApi`, identity `meowynetwork`). The backend
    asks Steam whose account it is (`AuthenticateUserTicket`), so one account counts once however many
    networks it reports from (with Family Sharing, the copy's owner). It keeps only a salted hash of that
    account, in memory, for the 6 hours below, never the ticket or the SteamID, and hashed differently
    from the rows, so it can't be matched with them. The pack cancels the ticket once the report is
    sent. Without a ticket (Steam didn't give one within 10 s), the report goes as 1.7.1 sent it.
  - **What reports can do:** never mark a server, and never back or change a verdict. When at least
    three voices within 6 hours mostly say a server looks fake, they only queue a visit by Meowy
    Network's observer, after the crawler's own suspects. A voice is a Steam account (a report whose
    ticket Steam vouched for), or a network (IPv4 /24, IPv6 /64) for a report without one: from 1.7.0
    or 1.7.1, or with a ticket Steam couldn't check. A network one of the accounts reported from adds
    no voice of its own. Meowy Network can stop counting reports without an account once 1.7.0 and
    1.7.1 are rare. Verdicts come only from the crawler's and the observer's own checks. A server the observer visits only because
    reports asked for it is never labelled `[UNVERIFIED]` if it refuses. The website doesn't log the
    pack's requests with your IP address.
  - `meowy_honest_serverlist 0` turns the whole check off; `meowy_serverlist_reports 0` keeps the
    checks but stops the reports. Our own servers are never checked. What each report did (counts and
    the HTTP result, never SteamIDs) goes to `%LOCALAPPDATA%\MeowyClientPack\serverlist.log`.
- **The Meowy console.** The game's console in the pack's look, at the main menu or over the game. The
  console key opens it anywhere. `meowy_console` opens it only at the main menu, with no join running and
  none ended in the last 2 seconds: any server's Lua can run that command, even right after a
  `disconnect` it queued.
  - **Lines:** the last 50,000 console lines with their time and colour, kept from the moment the game
    starts. Warnings show in amber and errors in red, and text that isn't valid UTF-8 is cleaned.
  - **Finding things:**
    - **Search** as you type (Ctrl+F) shows only the lines with it. With "Mark, don't filter", every line
      stays and the matches are marked; Enter (or F3) goes to the next, Shift+Enter to the previous.
      "Lua pattern" searches with a Lua pattern.
    - **Chips** for errors, warnings, network (connects, drops, timeouts), Workshop and the pack's own lines
      show how many there are of each and narrow the log to the ones picked.
    - **Collapse repeats** folds the same line into a count, also when it comes back within a few lines and
      10 seconds, so spam ("x512 in 10 s") doesn't bury the rest.
  - **Dividers** mark where a join starts, where you're in game and where you leave. While you're scrolled up,
    a pill says how many lines came in below and takes you back down.
  - **Copying:** drag over lines (Shift+click extends) and Ctrl+C or Copy copies them; Copy with nothing
    selected takes everything shown. Right-click a line to copy it, search for it, show only lines like it,
    or, for a Lua file, ask the game which addon it's from (`whereis`).
  - **Typing:** as you type a command's name, the matches show with their current values and what they do
    (from the engine's own command list, read once in the background), and Tab completes. Once a setting is
    typed, its value, default, flags and help show above the line, with "Reset to default" (only for a
    plain default, without `"`, `;` or a line break; it sets the default as one value). Ctrl+R searches
    what you typed before, and that history is kept between starts (`console_history.txt`).
  - **Pinned commands:** "+ Pin a command" keeps what's typed (or the last command) as a one-click chip;
    right-click a chip to unpin it. Kept between starts (`console_pins.txt`).
  - **Secrets:** a line with `password`, `rcon_password`, `sv_password`, `rcon`, `sv_setsteamaccount` or
    `cl_steamauth_token` in any of its `;` parts (any case), or a `bind`, `bindtoggle`, `bind_mac` or
    `alias` holding one, still runs, but isn't kept in the history or the pins, and its echo in the window
    hides the secret (`password ********`). Such lines saved by 1.7.0 are dropped at the next start.
  - **Commands:** a typed line runs exactly as if typed into the game's console, through the engine.
    The menu's `RunConsoleCommand` refuses some commands, the console doesn't. Up and Down go through what
    you typed. The commands for the game's console window act on this one: `clear` empties it (what ran
    before it in the line goes, what runs after stays), `showconsole` leaves it open, and `hideconsole` and
    `toggleconsole` close it. `con_filter_enable 1` with `con_filter_text` / `con_filter_text_out` filters
    its lines as it does the game's console ("con_filter on" in the header). The marks the window uses to
    find a `clear` it ran and the command list it asked for are made new at each start and count only while
    it's waiting for them, so a server printing them just prints a line.
  - **Window:** drag it by the title, resize it from the corner; the console key or Esc closes it. The header
    shows the frame rate and, in game, ping, loss and the server. Save writes the lines shown to
    `%LOCALAPPDATA%\MeowyClientPack\logs\console_<date>.txt` (paths hidden; the newest 20 kept) and copies
    that file's full path to the clipboard.
  - **Errors tab:** every Lua error since the game started, grouped by the addon it came from (with its
    Workshop ID and when it was last updated): the error, how often, first and last time, the realm, the
    server you were on, and the stack.
    - **Copy report** puts a ready-made bug report on the clipboard, with paths hidden.
    - **Open on Workshop** goes to the addon. At the main menu, **Disable addon** (asked first) turns it off.
  - **Error badge:** in place of Garry's Mod's error notices, a small pill says how many errors and from
    which addon. Garry's Mod's other two notices (a serious problem; the pause menu blocked) show the same
    way. `meowy_console_badge 0` brings Garry's Mod's notices back.
  - **Overlay:** `meowy_console_overlay 1` shows the newest warnings and errors over the game (2: every line),
    see-through, click-through and fading; off (0) by default.
  - **The console key:** when Garry's Mod's console shows, it's hidden at once and this one opens. Off in Meowy
    settings ("The console key opens the Meowy console", `meowy_console_key 0`) for the game's console.
  - It reads tier0's output through its own listener (the logging system or the spew function, whichever
    this build has). `meowy_console_status` says which, and how many lines are kept.
- **Find servers, faster.** Find servers' Internet list comes straight from Steam, and Steam's master server
  leaves out empty servers before anything is pinged. Steam pings at most about 5,000 servers a minute, so
  the full list of ~4,800 took 88 s here. The servers with players (964) are complete in 20 s, and the first
  hundred show within a second.
  - Each server reaches Garry's Mod's own page through the game's own code, with the same values the game's
    query gives (its tags read the same way). So the gamemode list, categories, blacklist and the count
    checks above work as before. Favourites, History and LAN are the game's own, complete lists.
  - It reads Steam's server list through the game's own `steam_api`, `ISteamMatchmakingServers`.
  - The page gets at most 50 servers a frame. A join started while the list is still filling stops it
    (the browser asks again the next time it's opened), so it doesn't compete with the join.
  - Off in Meowy settings ("Garry's Mod's server list: only servers with players") or with
    `meowy_fast_serverlist 0` for every server, the game's way. It only affects Garry's Mod's own server
    list: the Meowy home screen's Find servers always lists every server, the empty ones last.
    `meowy_fast_serverlist_status` shows the lists open.
  - **A Friends tab**, after Local Network: the servers your Steam friends are on (the game's own friends
    list: servers only, no names), shown like the other tabs. It's there with the home screen on or off.
- **Find servers from Meowy's index** (MR3, `docs/server-index-and-parties.md`). On the home screen,
  the Internet list fills in about a second from Meowy's own index of every Garry's Mod server. Then
  Steam's own list updates those rows in place (pings, counts) and adds any the index missed. Nothing is
  removed.
  - The index is `https://meowynetwork.com/api/pack/index`: Steam's master list, refreshed each minute,
    with the honest list's verdicts and no player names. The pack fetches it off the game's thread (gzip,
    no redirects, 4 MB at most, 20 seconds at most), and fetches it again each minute while the list stays
    open. A download cut off midway counts as failed, and its version tag isn't kept, so the next fetch
    asks for the whole index again instead of being told nothing changed.
  - Pings Steam hasn't measured yet show as `~45`, greyed. That's either your last ping to that server's
    network or an estimate from the distance (IP Geolocation by DB-IP). The pings Steam measures are kept in
    `%LOCALAPPDATA%\MeowyClientPack\pings.json` (per /24 network, 30 days), never in `garrysmod/data`.
  - If the index can't be reached, is older than 10 minutes, or is off (`meowy_server_index 0`), the list
    is Steam's alone, as before. `meowy_server_index_status` shows the last fetch.
- **Parties** (MR3). Friends who move between servers together. The home screen has a party card: **Start a
  party**, **Invite friends** (Steam's own invite window), and the friends playing Garry's Mod, each with
  an **Invite**. It lists who's in the party and which server each one is on, with **Join** next to each.
  - A party is a friends-only Steam lobby of up to 8, through the game's own `steam_api`
    (`ISteamMatchmaking`). Steam's friends list shows the members together, and any friend can join from
    there.
  - **Whose party you join:** an invite from someone who isn't your Steam friend is ignored. A party you
    enter (from an invite, Steam's friends list, a `steam://joinlobby` link or `+connect_lobby`) is kept
    only if it's a Meowy party led by you or a Steam friend; otherwise the pack leaves it before
    anything about you is published there. Your current party is left only once the new one has passed
    that check.
  - The leader picks a server in Find servers and chooses **Take the party here**. The leader joins it.
    Each member's pack then shows a 10-second countdown, "Following Ann to Big RP", with **Stay** and
    **Go now**: as a card wherever the menu is up (the home screen and Garry's Mod's own main screen, Find
    servers and the other screens, and the Escape menu in game, with or without its server card) and, in
    game, as a banner over play. It counts down only while one of them is on screen, so it waits while a
    join loads. **Stay** (or `meowy_party_stay`) keeps a member where they are.
  - A member never follows a move made before they joined, or to a server on Garry's Mod's blacklist,
    and follows at most once every 30 seconds: a move sooner than that isn't dropped, its countdown just
    can't end before the 30 seconds are up, and a newer move replaces it.
  - The party's next server isn't contacted before a member follows: no pre-warm and no content query
    for it, so a leader can't make members' PCs talk to a server they haven't agreed to join.
  - A friend without the pack gets Steam's ordinary invite to the party's server instead.
  - Rich Presence: `steam_player_group` (the party) and `meowy_pack` (so friends' packs know who has it).
    Steam shows these to friends only. `meowy_party 0` turns parties off, along with both keys.
    Only friends playing Garry's Mod are read, and the pack asks Steam for a friend's Rich Presence at
    most once per game, only while it shows no `meowy_pack` (Steam keeps friends' up to date itself).
    `meowy_party_status` shows the party.
- **Its own files** are in `%LOCALAPPDATA%\MeowyClientPack`, never in `garrysmod/data`, which any
  server's Lua can read, so the pack leaves no files there for other servers to find. (Like any binary
  module, a server's Lua can still ask whether it's installed.) Before 1.4.1 these files were in
  `garrysmod/data/meowy/`; the pack moves them on first start. Every file it writes, and everything it
  sends over the network, is listed in *Everything it sends and writes* at the end of this page.
- **A separate `garrysmod/data` for every server.** Any server's Lua can list and read all of
  `garrysmod/data`: other servers' files (their names alone say where you've played) and anything else
  kept there. With the pack started with the game, `garrysmod/data` is a link (a directory junction) to a
  folder of its own for each server: `%LOCALAPPDATA%\MeowyClientPack\vault\<server>` while you're on that
  server, `vault\menu` at the main menu. If the game is on another drive the vault is
  `steamapps\MeowyClientPackVault`. A server only ever sees its own folder: every file path GMod gives Lua
  reaches `data/` through the link, and the vault itself is out of reach of all of them. Joining or
  leaving re-points the link in one step, and no file is moved while the game runs, so a file the game
  keeps open (a `.gma` a server mounted from `data/`, say) can't hold a switch up. If you look in
  `garrysmod/data` in Explorer, you see the folder it points to right now; the others are in the vault.
  The first time, your existing `data/` (PAC3 outfits, AdvDupe2 dupes, addon settings) becomes the main
  menu's folder, `vault\menu`, so the main menu, singleplayer and games you host still see it, and no
  server does; each server starts with an empty folder of its own. (1.7.0 kept it apart instead, in
  `vault\before-sandbox`, where nothing saw it. On a 1.7.0 install, 1.7.1 merges that into `vault\menu`
  once at start, never overwriting: on a name clash both are kept, and the copy from before the pack
  becomes `name.before-pack.ext`. The console says so.) Uninstalling puts your files back. A connection that isn't a plain
  `a.b.c.d:port` (a Steam P2P game, say) gets a shared folder of its own (`vault\p2p`, `vault\other`),
  never the main menu's files; singleplayer and hosting a game yourself use the main menu's. The folder is
  chosen from the game engine's own connection, never from anything a server prints. After a crash, the
  next start points the link back at the main menu's folder, even when the pack itself is skipped (safe
  mode, or the skip after a crash at start-up): the start-up stub does that part on its own. The switching
  only runs while the pack is started with the game, though: in a session where it isn't (safe mode, the
  skip after a crash), every server sees the main menu's folder, and after a Steam "Verify integrity of
  game files" removed the start-up line, whichever folder the link last pointed to. Links need an NTFS
  drive; on another kind (FAT32, exFAT) the pack puts your own files back in a plain `data/` and every
  server shares it. `meowy_data_sandbox 0` turns it off (every server sees the main menu's folder, with
  your own files), and `meowy_data_sandbox_status` says where `data/` points. Servers can't switch it off
  for you (see *Settings*).
  - **What it doesn't separate:** only `garrysmod/data` is per server. A server's Lua can still read
    what other servers left in places the game shares between them: the client `sql` database and
    cookies (`garrysmod/cl.db`), and maps and files downloaded from other servers (`garrysmod/download`).
    So it stops servers reading each other's `data/` files, not every trace of where you've played.
- **Stay connected through drops** (1.6+). Once, at the main menu, the pack asks "Stay connected through
  drops?". Nothing changes unless you say yes. Then:
  - Garry's Mod waits 60 seconds for a dropped connection to come back instead of 30 (`cl_timeout 60`).
    Your previous value is saved and put back when you turn it off (unless you changed it yourself since).
    The game saves it to `config.cfg` when it closes normally; if it's lost (a crash, a config file), the
    pack sets 60 again at the next start, unless you've set a value of your own. The pack never rewrites
    `config.cfg` itself.
  - If you're disconnected from a Meowy Network server anyway, the pack rejoins it: "Reconnecting to Meowy
    Network... (attempt n)", with **Cancel**. What the server gives back is its own rule: after a real
    drop (it saw you frozen for 10 seconds or more) while you were alive, and only if you're back within
    3 minutes, at most 3 times an hour and 10 a day. Your job comes back through the normal job limits
    (vote and mayor jobs never do). Your weapons, health and armor come back only if you weren't in a
    fight in the last 20 seconds and weren't restrained or downed. Jail and wanted time are always kept.
    Nothing comes back after a server restart (the pack still rejoins).
  - Only to the server the game was really connected to (the engine's own connection, as everywhere else in
    the pack), never to an address from a message or a server's Lua. Whether to rejoin is decided only from
    the game engine's own view of the connection, checked every frame: if the engine says the server had
    gone silent (timing out, nothing received for 10 seconds or more) when the connection ended, it was a
    drop. If packets were still arriving (a kick, a ban, quitting, a map change), it never rejoins. No
    message or console text is read for this.
  - A planned restart: just before restarting, our server tells the pack (`meowy.ExpectRestart`, which only
    works while you're on our server). If the connection then ends within that time and the server goes
    down (stops answering), the pack waits for it to come back and rejoins. If it's still answering 15
    seconds after you left, it wasn't the restart (you disconnected, were kicked or joined another server):
    nothing is rejoined, shown or offered.
  - Before each join it asks the server whether it's up (the same query as the server list). Tries come
    at about 2, 4, 8, 15 and 30 seconds (+-30%), then every 30; it gives up after 3 minutes (5 when the
    server is restarting) or after 3 joins in a row that failed, and doesn't try again until you've been
    back in the game. It never stores or sends a server password.
  - The server check is the pack's own query, not Steam's, so it works while Steam is still logging back on
    after the same drop. If the server is up but Steam isn't yet, the card says "Waiting for Steam to
    reconnect" and it joins the moment Steam is back (up to 5 minutes from the drop).
  - If it gives up (no answer in time, or joins that kept failing), the card stays with **Rejoin now** for one
    more try by hand. Not after a kick, a ban, a password or your own Cancel.
  - After a crash while you were on Meowy Network, the next start shows "Rejoin Meowy Network?". It
    only joins when you click **Rejoin**.
  - After a crash on any other public server, the next start shows "Rejoin the server?" with its name
    and address, also only on a click. The address is the game's own connection (never a LAN, loopback
    or Steam P2P one) and is kept in `%LOCALAPPDATA%\MeowyClientPack\on_server.txt` only while you're
    on that server. Automatic rejoin stays for Meowy Network.
  - **Meowy settings** in the main menu's list, after Options (or `meowy_settings` in the console)
    opens the pack's settings to change your answer.
- **Continue where you left off.** The main menu's list gets **Continue** above Start New Game, with the
  last server you got into the game on (any server) and how long ago. One click joins it (or
  `meowy_continue` in the console); nothing joins on its own, and it isn't shown while you're in a game.
  The address is the game's own connection, kept only for a public `a.b.c.d:port` (never a LAN,
  loopback or Steam P2P one), in `%LOCALAPPDATA%\MeowyClientPack\last_server.txt`; the server's name is
  only ever shown as text. Turning it off in Meowy settings (or `meowy_menu_continue 0`) also forgets the
  server, and none is kept while it's off.
- **Your servers on the main menu.** A card at the bottom right of the main menu, out of game only:
  - Meowy Network's live player count and map;
  - servers your Steam friends are playing on (the game's own friends server list: servers only, no
    friend's name is read);
  - your favourites, busiest first;
  - the servers you played on lately, newest first, with when.

  **Search** at the top of the card looks through every server in those lists (name or map), not just
  the ones shown; the search happens on the page, so nothing you type is sent anywhere. `/` jumps to it,
  the arrow keys move, Enter joins the marked server, Esc clears.

  Each row is the server's own answer (name, map, players without bots, ping); one click joins it, and
  nothing joins on its own. `meowy_home` lists the rows numbered, and `meowy_home_join <n>` joins one.
  - **Lists:** the game's own favourites and history, asked with the game's own server query, at most
    once a minute while the card shows. The server list's player-count checks aren't run for it.
  - **What's left out:** only public `a.b.c.d:port` addresses are shown, never LAN, loopback or Steam
    relay (`169.254.x.x`) entries. Favourites that don't answer are counted under the list, not listed.
  - **Safety:** names are only ever shown as text; a click sends the page's row number, and the
    address comes from the pack's own list.
  - **Turning it off:** in Meowy settings, or `meowy_menu_home 0`.
- **Menu text size.** Garry's Mod draws its menu at the same pixel size on any screen, so it's small at 1440p
  and tiny at 4K. The pack zooms the whole menu page: Auto is the screen height over 1080, so 100% at
  1080p, 133% at 1440p and 200% at 4K. Or pick 100–200% in Meowy settings (`meowy_menu_scale`, 0 =
  auto). It covers the main and Escape menus and every screen in them (Find servers, New game,
  Addons and the rest). The pack's own windows already scale with the screen.
- **Meowy home screen (on by default).** The pack's home in place of Garry's Mod's main screen ("Meowy home
  screen" in Meowy settings, `meowy_menu`). It has:
  - **Continue**, and your servers, as on the card below;
  - **one search box** (`/` to get to it) for your servers, installed maps, subscribed addons and settings.
    A map opens Start new game with that map chosen, an addon opens Addons, and a setting (by its name or
    what it covers, like "volume") opens Options or Meowy settings;
  - **Garry's Mod's news**, the latest three, from its own feed, opened the way Garry's Mod opens them;
  - **Find servers, the pack's own:** everything Garry's Mod's server browser does, faster, and more.
    - **Lists:** Internet (straight from Steam, as in Find servers above: the servers with players
      first, then the empty ones), Favourites, History (the last visit first), Local network and
      Friends (servers your Steam friends are on). Each list is kept while you look at another.
    - **Gamemodes first** (Internet): each with its players and servers, sorted by players, servers or
      name, filtered by category (click once: only it, twice: hide it), with a search, **Install** for
      a gamemode you don't have, and **Every gamemode** for all the servers in one list.
    - **Servers in columns:** name, map, players, ping and ranking (and the gamemode in a list of
      every gamemode). Click a header to sort by it, again to turn it around; the default is Garry's
      Mod's own ranking. Rows show the favourite star (a click adds or removes it), region flag,
      password, an outdated or newer version, your last visit, missing maps in red, and the honest
      server list's `VERIFIED` / `FAKE - 12 real` / `UNVERIFIED` (a fake shows its real count).
    - **Filters,** kept between starts: not full, has players, no password, not outdated, players from
      and to, ping at most, and regions (each flag: only these, never these, or either).
    - **A server picked** (a click): its details, the map's picture if you have it, and its players
      with their score and time, asked again every 10 seconds (the honest list's own query, which
      also reads lists some servers hide from Garry's Mod's). **Join**, its password, **Join when a
      slot opens** for a full one (asked every 5 seconds), the favourite, **Copy address** and the
      gamemode's **Workshop page**.
    - A double-click or Enter joins, right-click copies the address, Up and Down move, Esc goes back.
      Favourites has **Add a server**: an address, with or without its port. Servers on Garry's Mod's
      blacklist are left out, as its own browser leaves them out. "Garry's Mod's list" opens the
      game's own browser.
  - **the usual links:** Start new game, Addons, Dupes, Saves and Demos are Garry's Mod's own screens.
    Options, Meowy settings and Quit work as usual, and Garry's Mod's bottom bar (Problems, games,
    language, gamemode) stays.

  How it works:
  - **Inside Garry's Mod's own menu page:** nothing is reloaded. While it's on, a style rule hides the
    main screen's list and news, the pack's home takes their place, and the links are the page's own
    screens, so going there and back is instant. When the page has just loaded (the game starting, a
    language change), the home goes in on the next frame, so Garry's Mod's own list doesn't flash first.
  - **In game:** the Escape menu is Garry's Mod's, with the server card.
  - **What it can do:** every server's text is shown as text. The home asks the pack for anything only
    through `meowy.post(kind, value)`, a fixed list of message types (join a row, Continue, Options,
    Meowy settings, Quit), each with its value checked.

  **Use Garry's Mod's menu** at the bottom switches back for the rest of that start; `meowy_menu 0`
  turns it off. `meowy_menu_status` says which is showing, and why. While it was a preview most players got
  `meowy_menu=0` saved without choosing it (every setting is saved when any one changes), so it's turned on
  once for everyone when this version first starts; turning it off after that sticks.
- **Server card in the Escape menu.** In game, the same spot in the Escape menu shows the server you're
  on, any server, plus three quick settings:
  - **The server:** its name, map and players (its own answer, asked at most every 30 seconds while
    the menu is open).
  - **The connection:** ping, packet loss and time here, read from the game engine's own connection.
    Ping is shown the way `net_graph` shows it.
  - **Buttons:** **Reconnect** (`retry`), and **Copy** for the address.
  - **Quick settings:** sliders for master volume (`volume`), voice volume (`voice_scale`) and field of
    view (`fov_desired`, 75 to 100).

  The address and the server section show only for ours or a public `a.b.c.d:port` (LAN, Steam P2P and
  singleplayer get the sliders alone). The page can only send a slider's row number and a number back;
  which convar, and its range, is the pack's own list. `meowy_server_now` prints the engine's reading.
  Off in Meowy settings, or `meowy_menu_escape 0`.
- **PC doctor** (1.6+). A tab in the pack's settings (and `meowy_doctor` in the console) with tips for
  the usual reasons the game runs slowly. It only reads; it changes no Windows or game setting, and
  nothing it finds leaves your PC.
  - **Which GPU draws the game.** It lists your graphics adapters (with Windows' own "high performance"
    and "power saving" ranking) and reads how much GPU time Windows has charged to the game on each one,
    the same numbers Task Manager shows per process. If the game is on the integrated GPU while a
    stronger one is there, it says so and offers a button to Windows' Graphics settings, with the steps.
    The pack doesn't set the per-app GPU preference itself: that's your Windows setting.
  - **Power:** on battery, battery saver, and Windows' power mode ("Best power efficiency").
  - **Multicore:** `gmod_mcore_test` and `mat_queue_mode`, with the command to type if it's off.
  - **32-bit game:** how to switch to the 64-bit branch (Steam, Properties, Betas).
- **Windows notifications while you're alt-tabbed** (1.5+). When something happens to you in the city
  and the game isn't the window in front, Windows shows a notification ("Your printer is under
  attack.", "Staff replied to your ticket."), the game's taskbar button gets a small badge and flashes
  until you're back, and clicking the notification brings the game to the front.
  - The words are the pack's own, built into it for each kind of event. The server only says which
    kind; nothing it sends is shown, so a notification never has names, amounts or links in it (they
    can appear on your lock screen).
  - At most one of each kind every 10 seconds and 20 an hour. Windows' Do Not Disturb and its
    per-app notification settings apply as to any other app.
  - The installer adds **Meowy Client Pack** to your Start menu (it starts Garry's Mod through
    Steam): Windows only shows notifications from an app it has a Start-menu entry for. Without it
    (say, a manual install) they show as a tray balloon instead. Uninstalling removes it.
  - Only while you're on a Meowy Network server. Our server can switch them off for you
    (`meowy.SetNotifyEnabled(false)`, kept in `notify.txt`); a switch for it in the game's settings
    comes later.
  - New kinds of notification don't need a new pack (1.6+). Our server hands the pack a list of
    kinds and their words, signed with Meowy Network's own key. The list is signed on our own PC; the
    key isn't in any repository or on the game server, which only has the signed list. The pack
    checks that signature against the key built into it and never takes an older list, so no other
    server or addon can change what a notification says. The list is kept as `notify_catalogue.txt`
    and checked again each time it's read.
- **Markers on Steam's game recording.** If you record with Steam (Steam settings, Game Recording), the
  pack adds a marker to the recording's timeline when you're arrested or released, your printer is
  destroyed, you complete a contract, or you become wanted, so you can jump straight to the moment
  and clip it in Steam. While the pack's loading screen is up, the join shows as loading on the
  timeline.
  - It uses Steam's own Timeline API from the Steam files Garry's Mod already loads; the pack adds
    no Steam files and nothing leaves your PC. Without recording on, Steam just ignores the markers.
  - The marker titles ("Arrested", "Contract complete") and icons are the pack's own. The server only
    says which kind; nothing it sends is shown.
  - At most one of each kind every 30 seconds and 60 an hour, and only while you're on a Meowy
    Network server.
- What a server's Lua can call (Discord status, crash reports, hardware, skipping the material reload,
  notifications, Timeline markers, `meowy.ExpectRestart`) only works on Meowy Network servers. The DLL
  asks the game itself which server it's connected to and compares that with the addresses built into
  it, so another server can't use it or read anything from it, even by faking its name or address in
  Lua. In singleplayer or on any other server every such call returns `false` and a reason. Everything
  else on this page (the main menu, the server browser, joins, the console and the rest) runs in the
  main menu's own Lua and in the DLL, which no server's Lua can call; servers can reach only the console
  commands and settings described with each feature.
- Windows only for now (32-bit and 64-bit Garry's Mod).

## Added in 1.7.0

- **Settings guard.** When a server changes your saved graphics, network, movement or HUD settings (which
  would follow you to every server), you're asked when you leave: **Keep**, **Put back** or **Always put
  back here**. Changes you made yourself in Options while you were there show too; that's what Keep is
  for. Meowy Network's own servers are never second-guessed (what changes there is your own choice in its
  FPS Booster). `meowy_settings_guard 0`, or its switch in Meowy settings, turns it off.
  - **Each server answers for itself.** A session starts with a join (the snapshot is taken before any
    server Lua runs) and ends a few seconds after you're back at the main menu. Joining another server
    without the menu in between (a party follow, the in-game browser) ends the last server's part: kept
    if it's ours, put back at once if it's on your always list, otherwise asked about later. Parts are
    asked about newest first, each checked for what's still in place only after the newer one is
    answered, so two servers that changed the same setting each answer for their own step.
  - **When the game closes on a server** (a quit or a crash), the game has already saved that server's
    changes to `config.cfg`. The next start asks about them ("3 settings changed while you were on X"),
    8 seconds after start (once `config.cfg` has run) or when a join starts, whichever is first. Then
    only saved settings count, and nothing is put back without asking, even for a server on your always
    list. Putting it back saves `config.cfg` again (`host_writeconfig`). A prompt you didn't answer before
    quitting comes back the same way. Anything over a week old is dropped. The session and the parts
    waiting are kept in `settings_guard_session.txt`.
  - **The always list** is kept in `settings_guard_always.txt`, not in a setting, so no server can read
    which other servers you play on (1.7.0's list is moved there once). Remove a server under Meowy
    settings → Settings guard, or with `meowy_settings_guard_forget <address>`, which works only at the
    main menu and after you confirm in a prompt.
- **`config.cfg` backups.** One copy of your `config.cfg` per start (when it changed), the newest 10, in
  `%LOCALAPPDATA%\MeowyClientPack\config_backups`. `meowy_settings_backups` lists them and
  `meowy_settings_restore <n>` puts one back, but only at the main menu (up at least 2 seconds, no join
  running) and only after you confirm in a prompt there: any server's Lua can run the command, but not
  click. Put back first saves your current settings (`host_writeconfig`), writes the chosen copy to
  `garrysmod\cfg\meowy_restore.cfg`, then makes a backup of your current `config.cfg`, then runs the
  copy. `meowy_restore.cfg` ends with `meowy_restore_done`, and is deleted as soon as the game has run
  that line, so after the rest; or after 30 seconds if it hasn't, or at the next start if the game
  closed in between.
- **Sound follows the default device.** Plug in headphones mid-game and the game's sound moves to Windows'
  new default device (the pack runs `snd_restart` when Windows says the default changed).
  `meowy_audio_follow 0` turns it off. The PC doctor also has a microphone section (Bluetooth hands-free
  mode, two microphones). Watching starts on a thread of its own without holding up the menu, so a slow
  Windows audio service at start no longer turns it off for the session.
- **The mouse stays in the game while you aim.** In windowed or borderless mode, while the game is in front
  and the cursor is hidden, the cursor is kept inside the game window (Windows' `ClipCursor`), so a fast
  flick can't click another window. It's free whenever a cursor shows and on alt-tab.
  `meowy_mouse_confine 0` turns it off. It checks 40 times a second only while the game is in front, 4 times
  a second while another window is, and not at all while it's off.
- **The loading screen on other servers.** Servers without a loading page of their own get a neutral
  version of the pack's screen: the server's name and map, each stage's time, and an estimate from your
  earlier joins there (kept per server on your PC). Servers with their own loading page keep it.
  `meowy_loadingscreen_everywhere 0` keeps it to Meowy Network, `2` shows it on every server.
- **Join priority and idle savings.** While a join loads, Windows is asked not to slow the game down even
  when it's in the background (`meowy_join_boost 0`). At the main menu, the loading screen's page is
  unloaded after two minutes in the background or once you're in game, and the menu's frame rate is capped
  while the game isn't in front (`meowy_idle_savings 0`). The pack changes `fps_max_menu` and
  `fps_max_nofocus` only for a while; your own values are kept in `cvar_restore.txt` and put back at the
  next start, and again for 30 seconds after it if `config.cfg` runs later and changes them, until they
  hold. Where 1.7.0 left `fps_max_menu` at its cap of 15 with nothing kept, 1.7.1 puts the game's
  default back once (noted in `repairs.txt`); a fresh install never touches a 15 of your own.
- **A map's compressed files, unpacked once.** The pack unpacks a map's compressed files ahead, on spare
  cores, instead of on every read. `meowy_pak_cache 0` turns it off.
  - Garry's Mod's `CZipPackFile::ReadFromPack` (in `filesystem_stdio.dll`) unpacks a whole LZMA entry on
    every read. The pack points that one call at its own cache: it changes the call's 4-byte target in
    memory, and on 64-bit adds a 14-byte jump to the cache within reach of it. It finds the call by its exact
    instructions and changes nothing if anything differs. The patch goes in at the first join and stays
    for the session; outside a join it passes every read straight through, and the cache (256 MB on
    64-bit, 64 MB on 32-bit) is emptied when the join ends.
  - Worker threads read ahead from the map's pak (`garrysmod\cache\map_pack.bsp`). An entry whose sizes
    can't be real is skipped, and if memory runs out the workers stop and the join goes on.
- **Signed updates.** The pack checks `meowynetwork.com/pack/v1/release-stable.txt` a minute after start and
  every 6 hours. A new version is used only if Meowy Network's release key signed the document and the
  file's SHA-256 matches, and only while the document hasn't expired, Meowy Network's policy allows that
  version and this install is in its rollout (picked with a random token that never leaves your PC).
  Only this architecture's DLL is downloaded, into `%LOCALAPPDATA%\MeowyClientPack\bin\<version>`, and
  the start-up stub starts it the next time the game opens, never mid-session. **Updates need "Start with
  the game"** (see above): only that stub switches versions.
  - **The way back:** if the game closes while a new version is starting (before the pack has finished
    loading), in its first two starts, the version is marked bad and the previous one loads. A crash
    later in a session (during a join, say) doesn't count: that version stays until Meowy Network
    revokes it with the kill switch, and then the stub loads the one before.
  - **No downgrades or replays:** every release document has a seq, and each architecture keeps the
    highest it took (`state\release_seq_<arch>.txt`), so the 32-bit and 64-bit game update on their own.
    1.7.0's shared `release_seq.txt` isn't read any more. The same document is taken again only when this
    architecture's copy of its version is missing or doesn't match its hash (the 1.7.0 launcher could
    delete it); an older one never is. Clean-up keeps both architectures' versions.
  - The Meowy Launcher does the same checks while the game is closed. Only installs made with 1.7.0's
    installer or later can update. Running the installer always makes its own version the one that loads
    (see *Installing again*).
  - **Out of the way of the game:** a check due while a join is loading waits until it has loaded
    (looked at every 30 seconds). The check runs below normal priority, and the download, its write and
    its SHA-256 at Windows' background disk priority. A download that doesn't arrive whole, or takes
    more than 10 minutes, is dropped and tried at the next check.
  - `meowy_updates 0` turns updates off everywhere. Every check (the pack's, mid-session too, and the
    launcher's) reads it first, also when the running version is revoked or too old, so after it's set
    nothing contacts the site or downloads. A version already downloaded waits: the stub starts no
    downloaded version it hasn't started before while updates are off. A version already running stays.
- **Meowy Network's kill switch.** At start and every 30 minutes the pack fetches a policy signed by Meowy
  Network (`meowynetwork.com/pack/v1/policy.txt` and `.sig`), and the launcher does the same. It can switch
  off one feature, one version (or every version below a minimum) or the whole pack, for example while a
  crash is fixed. Your own settings are kept and come back afterwards. `meowy_policy_status` shows the
  policy in effect. The 30-minute check waits while a join is loading, and the `.txt` and the `.sig`
  are fetched back to back over one connection.
  - **Without "Start with the game"** the pack is loaded only when you join Meowy Network, and the game
    frees it again when you leave. Then each join fetches the policy once, with no 30-minute repeat, on a
    thread that keeps the pack loaded until that fetch is done.
  - **Each join** also reads the kept policy again (the launcher may have kept a newer one meanwhile), but
    only when `policy.txt` or `policy.sig` changed since it was last read; otherwise the policy in memory
    is used without touching the files. If the launcher is busy with them, the join doesn't wait more
    than 100 ms: it keeps the policy in memory and reads the files at the next join.
  - **Three signing keys.** The release key is in a TPM chip on Meowy Network's PC: it can't be copied off
    it and needs a PIN on every use. It may sign any policy. The ops key (meant for automatic pauses by
    our backend) and the recovery key are password-protected key files, so a copy could still be stolen.
    From those two, the pack and the launcher take only a stricter policy: the pack switched off, more
    features off or versions revoked, a higher minimum version. Their seq must stay below 10^15 (that range is the
    release key's alone) and at most 1000 above the policy you have, and they can't set a notice (only
    keep or drop the one you have). So a stolen copy of either can only switch things off, and the
    release key can always undo that with a higher seq.
  - A policy replaces the kept one only with a higher seq, so an old one can't be replayed.
  - **Before anything loads**, the start-up stub obeys `state\policy.bin` (the pack on or off, the minimum
    version and the revoked versions), written from the verified kept policy. The pack at start and the
    launcher when it starts both rewrite it when it differs, so one an older launcher wrote without the
    minimum version or the revoked list heals. They take a shared lock (`Local\MeowyClientPack.Policy`)
    around it and never write one with a lower seq than the one there.
  - **The way back from a bad policy** is `MeowyClientPack.exe --reset-policy` (see *Installing, starting
    and uninstalling*), only when Meowy Network says to.

## Everything it sends and writes

**Over the network** (the pack and the launcher):
- **meowynetwork.com, over HTTPS.** Like any website it sees your IP address. It doesn't log the pack's
  requests with it, and the server index uses it only to guess your rough location for ping estimates.
  The pack's requests share one connection where they can (kept open between requests, HTTP/2 where
  Windows has it), ask for gzip, never follow a redirect, and use only an answer that arrived whole.
  - `/pack/v1/policy.txt` and `.sig` (the kill switch): from the pack at start and every 30 minutes
    (without "Start with the game": once each time you join Meowy Network), and from the launcher every
    30 minutes while the game is closed.
  - `/pack/v1/release-stable.txt`, `.sig` and a new version's DLL (updates): a minute after start and every
    6 hours, and from the launcher while the game is closed; never with `meowy_updates 0` or with "Start
    with the game" off.
  - `/api/pack/index` (Meowy's server index): while Find servers is open, in the game or in the launcher.
  - `/api/pack/servers` (the honest list's verdicts) and `/api/pack/servers/report` (the reports from
    other servers described under *Honest server list*; `meowy_serverlist_reports 0` stops them). Since
    1.7.2 each report also carries a Steam ticket made for Meowy Network, so the site can ask Steam which
    account sent it and count each account once; it keeps only a salted hash of the account, in memory,
    for 6 hours.
  - The loading page (`pack-loading.html` and its files, through WebView2): loaded at start and kept
    hidden. On Meowy Network it sends your SteamID64 to `/api/player-loading-info` for your name and
    avatar.
- **Game servers,** with the game's standard server queries (the ones the server browser makes): server
  details for the home card, the home screen's lists, the Escape menu's card and the launcher's window,
  player lists for the honest server list, the `meowy_content` rule of a server you point at or pick, and,
  before a rejoin, whether the server is up.
- **Steam,** through the game's own `steam_api`: the server list, parties (a lobby and two Rich Presence
  keys), Workshop downloads, Timeline markers, and a ticket for Meowy Network with each server-list
  report (Meowy Network then asks Steam whose account it is; see *Honest server list*).
- **Discord,** only the Discord app on your PC, over its local pipe, while you're on Meowy Network. Discord
  then shows your job, the player count and how long you've played to everyone who can see your Discord
  activity, not only friends. Meowy settings → Discord status on Meowy Network (`meowy_discord 0`) turns
  it off and clears what Discord shows at once.
- **Meowy Network's game server,** over the game's own connection, when you join it: the Lua part of
  crash reports from crashes on Meowy Network only, and your hardware description (see the top of
  *Feature by feature*).

**On disk:**
- **In `%LOCALAPPDATA%\MeowyClientPack`:**
  - settings: `menu_settings.txt`, `settings_guard_session.txt`, `settings_guard_always.txt`,
    `cvar_restore.txt` (values the pack changes for a while), `repairs.txt`, `startup_off.txt`,
    `config_backups\`;
  - start-up: `boot.lock` and `recover.lock` (for a moment), `state\` (the kept policy, `policy.bin`, the
    update pointers and floors, `install_token`), `bin\<version>` (downloaded versions);
  - loading screen: `join_times.txt`, `join_times_servers.txt`, `last_join.txt`, `css_scale.txt` and
    WebView2's profile;
  - staying connected and Continue: `resilience.txt`, `on_server.txt`, `last_server.txt`,
    `meowy_crashes.txt`. These, `menu_settings.txt`, the settings guard's two files, `repairs.txt`,
    `cvar_restore.txt` and the console's history and pins are written whole (a temporary file, then renamed over the old one) but not forced to the
    disk at once: they survive the game crashing, and many are written while a join loads. The
    policy, the update pointers and the `config.cfg` backups are still flushed to the disk;
  - notifications: `notify.txt`, `notify_icon.png`, `notify_catalogue.txt`;
  - joins and servers: `joins\` (pre-warm), `pings.json`, `serverlist.log`;
  - the console: `console_history.txt`, `console_pins.txt`, `logs\`;
  - `vault\` (each server's `garrysmod/data`, unless the game is on another drive), `launcher\` (the
    launcher, its settings and log) and `uninstall\` (a copy of the installer, for Windows' list of apps).
- **In the game's folder:** the start-up line in `lua/includes/menu.lua`; `garrysmod/data` as a link
  into the vault; `garrysmod\cfg\meowy_restore.cfg`, only for the moment `meowy_settings_restore` puts a
  copy back; the game's own `config.cfg`, saved (`host_writeconfig`) when a settings-guard
  put-back from an earlier run or a restore needs it; and, from the launcher, the Workshop font cache
  renamed while the game starts.
- **In Windows' settings for your account:** the **Meowy Client Pack** entry in installed apps (see
  *From Windows' list of apps*), the launcher's start at sign-in (if chosen), the `meowy://`
  link handler, and the **Meowy Client Pack** Start-menu entry.
- Uninstalling removes all of it, except your `config.cfg`. Each server's
  `data/` files go to `Documents\Meowy Client Pack backup`, and a vault it can't empty, or can't put back
  because Garry's Mod is gone, is kept (see *Uninstall*).
