# Meowy Client Pack

An optional add-on for players of the Meowy Network Garry's Mod server. It shows your Discord status
while you play: your job, the player count, time played, and a button friends can use to join.

Garry's Mod doesn't let servers or the Workshop send DLLs to players, so this is something you
install yourself. The server only checks whether it's there. Nothing changes if you don't install it.

## Install
1. Download `MeowyClientPack.exe` from the [latest release](https://github.com/MeowyNetwork/meowy-client-module/releases/latest).
2. Close Garry's Mod, run it, and click **Install**. It finds Garry's Mod through Steam.
3. Join the server.

The installer isn't code-signed yet, so Windows may show "Windows protected your PC". Click
**More info**, then **Run anyway**. You can check the files first: every release lists SHA256
hashes in `SHA256SUMS.txt`, and its files carry a GitHub build attestation proving they were built
from this repository by its CI (`gh attestation verify MeowyClientPack.exe --repo MeowyNetwork/meowy-client-module`).

**Manual install:** unzip `meowy-client-module-manual.zip` into
`steamapps/common/GarrysMod/garrysmod/lua/bin/` (create `bin` if it doesn't exist). To have it
start with the game, also add this line at the end of `garrysmod/lua/includes/menu.lua`:
`pcall(require, "meowy_boot") -- meowy-client-pack v1`

**Uninstall:** run the installer and click **Uninstall** (or `MeowyClientPack.exe --uninstall`). It
first puts back the `garrysmod/data` files the pack kept apart (see *A separate `garrysmod/data` for
every server* below): your own files, from before the pack and from the main menu, go back into
`garrysmod/data`, never overwriting anything there (on a name clash both are kept, and the one coming
back gets a suffix such as `name.before-pack.txt`). Each server's own files go to
`Documents\Meowy Client Pack backup\<server>`, not into `data/`, where every server could read them.
Then it removes the pack's files, its start-up line and `%LOCALAPPDATA%\MeowyClientPack`; a vault
that couldn't be emptied is left in place and named. Uninstalling by hand (deleting the four
`gmcl_meowy_*` and `gmsv_meowy_boot_*` files from that folder and the `meowy-client-pack` line from
`lua/includes/menu.lua`) leaves the vault where it is: copy `vault\before-sandbox` and `vault\menu`
back into `garrysmod/data` yourself.

**Start with the game** (on by default in the installer): the installer adds that one line to
`lua/includes/menu.lua`, so the pack is loaded at the main menu instead of only once a server's
Lua starts. This is what later join speed-ups and our own loading screen need. Untick it to keep
the old behaviour. If the game ever fails to start with it, hold **Shift** while Garry's Mod
starts (or add `-meowy_safe` to its launch options) and the pack isn't loaded at start-up. If
the game closes while the pack is starting, the next start skips it once by itself. A Steam
"Verify integrity of game files" removes the line; run the installer again to add it back.

## What it does, and doesn't
- It talks only to the Discord app on your PC, over Discord's local connection
  (`\\.\pipe\discord-ipc-N`), on its own thread so the game never waits on it.
- Since 1.1 it also reads the game's own crash reports (`GarrysMod/crashes/*.txt`), so the
  server can see which addon crashed your game. Only the "Lua Stack Traces" section is read
  (at most 3 reports per join, 4 KB each, any `C:\...` paths masked). The rest of the report,
  which names your install path, never leaves your PC.
- Since 1.2 each crash report also says where the game crashed, e.g.
  `engine.dll+0x200660 (0xC0000005)`, read from the game's own crash dump. Only the module's file
  name and the offset are used; the dump itself never leaves your PC.
- Since 1.2 it can describe your hardware: CPU and GPU model, thread count, RAM, video memory and
  Windows version. No serial numbers or user names. The server stores it with your Steam account
  ID, next to your frame-time stats, to see what players' machines struggle with.
- Since 1.3 it can make joins faster. At the end of loading a server's Lua, Garry's Mod throws
  away every material it has loaded and loads them all again; on a large map that is 5-15
  seconds of every join. When the server asks for it, the module skips the "throw away" step
  (anything new still loads). The game's materials stay exactly as they were loaded seconds
  earlier.
  - This is the one place the module changes the game: 6 bytes of `client.dll`, in memory, for
    that session only. Nothing on disk changes, and no other game is affected.
  - It finds that code by an exact byte pattern and does nothing if anything differs (a
    Garry's Mod update, for example).
  - Turn it off with `meowy_skipmatreload 0` in the console. A server can also turn it off for
    everyone.
  - It's switched on only while you join a Meowy Network server and put back as soon as you're
    in game, so other servers you visit in the same session are never affected.
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
    page never shows. Your name and Steam avatar are only passed to the page once the game's real
    connection is confirmed to be ours; for any other server it doesn't appear, or closes.
  - Only when the game is windowed or borderless. It stays until the game runs smoothly after you
    spawn. `meowy_loadingscreen 0` turns it off.
- **Honest server list.** Some servers fake their player count to climb Garry's Mod's server list: a
  rewritten query reply, bots hidden from the bot count, or idle accounts made only to sit there.
  The pack checks the counts in the in-game server list (Find Multiplayer Game):
  - It reads each populated server's player list twice, 45 seconds apart, the same query the
    browser makes when you click a server. A count far above the list, players whose time doesn't
    grow, or the same player joined at the same moment on two servers mark the server
    `[FAKE - N real]`; it then shows and ranks by N. A server that won't give its list shows
    `[LIST HIDDEN]`, with its count untouched.
  - It also downloads Meowy Network's own verdicts (`meowynetwork.com/api/pack/servers`), from a
    crawler that adds Steam's own count and the count's history.
  - **What it sends:** on a server you join that isn't ours, it runs the game's `status` command
    twice, about 90 seconds in and four minutes later. It sends meowynetwork.com the server's
    address, its advertised count, the number of bots `status` lists, and for each other account
    its SteamID, ping, packet loss, state and connected time. Your own row is left out (if the pack
    can't tell which row is yours, it sends nothing). The backend turns the SteamIDs into salted
    hashes as they arrive, never stores or logs them readable, and keeps the hashes 14 days, to spot
    the same accounts on a server around the clock. It judges behaviour only: hidden bots, flat
    pings, players stuck connecting, one account on two servers at once. A server is only marked from
    these reports when players from two different addresses report it.
  - `meowy_honest_serverlist 0` turns the whole check off; `meowy_serverlist_reports 0` keeps the
    checks but stops the reports. Our own servers are never checked. What each report did (counts and
    the HTTP result, never SteamIDs) goes to `%LOCALAPPDATA%\MeowyClientPack\serverlist.log`.
- Its only other network traffic is the loading page and its data, from meowynetwork.com, through WebView2. It
  writes only in `%LOCALAPPDATA%\MeowyClientPack` (`boot.lock` and `recover.lock` for a moment at start-up; for the
  loading screen `join_times.txt`, how long each stage of your last join took, `last_join.txt`,
  what that join went through, and `css_scale.txt`; for notifications `notify.txt`, your on/off
  switch, and `notify_icon.png`; and WebView2's profile). Nothing goes in
  `garrysmod/data`, which any server's Lua can read, so other servers can't tell you have the pack.
  Before 1.4.1 these files were in `garrysmod/data/meowy/`; the pack moves them on first start.
- **A separate `garrysmod/data` for every server.** Any server's Lua can list and read all of
  `garrysmod/data`: other servers' files (their names alone say where you've played) and anything else
  kept there. With the pack started with the game, `data/` holds only the files of the server you're on;
  every other server's, and the main menu's, wait in `%LOCALAPPDATA%\MeowyClientPack\vault\<server>`.
  If the game is on another drive, they wait in `steamapps\MeowyClientPackVault`. Both are out of
  reach of every file path GMod gives Lua. Switching is a rename, so it's instant. The first time,
  your existing `data/` goes to `vault\before-sandbox`, kept but never shown to a server; uninstalling
  puts it back (or copy anything back from there by hand). A connection that isn't a plain
  `a.b.c.d:port` (a Steam P2P game, say) gets a shared sandbox of its own (`vault\p2p`, `vault\other`),
  never the main menu's files; singleplayer and hosting a game yourself use the main menu's. After a
  crash, the next start puts everything back where it belongs, even when the pack itself is skipped
  (safe mode, or the skip after a crash at start-up): the start-up stub does that part on its own.
  The sandbox itself only runs while the pack is started with the game, though: in a session where
  it isn't (safe mode, the skip after a crash, or after a Steam "Verify integrity of game files"
  removed the start-up line), every server sees the same `data/`. `meowy_data_sandbox 0`
  turns it off (one `data/` for all servers again).
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
- It only works on Meowy Network servers. The DLL asks the game itself which server it's connected to
  and compares that with the addresses built into it, so another server can't use it or read anything
  from it, even by faking its name or address in Lua. In singleplayer, the main menu or on any other
  server every call returns `false` and a reason. At the main menu it only reports its version.
- Windows only for now (32-bit and 64-bit Garry's Mod).

## Lua API
```lua
if util.IsBinaryModuleInstalled("meowy") and pcall(require, "meowy") then
	meowy.DiscordStart("<discord application id>")
	meowy.DiscordSetActivity({
		details = "Police Officer",           -- first line
		state = "Playing on Meowy Network",   -- second line
		start = os.time(),                    -- shows time elapsed
		large_image = "https://...", large_text = "...",
		party_size = 14, party_max = 32,
		buttons = { { label = "Join", url = "https://..." } },  -- up to 2, https only
	})
end
-- meowy.VERSION, meowy.DiscordClear(), meowy.DiscordState(), meowy.DiscordLastError()
-- meowy.CrashReports(since_unix, max) -> { { time = <unix>, lua = "<stack traces>", fault = "engine.dll+0x... (0xC0000005)" }, ... }
-- meowy.Hardware() -> { cpu, threads, ram_mb, gpu, vram_mb, os }
-- meowy.SkipMaterialReload() -> true | false, reason   (call from a client autorun file)
-- meowy.RestoreMaterialReload(), meowy.MaterialReloadState() -> "patched" | "not patched" | "unsupported"
-- meowy.ServerAllowed() -> true | false, reason   (1.2.1+; every call above needs it to be true)
-- meowy.Notify(kind) -> true | false, reason   (1.5+; only while the game isn't in front, e.g.
--   if not system.HasFocus() then pcall(meowy.Notify, "printer_attacked") end. Reasons: "only works on
--   Meowy Network servers", "unknown kind", "disabled", "focused", "rate limited". Kinds: src/notify.cpp)
-- meowy.SetNotifyEnabled(bool) -> true | false, reason;  meowy.NotifyEnabled() -> bool | nil, reason
-- meowy.TimelineEvent(kind) -> true | false, reason   (a Steam Timeline marker, focused or not, e.g.
--   pcall(meowy.TimelineEvent, "arrested"). Kinds: arrested, released, printer_destroyed, hit_completed,
--   wanted_on_you. Reasons: "only works on Meowy Network servers", "unknown kind", "timeline unavailable",
--   "rate limited")
```
Identical updates are dropped, and updates are sent at most once every 4 s (Discord's rate limit).

## Building
Visual Studio 2022+ and CMake 3.20+:
```
cmake -S . -B build64 -A x64   && cmake --build build64 --config Release
cmake -S . -B build32 -A Win32 && cmake --build build32 --config Release
installer/build.ps1 -Win32 build32/Release/gmcl_meowy_win32.dll -Win64 build64/Release/gmcl_meowy_win64.dll
```
`-DMEOWY_BUILD_TESTS=ON` adds `ipc_smoke`, which checks the Discord connection without the game:
`ipc_smoke <discord application id> [seconds]`. It also adds `notify_smoke <scratch folder>` (the
notification rules), `notify_manual` (a real notification on this PC; see `tests/NOTIFY_MANUAL.md`) and
`timeline_smoke` (the Steam Timeline marker rules).
