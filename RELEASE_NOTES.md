**New in 1.7.2:** a Discord switch, uninstalling from Windows, and fixes. **Run the 1.7.2 installer once** to get the pack listed in Windows' installed apps: the in-game update can't add it.

- **Discord status switch.** Meowy settings → **Discord status on Meowy Network** turns it off. What Discord shows is cleared at once, and servers can't turn it back on.
- **Uninstall from Windows.** The installer adds **Meowy Client Pack** to Settings → Apps → Installed apps. Uninstall works there too, even after Garry's Mod is removed.
- **Fairer server-list reports.** Each report from another server now carries a Steam ticket, so Meowy Network counts each Steam account once, however many networks it reports from. Meowy Network asks Steam which account sent it and keeps only a scrambled form of it, for 6 hours.
- **Fixes.**
  - On installs that updated themselves, the `config.cfg` backups work again, and **Start with the game** shows the right setting in Meowy settings.
  - Putting back a `config.cfg` backup no longer leaves `meowy_restore.cfg` in the game's `cfg` folder.

Everything in detail: [How it works](https://github.com/MeowyNetwork/meowy-client-module/blob/main/docs/HOW-IT-WORKS.md).

**New in 1.7.1:** fixes from a full review of 1.7.0. **Run the 1.7.1 installer once**, even if the pack already updated itself in the game: the in-game update doesn't replace the Meowy Launcher from 1.7.0.

- **Safer against servers.**
  - Servers can no longer read the pack's settings, or switch off the ones that protect you: the data sandbox, updates, the settings guard, parties and the server-list reports.
  - Only crashes that happened on Meowy Network are sent to Meowy Network, never crashes on other servers.
  - You only join parties led by your Steam friends, and the party's next server isn't contacted until you follow.
  - The Meowy console no longer saves passwords you type, and a server can't blank it.
  - Putting back an old `config.cfg` works only at the main menu, after you confirm.
- **Fixes.**
  - Your own `garrysmod/data` files stay with the main menu, singleplayer and your own games, instead of being hidden on the first start. Files 1.7.0 set aside come back.
  - The settings guard asks about each server you were on. If the game closes while you're on a server, it asks at the next start.
  - The frame-rate limit the pack sets at the menu no longer sticks, and one 1.7.0 left behind is undone once.
  - "Rejoin the server?" after a crash on another server works.
  - The party countdown shows wherever the menu is, and a quick second move waits instead of being lost.
  - "Get ready" says when downloads failed, with **Try again**, and the home screen's server lists show it too.
  - The stay-connected prompt now says exactly what comes back after a drop.
- **Updates.**
  - Updates need **Start with the game**. If it's off, the launcher and the console now say so.
  - `meowy_updates 0` stops updates straight away.
  - The 32-bit and 64-bit game no longer hold each other's updates back.
  - Uninstall works after Garry's Mod is removed, and running the installer as administrator no longer leaves an administrator launcher in the tray.
  - The launcher shows the right version and puts the Workshop fonts back when you quit it.

Everything in detail: [How it works](https://github.com/MeowyNetwork/meowy-client-module/blob/main/docs/HOW-IT-WORKS.md).

**New in 1.7.0:** faster joins on every server, a new main menu and server browser, and a pack that keeps itself up to date. 1.6.0 was never released, so its changes are included here. Install this version once by hand; after that, new versions arrive on their own while "Start with the game" is on.

- **Faster joins, on any server.** Garry's Mod spends much of a join looking for files that aren't there and searching the same lists again and again. The pack cuts most of that out: in our tests, the first join to Meowy Network after starting the game went from over a minute to about 30 seconds. For servers you've played on (and Meowy Network), files and Workshop content are made ready at the main menu, before you click Join.
- **A new main menu.** A home screen with **Continue** (your last server), your friends' servers, your favourites and recent servers, and one search box for servers, maps, addons and settings. The menu is bigger on large screens, and in game the Escape menu shows the server you're on, with **Reconnect**.
- **A better server browser.** Find servers fills in within about a second and marks servers that fake their player count. It also adds a Friends tab, filters that are remembered, and **Join when a slot opens**. Servers Meowy Network has checked show **VERIFIED**.
- **Parties.** Start a party with your Steam friends. When the leader takes the party to a server, everyone follows after a short countdown, unless they click **Stay**.
- **Staying connected.** If your connection drops on Meowy Network, the pack can rejoin for you, and your job and jail time come back. It asks once whether you want this. After a crash, the next start offers to rejoin the server you were on, on any server.
- **The Meowy console.** A readable console, with search, autocomplete and an Errors tab that shows which addon is breaking.
- **Handy tools.**
  - **Settings guard:** asks before a server's changes to your settings stick.
  - Backups of your `config.cfg`.
  - **PC doctor:** finds the usual reasons the game runs slowly.
  - Sound follows your headphones when you plug them in.
  - The mouse stays inside the game window while you aim.
- **Our loading screen on other servers.** Servers without a loading page of their own get a simpler version of ours.
- **The Meowy Launcher.** The installer adds a small tray app. It starts the game a little faster, puts the pack back after a Steam update and offers to rejoin after a crash.
- **Updates and safety.**
  - The pack updates itself, only with versions Meowy Network has signed. A new version starts the next time you open the game.
  - If the game closes while a new version is starting, the pack goes back to the previous one.
  - `meowy_updates 0` turns updates off.
  - Meowy Network can switch a feature off for everyone while a bug is fixed.
- **Privacy.** Your Steam friends can see your party and that you have the pack (`meowy_party 0` turns that off). The new downloads from meowynetwork.com (the server list, updates and the kill switch) carry nothing about you; like any website, it sees your IP address, which the server list uses for rough ping estimates.

**Meowy settings**, in the main menu after Options, has switches for the menu, joins and staying connected; the rest are console settings.

**New in 1.5.2:** the pack's loading screen now shows on every join. Before, it only appeared the first time you joined after starting Garry's Mod; reconnecting or joining another server showed the game's default loading screen instead.

**New in 1.5.1:** Steam Timeline markers. If you use Steam's game recording, being arrested or released, losing a printer, finishing a contract and becoming wanted on Meowy Network now get a marker on the recording's timeline, so you can jump straight to those moments; joining one of our servers shows as loading, then playing. The markers' titles are the pack's own, at most one of each kind every 30 seconds. It uses the Steam that Garry's Mod already runs and only works on Meowy Network servers.

**New in 1.5:** three things.
- **Windows notifications while you're alt-tabbed.** When something happens to you in the city (your printer is under attack, you're wanted, staff replied to your ticket, the server restarts in 5 minutes) and the game isn't in front, Windows shows a notification, the game's taskbar button gets a badge and flashes, and clicking the notification brings the game back. The words are built into the pack, so a notification never shows names, amounts or links (it can appear on your lock screen). At most one of each kind every 10 seconds and 20 an hour; Windows' Do Not Disturb applies. The installer adds **Meowy Client Pack** to your Start menu, because Windows only shows notifications from apps listed there.
- **A separate `garrysmod/data` for every server.** Any server's Lua can read all of `garrysmod/data`, including other servers' files, whose names alone say where you've played. Now `data/` holds only the files of the server you're on; the rest wait in `%LOCALAPPDATA%\MeowyClientPack\vault`, out of reach of every server. Your existing files move to `vault\before-sandbox` the first time, and uninstalling puts them back (each server's own files go to `Documents\Meowy Client Pack backup`). `meowy_data_sandbox 0` turns it off.
- **An honest server list.** Some servers fake their player count to climb the server list. In Find Multiplayer Game the pack now checks each server's real player list and marks fakes as `[FAKE - N real]`, ranked by the real number, together with Meowy Network's own checks. On other servers you join it reports what `status` shows (other players' SteamIDs, pings and connection state, never your own row) so fakes can be spotted; the backend keeps only salted hashes, for 14 days. `meowy_serverlist_reports 0` stops the reports and `meowy_honest_serverlist 0` turns the whole check off.

Also: fixes from a security review of the pack (tighter limits on what server replies the pack reads, and a safer uninstall that never deletes your files).

**New in 1.4.1:** when you launch the game straight into the server (Steam join links), the screen you see for the first second while the loading page starts now looks just like the page, so there's no jump from a different-looking screen. The pack's own files (join times and the start-up check) moved from `garrysmod/data/meowy` to `%LOCALAPPDATA%\MeowyClientPack`: any server's Lua can read `garrysmod/data`, so other servers could tell you have the pack.

**New in 1.4:** our own loading screen. When you join a Meowy Network server, the pack shows its own screen instead of Garry's Mod's. It never freezes (Garry's Mod's own screen does for most of a join), shows each stage and a clock time for when you'll be in, and has things to do while you wait: *Where in Downtown?* (find where a photo was taken on the city map), *Rooftop run* (a cat running over the city's real rooftops that arrives when your game is ready) and *the city cat*. There's also a map of the city to explore, and Cancel works even while the game is busy. It's our web page from meowynetwork.com, shown with WebView2 (the browser built into Windows), and it appears only for joins to our servers and only in windowed or borderless mode; `meowy_loadingscreen 0` in the console turns it off. To make this possible the pack now **starts with the game**: the installer adds one line to `lua/includes/menu.lua` (removed on uninstall), and holding Shift while the game starts skips it once.

**New in 1.3:** faster joins. At the end of loading a server's Lua, Garry's Mod throws away every material and loads them all again; on Meowy Network's map that is 5-15 seconds of every join. The pack now skips that second load (the materials stay as they were loaded seconds earlier). It's an in-memory change to the game that is only active while you join a Meowy Network server, it switches itself off if Garry's Mod updates that code, and `meowy_skipmatreload 0` in the console turns it off. It also now only works on Meowy Network servers. It checks which server the game is really connected to (inside the DLL, not in Lua, so a server can't fake it). Before this, any server could have read your recent crash reports and hardware description, or changed your Discord status. Discord buttons now only accept https links.

**New in 1.2:** crash reports also say where the game crashed (the module and offset, e.g. `engine.dll+0x200660`), and the pack can describe your hardware (CPU, GPU, RAM, Windows version) so the server can see what players' machines struggle with. No serial numbers or user names are collected; the description is stored on our server with your Steam account ID so it can be matched with performance stats.

**New in 1.1:** when you join, the server receives the Lua part of any game crash reports since your last visit, so crashes caused by addons can be found and fixed. Only the "Lua Stack Traces" section is sent; nothing else from the report leaves your PC.

Download **MeowyClientPack.exe**, close Garry's Mod, run it and click **Install**.

The installer isn't code-signed yet: if Windows shows "Windows protected your PC", click **More info** → **Run anyway**.
File hashes are in `SHA256SUMS.txt`. `MeowyClientPack.exe` and the four DLLs inside the zip have a GitHub build attestation
(`gh attestation verify <file> --repo MeowyNetwork/meowy-client-module`); the zip itself and `SHA256SUMS.txt` don't.

Manual install: unzip `meowy-client-module-manual.zip` into `garrysmod/lua/bin/`.
