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
`steamapps/common/GarrysMod/garrysmod/lua/bin/` (create `bin` if it doesn't exist).

**Uninstall:** run the installer and click **Uninstall**, or delete `gmcl_meowy_win32.dll` and
`gmcl_meowy_win64.dll` from that folder.

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
- It makes no network connections, writes no files, and doesn't touch the game's memory.
- It only works on Meowy Network servers. The DLL asks the game itself which server it's connected to
  and compares that with the addresses built into it, so another server can't use it or read anything
  from it, even by faking its name or address in Lua. In singleplayer, the main menu or on any other
  server every call returns `false` and a reason.
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
