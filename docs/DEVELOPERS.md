# Developing the Meowy Client Pack

The Lua API servers use, and how to build and test the pack. How each feature works is in
[HOW-IT-WORKS.md](HOW-IT-WORKS.md).

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
-- meowy.NotifyCatalogue(body, sig) -> true, version | false, reason   (1.6+; our signed kind list from
--   DarkRP's tools/notify_catalogue/build.py. Reasons: "bad signature", "bad catalogue: ...",
--   "older catalogue". See src/notify_catalogue.h)
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
The Meowy Launcher (Rust, Tauri 2; no tauri-cli or Node needed) and its tests; pass it to the installer
with `-Launcher launcher/target/release/MeowyLauncher.exe`:
```
cd launcher
cargo test -p meowy-launcher-core
cargo build -p meowy-launcher --release
```
`node tests/launcher_card_test.js` checks its crash card's text; `launcher/tools/measure_startup.ps1` times
click to pack start-up with and without it.

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
