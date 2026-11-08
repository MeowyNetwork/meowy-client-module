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

## Menu-state Lua (the pack's own chunks)
The main menu's Lua is in `src/lua/` and built into the DLL as resources. No server's Lua can call its
functions; a server can only run the console commands it registers, so each command that matters checks
where it's run from or asks the player first.
`settings.lua` (resource 140) runs first, then `module.cpp`'s chunk, then the rest.
```lua
-- meowy.Setting(name, default, help) -> setting   one object per name: asked for again, the first comes
--   back. Hidden (FCVAR_HIDDEN), kept in menu_settings.txt, read through the policy; the names in
--   settings.lua's PROTECTED are put back when anything but the menu changes them after a join.
--   GetBool/GetInt/GetFloat/GetString read the policy-aware, approved value.
-- meowy.SettingOf(name) -> setting | nil   another chunk's setting, read the way its owner reads it
--   (the policy and the put-back included). Never read a pack setting through GetConVar's raw value.
-- meowy.SettingsList() -> { { name, value, default, help }, ... }   for the Meowy console's completion
-- meowy.MenuFileRead(kind) -> text ("" when there's none)
-- meowy.MenuFileWrite(kind, text) -> ok   ("" removes the file). Kinds, and nothing else:
--   "settings_guard_session", "settings_guard_always", "repairs" (the .txt of that name)
-- meowy.ConsoleRun(line)   runs a line as typed into the console; counts as the menu's own change
```
In the menu state `GetConVar`, `GetConVarString` and `ConVarExists` answer a pack setting's name with its
setting object, since the hidden convars are invisible to the raw functions there too.

Files the pack keeps in `%LOCALAPPDATA%\MeowyClientPack` (HOW-IT-WORKS lists them all), added in 1.7.1:
`meowy_crashes.txt` (crash reports written while on Meowy Network, the only ones sent),
`settings_guard_session.txt`, `settings_guard_always.txt`, `repairs.txt` (one-time repairs done) and
`state\release_seq_<arch>.txt` (the updater's seq floor per architecture; 1.7.0's `release_seq.txt` isn't
read any more).

