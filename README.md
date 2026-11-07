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
`lua/includes/menu.lua`) leaves the vault where it is, and `garrysmod/data` stays a link to the main
menu's folder in it (it keeps working). To undo that too: with the game closed, delete the
`garrysmod/data` link (deleting a link never deletes what it points to), make an empty `data` folder,
and copy `vault\menu` and `vault\before-sandbox` into it.

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
  - It changes 6 bytes of `client.dll`, in memory, for that session only. Nothing on disk
    changes, and no other game is affected.
  - It finds that code by an exact byte pattern and does nothing if anything differs (a
    Garry's Mod update, for example).
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
  - It changes the file system's table of functions in memory (one entry), only while a join loads.
    Folders other things write to during a join (`data`, `download`, `cache`, `cfg`, Steam's workshop
    folder) are never listed, a write drops that folder's list, and a list kept from an earlier join
    is used again only if the folder hasn't changed since.
  - Turn it off with `meowy_fast_file_checks 0`. `meowy_fast_file_checks_status` shows the counts.
- The pack's own settings (`meowy_loadingscreen`, `meowy_data_sandbox`, `meowy_honest_serverlist`,
  `meowy_serverlist_reports`, `meowy_fast_lua_lookup`, `meowy_fast_lua_find`, `meowy_fast_fonts`,
  `meowy_skipmatreload_anyserver`) are kept in `%LOCALAPPDATA%\MeowyClientPack\menu_settings.txt`.
  The game doesn't save the main menu's settings itself, so before this they were back to their
  defaults at every start.
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
  - Turn it off with `meowy_fast_lua_find 0` (it switches at your next disconnect).
    `meowy_fast_lua_find_status` shows the walks and the names skipped.
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
  - It reads files only. Nothing is changed, and the game opens the same files as before.
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
      - Meowy Network's servers and servers you've joined before are got ready without asking: a first join
        then only checks the items instead of downloading them on the loading screen (measured: 0.03 s per
        prepared item instead of its download).
      - Any other server's row (the home card, and the server picked in Find servers) shows **Get ready ·
        1.3 GB** instead; nothing downloads until you click it. Over 4 GB always waits for the click.
      - While it downloads, the row shows **Getting ready · 412 MB of 1.3 GB**. `meowy_content_get_ready
        <address>` does the same from the console.
      - The rule is only a list of Workshop IDs: all it can make the pack do is download Workshop items, which
        Steam checks and the join checks again. The query is the one the server browser already makes.
    - **Measured** on our dev server, fresh start, with one 94 MB legacy item's unpacked copy deleted: the
      loading screen's Workshop step took 4.7 s without the check and 2.1 s with it (it unpacked the item
      at the menu; checking all 29 items took 3.0 s). An update day's downloads move the same way.
    - **Switch:** its own, in Meowy settings ("Update a server's Workshop items before you join",
      `meowy_prewarm_workshop`, on by default). `meowy_prewarm_status` shows the last check.
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
    crawler that adds Steam's own count and the count's history, and from an observer that joins
    servers: `[VERIFIED]` means it joined in the last 7 days and saw nothing fake; `[UNVERIFIED]`
    means the server refused it (a VPN or hosting block), which is not an accusation. Servers marked
    `[UNVERIFIED]` or `[LIST HIDDEN]` sit slightly lower in the list than otherwise-equal ones; none
    are hidden.
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
- **The Meowy console.** The console key (or `meowy_console`) opens the game's console in the pack's look, at
  the main menu or over the game:
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
    typed, its value, default, flags and help show above the line, with "Reset to default". Ctrl+R searches
    what you typed before, and that history is kept between starts.
  - **Pinned commands:** "+ Pin a command" keeps what's typed (or the last command) as a one-click chip;
    right-click a chip to unpin it. Kept between starts.
  - **Commands:** a typed line runs exactly as if typed into the game's console, through the engine.
    The menu's `RunConsoleCommand` refuses some commands, the console doesn't. Up and Down go through what
    you typed. The commands for the game's console window act on this one: `clear` empties it (what ran
    before it in the line goes, what runs after stays), `showconsole` leaves it open, and `hideconsole` and
    `toggleconsole` close it. `con_filter_enable 1` with `con_filter_text` / `con_filter_text_out` filters
    its lines as it does the game's console ("con_filter on" in the header).
  - **Window:** drag it by the title, resize it from the corner; the console key or Esc closes it. The header
    shows the frame rate and, in game, ping, loss and the server. Save writes the lines shown to
    `%LOCALAPPDATA%\MeowyClientPack\logs\console_<date>.txt` (paths hidden; the newest 20 kept).
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
  - Off in Meowy settings ("Find servers: only servers with players") or with `meowy_fast_serverlist 0`
    for every server, the game's way. `meowy_fast_serverlist_status` shows the lists open.
  - **A Friends tab**, after Local Network: the servers your Steam friends are on (the game's own friends
    list: servers only, no names), shown like the other tabs. It's there with the home screen on or off.
