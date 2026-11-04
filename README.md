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

**Uninstall:** run the installer and click **Uninstall**, or delete the four `gmcl_meowy_*` and
`gmsv_meowy_boot_*` files from that folder and the `meowy-client-pack` line from
`lua/includes/menu.lua`.

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
  - **What it sends:** once per server you join that isn't ours, about 90 seconds in, it runs the
    game's `status` command and sends meowynetwork.com the server's address, its advertised count,
    the number of bots `status` lists, and the SteamIDs it lists. The backend looks the accounts up
    on Steam (age, level, games owned, public profiles only) and keeps only the server's tally,
    e.g. "38 of 86 look made to boost"; the SteamIDs are never stored or logged. A server is only
    marked from these reports when players from two different addresses report it.
  - `meowy_honest_serverlist 0` turns the whole check off; `meowy_serverlist_reports 0` keeps the
    checks but stops the reports. Our own servers are never checked.
- Its only other network traffic is the loading page and its data, from meowynetwork.com, through WebView2. It
  writes only in `%LOCALAPPDATA%\MeowyClientPack` (`boot.lock` for a moment at start-up; for the
  loading screen `join_times.txt`, how long each stage of your last join took, `last_join.txt`,
  what that join went through, and `css_scale.txt`; and WebView2's profile). Nothing goes in
  `garrysmod/data`, which any server's Lua can read, so other servers can't tell you have the pack.
  Before 1.4.1 these files were in `garrysmod/data/meowy/`; the pack moves them on first start.
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
`ipc_smoke <discord application id> [seconds]`.