## Building
Visual Studio 2022+ and CMake 3.20+:
```
cmake -S . -B build64 -A x64   && cmake --build build64 --config Release
cmake -S . -B build32 -A Win32 && cmake --build build32 --config Release
installer/build.ps1 -Win32 build32/Release/gmcl_meowy_win32.dll -Win64 build64/Release/gmcl_meowy_win64.dll -Boot32 build32/Release/gmsv_meowy_boot_win32.dll -Boot64 build64/Release/gmsv_meowy_boot_win64.dll -Launcher launcher/target/release/MeowyLauncher.exe
```
`-Boot32` and `-Boot64` (the start-up stubs) are required; `-Launcher` is optional (build it as below).
The version is `src/version.h`'s, for both DLLs,
both stubs and the launcher; the installer's own is `AssemblyVersion` in `installer/Installer.cs`.

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
prefix rules and its patch stubs on a copy of the game's code). `steam_ticket_smoke` checks the server-list
report's Steam ticket offline; `steam_ticket_smoke --live <GarrysMod folder>` gets a real one from the Steam
client on this PC through the game's `steam_api`, and with `STEAM_API_KEY` set asks Steam's Web API whose it
is, as the backend does (so it says whether a key can check Garry's Mod tickets; neither is printed).

The menu Lua's rules and glue have offline tests, run by CI with Lua 5.1 (each `tests/*_test.lua`,
including `settings_test.lua`, `settings_guard_logic_test.lua`, `console_logic_test.lua`,
`content_logic_test.lua` and `party_logic_test.lua`), and the pages have Node tests:
```
for f in tests/*_test.lua; do lua "$f"; done
node tests/home_page_test.js
node tests/launcher_card_test.js
```
`-DMEOWY_BUILD_TESTS=ON` also adds `doctor_smoke [power]`: the PC doctor's facts on this PC, after
rendering a little with Direct3D 11 on the default adapter (or with `power`, Windows' power-saving one),
so the per-process GPU time should point at that adapter. The other smoke tests (`policy_smoke`,
`updater_smoke`, `sandbox_smoke`, `session_smoke`, `launcher_pipe_smoke` and the rest in `tests/`) and
`uninstall_smoke.cs` (the installer's rules, built with `csc`) run in CI as `.github/workflows/build.yml`
shows, most of them on both bitnesses.

## Releasing
1. **Version:** `src/version.h` (`MEOWY_VERSION` and its three numbers), `AssemblyVersion` in
   `installer/Installer.cs`, and the example in `src/launcher_pipe.h`'s comment. The launcher takes its
   version from `version.h` (`launcher/app/build.rs`). Add the new version's section at the top of
   `RELEASE_NOTES.md`: the whole file becomes the GitHub release's text.
2. **Tag** `vX.Y.Z` on main. CI builds, attests the four DLLs, `MeowyLauncher.exe` and
   `MeowyClientPack.exe` (not the zip or `SHA256SUMS.txt`), and creates the GitHub release with the
   installer, the manual zip and `SHA256SUMS.txt`.
3. **Sign the release for the updater** with `tools/release/make-release.ps1` (the TPM release key; it asks
   for the PIN). It lays out `pack/v1/` to upload to meowynetwork.com (the Website repo's `public/pack/v1`,
   then a frontend deploy): the DLLs first, `release-stable.txt` and `.sig` last.
   - **Seq:** `-Seq` must be above the published document's. The same seq is accepted only to sign the very
     same version and files again (both updaters may take the floor's document again when its file went
     missing, so two different documents must never share a seq). The script reads the published document
     and stops if it can't, unless `-Offline` (then the seq isn't checked).
   - **Salt** (which installs are in a rollout): `-Salt <hex>`, else the salt of the published document
     (or `-OutDir`'s) when it names this version, else a new random one. `-NewSalt` forces a new one. If it
     can read neither, it stops and asks for `-Salt` or `-NewSalt`. So signing the same version again keeps
     the installs that already had it, and a rollout only grows.
   - **Rollout in steps:** sign again with a higher `-Seq` and a bigger `-Rollout`; the salt stays.
   - **Expiry:** the document expires after `-Days` (30 by default). Sign it again before then (same
     version and files, same seq or higher), or installs that haven't taken it report that the release
     document has expired. Put the date in the calendar when you publish (1.7.0's expires 2026-12-08).
4. **Tell older packs:** Meowy Network's server says a newer version exists through `LATEST_VERSION` in
   the DarkRP repo's `addons/meowy_native_modules/lua/autorun/client/cl_meowy_client_module.lua`.
5. **Ask players to run the installer** when the launcher changed: the pack's in-game updates replace only
   the pack, never the Meowy Launcher.

### The kill switch's policy
`tools/policy/sign-policy.ps1 policy.txt` signs `policy.txt` (LF line ends) and writes `policy.sig`.
Publish both under `pack/v1/` like a release.
- **Release key** (default; the TPM, with its PIN): any policy, but its seq must be above the published one.
- **Ops and recovery keys** (`-Key ops|recovery -KeyFile <file>`, which asks for the file's password):
  password-protected PKCS#8 files from `tools/policy/new-keys.ps1`. The pack and the launcher (1.7.1+)
  take from them only a policy that is
  stricter than the one kept (the pack on stays on or goes off, nothing leaves `off` or `revoked`,
  `min_version` only rises), with a seq below 10^15 and at most 1000 above the kept one (at most 1000 on
  an install that has none), and with no notice other than the kept one. The script checks all of that
  against the published policy (or `-Kept <policy.txt>`) and refuses to sign what installs would refuse;
  if the published policy can't be read, it stops and asks for `-Kept`.
- **Keep seqs counting up by one.** A fresh install with no kept policy takes an ops or recovery policy
  only up to seq 1000, so fewer than 1000 published policies keep every case working.
- **Key passwords.** `new-keys.ps1` asks for a password for each of the two files (at least 12 characters)
  and writes them encrypted (AES-256, PBKDF2-SHA256 at 600,000 rounds). For a file made without one,
  `tools/policy/protect-key.ps1 -KeyFile <file>` adds a password in place, after checking the new file
  opens as the same key. Copies made before that still have no password: back the file up again and
  delete them. A lost password loses that key; the release key can still undo anything it signed.
  `sign-policy.ps1` still signs with a file that has no password, with a warning.
  `tests/policy_key_test.ps1` checks all of this on throwaway keys.
- The recovery key is meant to live off the PC that holds the release key.
- A player stuck on a bad policy can run `MeowyClientPack.exe --reset-policy` (game closed): it forgets
  the kept policy, and the next fetch takes the published one.
