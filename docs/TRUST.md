# Is it safe? What the pack changes, and who you're trusting

This page is for someone deciding whether to install a native mod into their game. It says what the pack
changes in Garry's Mod, what that means for VAC, who can change the pack on your PC after you install it,
and how to check or undo all of it. The details behind each point are in [How it works](HOW-IT-WORKS.md).

## The short version

- The pack is a DLL that Garry's Mod loads. It changes how the game loads files and Lua, **in the game's own
  memory, while it runs**. Nothing in the game's program files is changed, except one added line in
  `lua/includes/menu.lua` that loads the pack at start.
- **Garry's Mod is protected by VAC, and nobody at Valve or Facepunch has reviewed or approved this pack.**
  Valve doesn't publish what VAC looks for. A mod that changes game code in memory is the kind of thing an
  anti-cheat can look at. We can't promise it will never be flagged. Installing it is your own risk.
- It gives no advantage in play: nothing it does shows you other players, aims, or times anything for you.
- The source is public and the builds are tied to it. The files are **not** code-signed for Windows yet, so
  Windows warns about the installer.
- After you install it, Meowy Network can send it new versions and can switch features off remotely. Both
  are signed, both are described below, and updates can be turned off.

## What it changes in the game

Each of these has a switch in **Meowy settings → Performance** (or the console name given), finds its
place by an exact match and does nothing if a Garry's Mod update moved things, and is gone when the game
closes.

| What | Where | How | Switch |
|---|---|---|---|
| Fast Lua lookup | `client.dll` | A jump at the start of the game's own search loop to a small routine that asks the list's own index instead | `meowy_fast_lua_lookup` |
| Fast `file.Find` | `client.dll` | Two jumps in the game's loop over a server's Lua files | `meowy_fast_lua_find` |
| Missing-file checks | `filesystem_stdio.dll` | The filesystem object is pointed at a copy of its function table with one entry (file open) replaced. No code bytes change | `meowy_fast_file_checks` |
| Decompress a map's files once | `filesystem_stdio.dll` | One call inside the game is pointed at the pack's routine | `meowy_pak_cache` |
| Font library kept loaded | `vguimatsurface.dll` | Two entries in that DLL's import table (`LoadLibraryA`, `FreeLibrary`) | `meowy_fast_fonts` |
| Pre-warm (which files a join read) | `filesystem_stdio.dll` | Two entries in that DLL's import table (`ReadFile`, `CloseHandle`) | `meowy_prewarm` |
| Skip the second material load | `client.dll` | Six bytes replaced with no-ops while a join loads, put back in game. Only on Meowy Network unless you opt in | `meowy_skipmatreload_anyserver` |

It also:
- sets `gmod_mcore_test 1` (multicore rendering) at start, a console setting of the game's own
  (`meowy_multicore`);
- adds one line to `lua/includes/menu.lua` so it starts with the game (the installer does this; Steam's
  "verify integrity of game files" removes it, and the pack still loads when you join Meowy Network);
- if you turn the data sandbox on, makes `garrysmod/data` a link to a folder per server. It is off unless
  you turn it on.

- keeps the mouse inside the game's window while you aim, with a Windows call (`meowy_mouse_confine`).

It does **not** touch Steam, VAC's own modules, other games or other processes, or anything about other
players. It reads no memory to learn something the game doesn't already show you.

## VAC and server anti-cheats

- **VAC.** See the short version: no approval, no promise. What we can say is what the pack does (the
  table above) and what it doesn't do. If you want certainty, don't install a native mod.
- **Servers can tell you have it.** A server's Lua can ask whether the pack's module is installed. Servers
  can't read the pack's settings or switch the protective ones off (see *Settings* in How it works).
- **A server's own anti-cheat** may treat any unknown module as suspicious. That is the server's rule to
  make. The pack doesn't hide from it.

## Who can change it after you install

Two things reach your PC from `meowynetwork.com` after installation:

1. **Updates.** A new version is downloaded only if its release document is signed with Meowy Network's
   release key (ECDSA P-256; the key is held in a hardware security chip, not in a file) and the file's
   SHA-256 matches. An update that takes the game down while the pack is starting is rolled back at the
   next start; a problem that shows up later is what the kill switch below is for.
   **Whoever holds that key can ship new code to every install that has updates on.** That is the trust
   you place in Meowy Network. Meowy settings → Privacy → **Keep the pack up to date** turns updates off.
2. **The kill switch.** A signed policy, checked every 30 minutes, can switch features or the whole pack
   off, refuse versions with a known problem, and show a short notice. It can only turn things off. It
   keeps working with updates off, so a broken feature can be stopped for everyone.

Nothing else is accepted from the network as instructions. Servers you join can't update the pack or
change what it does beyond the functions listed in [DEVELOPERS.md](DEVELOPERS.md), most of which only
work on Meowy Network's own servers.

## What leaves your PC

Listed in full, with what each request carries, under
[Everything it sends and writes](HOW-IT-WORKS.md#everything-it-sends-and-writes). The one that says
something about where you play is the server-list report (who was really connected on other servers you
join), which the welcome window asks about and Meowy settings → Privacy turns off.

## Checking the files

- **Source:** this repository, MIT licence. Releases are built by its CI, not on someone's PC.
- **Build attestation:** the installer, the launcher and the four DLLs carry a GitHub attestation that
  they were built from this repository:
  `gh attestation verify MeowyClientPack.exe --repo MeowyNetwork/meowy-client-module`
- **Hashes:** `SHA256SUMS.txt` in each release.
- **No Windows code signature yet.** Windows SmartScreen says "Windows protected your PC" for the
  installer, and Smart App Control may block it. That is about the missing signature, not about what the
  file does; check it with the two points above before choosing **Run anyway**.

## Turning it off, and removing it

- **One start without it:** hold **Shift** while Garry's Mod starts, or add `-meowy_safe` to its launch
  options.
- **One feature:** its switch in Meowy settings.
- **All of it:** Windows' Settings → Apps → Installed apps → **Meowy Client Pack** → Uninstall, or run the
  installer again and click **Uninstall**. It removes the DLLs and the start-up line and puts your own
  `garrysmod/data` back.
