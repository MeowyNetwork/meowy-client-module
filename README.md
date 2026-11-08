# Meowy Client Pack

A free, optional add-on for Garry's Mod from Meowy Network. It makes joining servers faster, gives you
a better main menu and server browser, and adds a few tools that make the game nicer to play. Most of it
works on every server, not just ours.

Garry's Mod doesn't let servers send add-ons like this to players, so you install it yourself. It runs
on Windows, with 32-bit or 64-bit Garry's Mod. If you don't install it, nothing changes: you can still
play on Meowy Network.

## Install

1. Download **MeowyClientPack.exe** from the [latest release](https://github.com/MeowyNetwork/meowy-client-module/releases/latest).
2. Close Garry's Mod.
3. Run the file and click **Install**. It finds Garry's Mod through Steam. Leave **Start with the game**
   ticked: the pack needs it to update itself.

Then start the game as usual. From then on the pack keeps itself up to date (see [Updates](#updates)).

Windows may say "Windows protected your PC", because the installer isn't code-signed yet. Click
**More info**, then **Run anyway**. To check the file first, see
[checking the download](docs/HOW-IT-WORKS.md#installing-starting-and-uninstalling).

## What you get

### Faster joins, on any server

While Garry's Mod loads a server, it wastes time looking for files that don't exist, searching the same
lists again and again, and downloading Workshop content while you wait. The pack cuts most of that out.
In our tests, the first join to Meowy Network after starting the game went from over a minute to about
30 seconds.

When you're about to join a server you've played on before (or Meowy Network), the pack gets its files
and Workshop content ready at the main menu, so there's less left to do on the loading screen.

### A loading screen that doesn't freeze

On Meowy Network you get our own loading screen. It shows each step and when you'll be in, and has a few
small games to pass the time. Other servers that don't have their own loading page get a simpler
version. It works in windowed and borderless mode.

### A new main menu

The main menu gets a home screen with:
- **Continue**, to join your last server in one click;
- Meowy Network's player count;
- the servers your friends are on, your favourites and your recent servers;
- one search box for servers, maps, addons and settings.

On big screens the menu is no longer tiny. In game, the Escape menu shows the server you're on and your
ping, with a **Reconnect** button and quick sliders for volume and field of view.

### A better server browser

**Find servers** fills in within about a second instead of a minute and a half. Servers that fake their
player count are marked **FAKE** and show the real number. You also get:
- a Friends tab;
- filters that stay set between starts;
- **Join when a slot opens** for full servers.

### Parties

Start a party with your Steam friends from the home screen. When the leader takes the party to a server,
everyone follows after a 10-second countdown. Anyone can click **Stay** instead. You only ever join
parties led by your Steam friends.

### Staying connected

- If your connection drops on Meowy Network, the pack can rejoin for you. Back within 3 minutes, you
  usually keep your job and jail time, and your weapons too unless you were just in a fight. It asks once
  whether you want this.
- After a crash, the next start offers to rejoin the server you were on. It only joins when you click.

### Handy tools

- **The Meowy console:** the game's console, but readable, with search, autocomplete and an Errors tab
  that shows which addon is breaking. Passwords you type in it aren't saved.
- **Settings guard:** if a server changes your graphics or other settings, you're asked when you leave
  whether to keep them or put them back. If the game closes while you're on that server, you're asked at
  the next start. Your `config.cfg` is backed up, too.
- **PC doctor:** checks the usual reasons the game runs slowly (the wrong graphics card, battery saver,
  the 32-bit game) and explains how to fix them. It only looks; it changes nothing.
- **Sound follows your headphones** when you plug them in mid-game.
- **The mouse stays in the game window** while you aim, so a fast flick can't click your other screen.
- **Each server gets its own `garrysmod/data` folder,** so a server can't read the files other servers
  saved there. Your own files stay with the main menu, singleplayer and the games you host.

### Extras on Meowy Network

- Your Discord status shows your job and the player count, with a **Join** button. Anyone who can see
  your Discord activity sees it, not just your friends. **Discord status** in Meowy settings turns it off.
- Windows notifications when something happens to you while you're alt-tabbed, such as your printer
  being attacked or staff replying to your ticket.
- Markers on Steam's game recording when you're arrested, become wanted and so on, so you can find the
  moment later.

### The Meowy Launcher

The installer also adds a small app that sits in the tray. It:
- starts Garry's Mod a little faster;
- puts the pack back after a Steam update or file check;
- offers to rejoin after a crash;
- shows servers while the game is closed.

It doesn't change the running game. To make it start faster, its **Play** turns off Lua autorefresh
(only addon makers use it) and moves the Workshop fonts aside until the pack has loaded. "Faster start"
in its window turns that off. You can stop it starting with Windows, either in the installer or in its own
window. In-game updates don't update the launcher; running a newer installer does.

## Changing things

**Meowy settings**, in the main menu after Options, has switches for staying connected, starting with the
game, getting servers ready, the console key, the menu, Discord status and the settings guard. Everything
else, including the other privacy settings below, is a console setting, listed in
[How it works](docs/HOW-IT-WORKS.md#settings).

To type console settings, turn the console on first: **Options → Keyboard → Advanced → Enable developer
console**. Then press the key under Esc.

If the game ever won't start with the pack, hold **Shift** while Garry's Mod starts: the pack is skipped
for that start. If the game crashes while the pack is starting, the next start skips it on its own.

## Updates

The pack checks for new versions itself, as long as **Start with the game** is on. If you unticked it,
run the installer again with it ticked. The pack only uses a new version that Meowy Network has signed,
and it switches to it the next time you open the game, never in the middle of a session. If the game
closes while a new version is starting, the pack goes back to the one before. To turn updates off, type
`meowy_updates 0` in the console.

Meowy Network can also switch a feature off for everyone, for example while a bug is being fixed. Your
own settings come back afterwards.

## Your privacy

This is what leaves your PC, and where it goes:
- **Only on Meowy Network:** when you join, the part of crash reports from crashes on Meowy Network that
  shows which addon crashed (never crashes on other servers), and a short description of your hardware
  (CPU, graphics card, memory, Windows version; no serial numbers or names). Both are saved with your
  Steam ID.
- **To Discord, on Meowy Network:** your job, the player count and how long you've played, if the Discord
  app is running. Anyone who can see your Discord activity sees it. **Discord status** in Meowy settings
  turns it off.
- **On other servers:** to catch fake player counts, the pack sends Meowy Network the server's player
  list as the game's `status` command shows it: other players' Steam IDs, pings and times, never yours.
  The Steam IDs are scrambled (hashed) as they arrive and deleted after 14 days. Each report also
  carries a Steam ticket, so Meowy Network can ask Steam which account sent it and count each account
  once; it keeps only a scrambled form of your account, for 6 hours. Reports never mark a server as
  fake by themselves. `meowy_serverlist_reports 0` stops this.
- **To your Steam friends:** your party, and the fact that you have the pack. `meowy_party 0` turns that
  off.
- **Downloads from meowynetwork.com:** the loading screen, the server list and updates. Like any website,
  it sees your IP address, and the server list uses that to guess your rough location for ping estimates.
  The site doesn't log the pack's requests with your IP. On Meowy Network, the loading screen sends it
  your Steam ID to show your name and picture.

Servers can't read the pack's settings, or change the ones above. Everything else stays on your PC,
almost all of it in `%LOCALAPPDATA%\MeowyClientPack`. The pack changes the game only in memory, while it
runs. On disk it adds one line to `garrysmod/lua/includes/menu.lua` (so it starts with the game) and
turns `garrysmod/data` into a link to each server's own folder. Uninstalling undoes both.

Every detail, feature by feature, is in [How it works](docs/HOW-IT-WORKS.md).

## Uninstall

Uninstall **Meowy Client Pack** in Windows' **Settings → Apps → Installed apps** (it's listed once you've
run the 1.7.2 installer or newer), or run **MeowyClientPack.exe** again and click **Uninstall**. It puts
your own `garrysmod/data` files back,
saves each server's files to `Documents\Meowy Client Pack backup`, and then removes the pack, the launcher
and everything they stored. If you've already removed Garry's Mod, it still removes the launcher and the
pack's own files.

## More

- [How it works](docs/HOW-IT-WORKS.md): every feature in detail, what it changes, and its console setting.
- [For developers and server owners](docs/DEVELOPERS.md): the Lua API, building and tests.
- [Content platform](docs/CONTENT_PLATFORM.md): how a server can have its Workshop content ready before
  players join.
- [Release notes](RELEASE_NOTES.md): what changed in each version.