- Its only other network traffic is the loading page and its data, from meowynetwork.com, through WebView2. It
  writes only in `%LOCALAPPDATA%\MeowyClientPack` (`boot.lock` and `recover.lock` for a moment at start-up; for the
  loading screen `join_times.txt`, how long each stage of your last join took, `last_join.txt`,
  what that join went through, and `css_scale.txt`; for notifications `notify.txt`, your on/off
  switch, and `notify_icon.png`; for staying connected `resilience.txt`, your answer and the
  `cl_timeout` you had before, and `on_server.txt`, which server you're on, so a crash can
  offer a rejoin; for the main menu's Continue `last_server.txt`, the last server you played on; and
  WebView2's profile). Nothing goes in
  `garrysmod/data`, which any server's Lua can read, so the pack leaves no files there for other servers to
  find. (Like any binary module, a server's Lua can still ask whether it's installed.)
  Before 1.4.1 these files were in `garrysmod/data/meowy/`; the pack moves them on first start.
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
  The first time, your existing `data/` becomes `vault\before-sandbox`, kept but never shown to a server;
  uninstalling puts it back (or copy anything back from there by hand). A connection that isn't a plain
  `a.b.c.d:port` (a Steam P2P game, say) gets a shared folder of its own (`vault\p2p`, `vault\other`),
  never the main menu's files; singleplayer and hosting a game yourself use the main menu's. The folder is
  chosen from the game engine's own connection, never from anything a server prints. After a crash, the
  next start points the link back at the main menu's folder, even when the pack itself is skipped (safe
  mode, or the skip after a crash at start-up): the start-up stub does that part on its own. The switching
  only runs while the pack is started with the game, though: in a session where it isn't (safe mode, the
  skip after a crash), every server sees the main menu's folder, and after a Steam "Verify integrity of
  game files" removed the start-up line, whichever folder the link last pointed to. Links need an NTFS
  drive; on another kind (FAT32, exFAT) the pack puts your own files back in a plain `data/` and every
  server shares it. `meowy_data_sandbox 0` turns it off (every server sees the main menu's folder), and
  `meowy_data_sandbox_status` says where `data/` points.
- **Stay connected through drops** (1.6+). Once, at the main menu, the pack asks "Stay connected through
  drops?". Nothing changes unless you say yes. Then:
  - Garry's Mod waits 60 seconds for a dropped connection to come back instead of 30 (`cl_timeout 60`).
    Your previous value is saved and put back when you turn it off (unless you changed it yourself since).
    The game saves it to `config.cfg` when it closes normally; if it's lost (a crash, a config file), the
    pack sets 60 again at the next start, unless you've set a value of your own. The pack never rewrites
    `config.cfg` itself.
  - If you're disconnected from a Meowy Network server anyway, the pack rejoins it: "Reconnecting to Meowy
    Network... (attempt n)", with **Cancel**. The server keeps your job, jail time, wanted level and
    loadout for 3 minutes.
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
-- meowy.ExpectRestart(seconds) -> true, seconds | false, reason   (1.6+; the server restarts within
--   `seconds`, clamped to 30..600. If the connection ends before then, players who chose to stay
--   connected are rejoined once the server answers again, within 5 minutes)
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
notification rules), `notify_manual` (a real notification on this PC; see `tests/NOTIFY_MANUAL.md`),
`timeline_smoke` (the Steam Timeline marker rules), `lua_lookup_smoke [GarrysMod folder] [stub file]`
(the fast Lua lookup gives the game's answers on a fake string table, and finds its patch site in the
installed `client.dll` of the same bitness) `font_loads_smoke <scratch folder>` (the font pass's
gdi32 loads, on the test's own imports and a real font file), `fs_cache_smoke <scratch folder>` (which
paths the missing-file cache answers for, and its answers against real folders), `prewarm_smoke <scratch
folder>` (the join pre-warm's file lists, budget and cancelling) and `lua_find_smoke` (the fast `file.Find`'s
prefix rules and its patch stubs on a copy of the game's code).

Staying connected's and the PC doctor's menu Lua is in `src/lua/` (built into the DLL as resources). Their rules and menu
glue have offline tests, run by CI with Lua 5.1:
```
lua tests/resilience_logic_test.lua
lua tests/resilience_menu_test.lua
lua tests/doctor_logic_test.lua
```
`-DMEOWY_BUILD_TESTS=ON` also adds `doctor_smoke [power]`: the PC doctor's facts on this PC, after
rendering a little with Direct3D 11 on the default adapter (or with `power`, Windows' power-saving one),
so the per-process GPU time should point at that adapter.
