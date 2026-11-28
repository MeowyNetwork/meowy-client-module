-- The crash explainer's rules (src/lua/crash_logic.lua), no game:
--   lua tests/crash_logic_test.lua        (from the repository root; Lua 5.1+)
-- What it can't show: that a real report's fault was read right (info_smoke),
-- or that its reading of a crash is the true cause. It never claims that.
local here = (arg and arg[0] or ""):match("^(.*)[/\\]") or "."
local M = dofile(here .. "/../src/lua/crash_logic.lua")

local failures, count = 0, 0
-- check(name, got) wants true; check(name, got, want) wants that, nil included.
local function check(name, got, ...)
	count = count + 1
	local want = true
	if select("#", ...) > 0 then want = (...) end
	if got ~= want then
		failures = failures + 1
		print(("FAIL %s: got %s, want %s"):format(name, tostring(got), tostring(want)))
	end
end
local function has(text, part) return string.find(text, part, 1, true) ~= nil end

local NO_LUA = "-Lua Stack Traces-\n==================\n  Client\n    Lua Interface = NULL\n\n  Server\n\t*Not in Lua call OR Lua has panicked*\n\n"
	.. "  MenuSystem\n\t*Not in Lua call OR Lua has panicked*\n"
local CLIENT_LUA = "-Lua Stack Traces-\n==================\n  Client\n    1. DrawModel - [C]:-1\n     2. unknown - lua/includes/modules/halo.lua:150\n"
	.. "      3. Draw - addons/cool_hud/lua/cool_hud/cl_draw.lua:42\n       4. unknown - gamemodes/darkrp/gamemode/cl_init.lua:10\n\n"
	.. "  Server\n\t*Not in Lua call OR Lua has panicked*\n\n  MenuSystem\n    1. Think - lua/menu/menu.lua:5\n"

-- --- Modules -------------------------------------------------------------------
do
	check("no module", (M.module("")), "unknown")
	check("ours", (M.module("gmcl_meowy_win64.dll")), "pack")
	check("ours, the stub", (M.module("gmsv_meowy_boot_win32.dll")), "pack")
	check("another binary module", (M.module("gmcl_chttp_win64.dll")), "addon_module")
	check("it is named", select(2, M.module("gmcl_chttp_win64.dll")), "a binary module (gmcl_chttp_win64.dll)")
	check("NVIDIA", (M.module("nvd3dum.dll")), "gpu")
	check("NVIDIA, any case", (M.module("NVD3DUMX.DLL")), "gpu")
	check("AMD", (M.module("atidxx64.dll")), "gpu")
	check("Intel", (M.module("igd10iumd64.dll")), "gpu")
	check("Steam overlay", (M.module("GameOverlayRenderer64.dll")), "overlay")
	check("OBS", (M.module("graphics-hook64.dll")), "overlay")
	check("the browser", (M.module("html_chromium.dll")), "browser")
	check("the game", (M.module("client.dll")), "game")
	check("the renderer", (M.module("shaderapidx9.dll")), "game")
	check("Windows", (M.module("ntdll.dll")), "windows")
	check("something else keeps its name", select(2, M.module("weird.dll")), "weird.dll")
end

-- --- The Lua section -----------------------------------------------------------
do
	check("no Lua running", M.luaFrames(NO_LUA), nil)
	check("no text", M.luaFrames(nil), nil)
	local l = M.luaFrames(CLIENT_LUA)
	check("the first realm with frames", l.realm, "Client")
	check("its frames", #l.frames, 4)
	check("a frame's function", l.frames[3].fn, "Draw")
	check("a frame's file", l.frames[3].file, "addons/cool_hud/lua/cool_hud/cl_draw.lua")
	check("a frame's line", l.frames[3].line, 42)
	check("a C frame", l.frames[1].file, "[C]")
	check("whose code: the first addon named", l.source, "the addon \"cool_hud\"")
	local menu = M.luaFrames("  Client\n    Lua Interface = NULL\n  MenuSystem\n    1. Think - lua/menu/menu.lua:5\n")
	check("only the menu was in Lua", menu.realm, "MenuSystem")
	check("no addon named", menu.source, nil)
	local gm = M.luaFrames("  Client\n    1. x - gamemodes/terrortown/gamemode/cl_init.lua:3\n")
	check("a gamemode is named", gm.source, "the gamemode \"terrortown\"")
	local many = "  Client\n"
	for i = 1, 30 do many = many .. "    " .. i .. ". f - lua/a.lua:" .. i .. "\n" end
	check("only the top frames are kept", #M.luaFrames(many).frames, 8)
end

-- --- Readings ------------------------------------------------------------------
local function ex(module, code, lua, facts)
	return M.explain({ time = 1700000000, lua = lua or NO_LUA, fault_module = module, fault_offset = 0x1A2B3C, fault_code = code }, facts
		or { bits = 64, pack = "1.9.0", game = "261112/x86-64/x64", multicore = true })
end
do
	local e = ex("nvd3dumx.dll", 0xC0000005)
	check("gpu: what", has(e.what, "inside the NVIDIA graphics driver") and has(e.what, "access violation"))
	check("gpu: cause", has(e.cause, "graphics driver problem"))
	check("gpu: advice starts with the driver", has(e.advice[1], "Update the graphics driver"))
	check("gpu: multicore is mentioned when it's on", has(table.concat(e.advice, "|"), "Multicore rendering"))
	check("gpu: not when it's off", not has(table.concat(ex("nvd3dumx.dll", 0xC0000005, nil, { bits = 64, multicore = false }).advice, "|"), "Multicore"))

	e = ex("client.dll", 0xC0000005, nil, { bits = 32, pack = "1.9.0" })
	check("32-bit, plain access violation: not called out of memory", not has(e.cause, "ran out of memory"))
	check("32-bit: the 64-bit game is suggested", has(table.concat(e.advice, "|"), "x86-64"))
	e = ex("tier0.dll", 0xC0000409, nil, { bits = 32 })
	check("32-bit abort: possibly memory, said as possibly", has(e.cause, "Possibly the game ran out of memory"))
	check("32-bit abort: 64-bit first", has(e.advice[1], "x86-64"))
	e = ex("ntdll.dll", 0xC0000017, nil, { bits = 32 })
	check("Windows said no memory: most likely", has(e.cause, "Most likely the game ran out of memory"))
	e = ex("ntdll.dll", 0xC0000017)
	check("64-bit out of memory: other programs", has(e.advice[1], "Close other programs"))
	check("64-bit: no branch advice", not has(table.concat(e.advice, "|"), "x86-64"))
	e = ex("tier0.dll", 0xC0000409)
	check("64-bit abort isn't called out of memory", not has(e.cause, "memory."))

	e = ex("gmcl_meowy_win64.dll", 0xC0000005)
	check("ours: said plainly", has(e.cause, "inside the Meowy Client Pack itself"))
	check("ours: safe mode first", has(e.advice[1], "holding Shift"))
	check("ours: and asked to be sent", has(e.advice[2], "ours to fix"))
	e = ex("gmcl_chttp_win64.dll", 0xC0000005)
	check("another module: named", has(e.cause, "gmcl_chttp_win64.dll"))
	e = ex("GameOverlayRenderer64.dll", 0xC0000005)
	check("overlay: named", has(e.cause, "the Steam overlay") and has(e.advice[1], "turn off the Steam overlay"))
	e = ex("html_chromium.dll", 0x80000003)
	check("browser: a server's page", has(e.cause, "built-in web browser"))

	e = ex("lua_shared.dll", 0xC00000FD, CLIENT_LUA)
	check("stack overflow: with the addon", has(e.cause, "called itself without end") and has(e.cause, "the addon \"cool_hud\""))
	e = ex("client.dll", 0xC0000005, CLIENT_LUA)
	check("Lua running: the addon is the likely trigger", has(e.cause, "the addon \"cool_hud\"") and has(e.cause, "most likely trigger"))
	check("Lua running: never 'caused by'", not has(e.cause, "caused by"))
	e = ex("client.dll", 0xC0000005)
	check("game code, no Lua: no finger pointed", has(e.cause, "doesn't point at an addon"))
	e = ex("materialsystem.dll", 0xC0000005)
	check("renderer with multicore on: the switch is offered", has(table.concat(e.advice, "|"), "Multicore rendering off"))
	e = ex("materialsystem.dll", 0xC0000005, nil, { bits = 64, multicore = false })
	check("renderer with it off: not offered", not has(table.concat(e.advice, "|"), "Multicore"))
	e = ex("filesystem_stdio.dll", 0xC0000006)
	check("disk: verify the files", has(e.cause, "couldn't be read") and has(e.advice[1], "Verify integrity"))
	e = ex("", 0)
	check("nothing known: said so", has(e.what, "doesn't say where") and has(e.cause, "doesn't point at a likely cause"))
	check("always something to try or 'nothing to do'", #e.advice >= 1)
	e = ex("weird.dll", 0xDEADBEEF)
	check("an unknown code is shown as it is", has(e.what, "0xDEADBEEF"))
end

-- --- The text to copy ----------------------------------------------------------
do
	local e = ex("client.dll", 0xC0000005, CLIENT_LUA, { bits = 64, pack = "1.9.0", game = "261112/x86-64/x64", multicore = true, server = "Some RP | 1.2.3.4:27015", hooks_on = false })
	check("share: where", has(e.share, "Stopped in: client.dll+0x1A2B3C (0xC0000005)"))
	check("share: when, UTC", has(e.share, "2023-11-14 22:13 UTC"))
	check("share: the game and the pack", has(e.share, "Game: 64-bit, build 261112/x86-64/x64; Meowy Client Pack 1.9.0; multicore rendering on; join speed-ups off"))
	check("share: the server", has(e.share, "Server: Some RP | 1.2.3.4:27015"))
	check("share: the Lua frames", has(e.share, "  3. Draw - addons/cool_hud/lua/cool_hud/cl_draw.lua:42"))
	check("share: the reading", has(e.share, "Reading: "))
	check("share: no drive path", not string.find(e.share, "%a:[\\/]"))
	e = ex("client.dll", 0xC0000005)
	check("share: no Lua said", has(e.share, "Lua running: none"))
	check("share: no server line without one", not has(e.share, "Server:"))
end

-- --- How long ago --------------------------------------------------------------
do
	check("a minute", M.ago(1000, 990), "a minute ago")
	check("minutes", M.ago(10000, 10000 - 600), "10 minutes ago")
	check("an hour", M.ago(10000, 10000 - 4000), "an hour ago")
	check("hours", M.ago(100000, 100000 - 5 * 3600), "5 hours ago")
	check("yesterday", M.ago(1000000, 1000000 - 30 * 3600), "yesterday")
	check("days", M.ago(1000000, 1000000 - 5 * 86400), "5 days ago")
end

print(("%d checks, %d failures"):format(count, failures))
os.exit(failures == 0 and 0 or 1)
