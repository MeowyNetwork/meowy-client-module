-- What a crash was, in plain words: pure functions (no GMod API), so they can
-- be tested with a plain Lua interpreter (tests/crash_logic_test.lua).
-- Lua 5.1 compatible: Garry's Mod runs LuaJIT.
--
-- Garry's Mod writes a report for a crash (crashes\<id>.txt and .dmp), and the
-- pack reads from it where the game stopped (a module's file name, an offset
-- and Windows' exception code) and what Lua was running. That says a lot to
-- someone who has read many of these and nothing to a player. M.explain turns
-- it into: what kind of crash it was, what most likely caused it, what to
-- try, and a short text to paste when asking for help.
--
-- It is a reading of the evidence, never a verdict: the place a program stops
-- is often not where the mistake was made. The wording says "most likely" and
-- "while", and the advice starts with the cheapest thing to try.
-- Nothing here is sent anywhere; the text is for the player to copy.

local M = {}

-- Windows' exception codes seen in practice, in plain words.
local CODES = {
	[0xC0000005] = { "access", "it used memory it wasn't allowed to (an access violation)" },
	[0xC00000FD] = { "stack", "it ran out of stack (usually something calling itself without end)" },
	[0xC0000409] = { "abort", "the program stopped itself after finding its own memory damaged" },
	[0xC0000374] = { "heap", "its memory was found damaged (heap corruption)" },
	[0xE06D7363] = { "exception", "an error inside the program wasn't handled (a C++ exception)" },
	[0xC000001D] = { "instruction", "it ran an instruction this processor doesn't have" },
	[0xC0000094] = { "divide", "it divided by zero" },
	[0x80000003] = { "break", "it stopped on purpose at a check that failed" },
	[0xC0000017] = { "memory", "Windows had no memory left to give it" },
	[0xC000012D] = { "memory", "Windows had no memory left to give it" },
	[0xC0000006] = { "disk", "a file it was using couldn't be read from the disk" },
}

-- Who a module belongs to, by file name (lower case). The first match counts.
-- kind: "pack" (ours), "gpu", "overlay", "addon_module", "browser", "game",
-- "windows", or "unknown".
local MODULES = {
	{ "^gmcl_meowy_", "pack", "the Meowy Client Pack" },
	{ "^gmsv_meowy_", "pack", "the Meowy Client Pack" },
	{ "^nvd3dum", "gpu", "the NVIDIA graphics driver" }, { "^nvldumd", "gpu", "the NVIDIA graphics driver" },
	{ "^nvwgf2um", "gpu", "the NVIDIA graphics driver" }, { "^nvoglv", "gpu", "the NVIDIA graphics driver" },
	{ "^atidxx", "gpu", "the AMD graphics driver" }, { "^aticfx", "gpu", "the AMD graphics driver" },
	{ "^atiumd", "gpu", "the AMD graphics driver" }, { "^amdxx", "gpu", "the AMD graphics driver" },
	{ "^amdxc", "gpu", "the AMD graphics driver" },
	{ "^igd", "gpu", "the Intel graphics driver" }, { "^igc", "gpu", "the Intel graphics driver" },
	{ "^gameoverlayrenderer", "overlay", "the Steam overlay" },
	{ "^discordhook", "overlay", "the Discord overlay" }, { "^discord_hook", "overlay", "the Discord overlay" },
	{ "^rtsshooks", "overlay", "RivaTuner / MSI Afterburner's overlay" },
	{ "^graphics%-hook", "overlay", "OBS's game capture" },
	{ "^medal%-hook", "overlay", "Medal's overlay" },
	{ "^nahimic", "overlay", "Nahimic audio software" },
	{ "^reshade", "overlay", "ReShade" }, { "^dxvk", "overlay", "DXVK" },
	{ "^owclient", "overlay", "Overwolf's overlay" }, { "^overlay64", "overlay", "an overlay" },
	{ "^html_chromium", "browser", "the game's built-in web browser" }, { "^libcef", "browser", "the game's built-in web browser" },
	{ "^chrome_elf", "browser", "the game's built-in web browser" },
	{ "^gmcl_", "addon_module", "a binary module (%s)" }, { "^gmsv_", "addon_module", "a binary module (%s)" },
	{ "^lua_shared", "game", "the game's Lua engine" },
	{ "^client%.dll$", "game", "the game (client)" }, { "^server%.dll$", "game", "the game (server)" },
	{ "^engine%.dll$", "game", "the game's engine" }, { "^materialsystem", "game", "the game's material system" },
	{ "^shaderapidx9", "game", "the game's renderer" }, { "^stdshader", "game", "the game's shaders" },
	{ "^studiorender", "game", "the game's model renderer" }, { "^datacache", "game", "the game's model cache" },
	{ "^vphysics", "game", "the game's physics" }, { "^filesystem_stdio", "game", "the game's file system" },
	{ "^vgui", "game", "the game's interface" }, { "^gameui", "game", "the game's interface" },
	{ "^menusystem", "game", "the game's menu" }, { "^soundemittersystem", "game", "the game's sound" },
	{ "^vaudio", "game", "the game's sound" }, { "^tier0", "game", "the game's core library" },
	{ "^vstdlib", "game", "the game's core library" }, { "^gmod%.exe$", "game", "the game" }, { "^hl2%.exe$", "game", "the game" },
	{ "^steam_api", "game", "Steam's library" }, { "^steamclient", "game", "Steam's library" },
	{ "^d3d9", "windows", "Direct3D (Windows' graphics)" }, { "^d3dx9", "windows", "Direct3D (Windows' graphics)" },
	{ "^dxgi", "windows", "Direct3D (Windows' graphics)" },
	{ "^ntdll", "windows", "Windows" }, { "^kernelbase", "windows", "Windows" }, { "^kernel32", "windows", "Windows" },
	{ "^ucrtbase", "windows", "Windows' C library" }, { "^msvcr", "windows", "Windows' C library" },
	{ "^vcruntime", "windows", "Windows' C library" }, { "^msvcp", "windows", "Windows' C library" },
	{ "^win32u", "windows", "Windows" }, { "^user32", "windows", "Windows" }, { "^gdi32", "windows", "Windows" },
}

-- A module's file name -> kind, and who that is in words.
function M.module(name)
	local n = string.lower(tostring(name or ""))
	if n == "" then return "unknown", "somewhere outside any loaded program file" end
	for _, m in ipairs(MODULES) do
		if string.find(n, m[1]) then return m[2], (string.gsub(m[3], "%%s", tostring(name))) end
	end
	return "unknown", tostring(name)
end

-- The report's Lua section -> { realm = "Client" | "Server" | "MenuSystem",
-- frames = { { fn, file, line }, ... }, source = who that code belongs to },
-- for the first realm that was running Lua; nil when none was.
function M.luaFrames(trace)
	local realm, found
	for line in string.gmatch((type(trace) == "string" and trace or "") .. "\n", "([^\n]*)\n") do
		local name = string.match(line, "^%s*(%a+)%s*$")
		if name == "Client" or name == "Server" or name == "MenuSystem" then
			if found then break end
			realm = name
		elseif realm then
			local fn, file, ln = string.match(line, "^%s*%d+%.%s+(.-)%s+%-%s+(.-):(%-?%d+)%s*$")
			if fn then
				found = found or { realm = realm, frames = {} }
				if #found.frames < 8 then found.frames[#found.frames + 1] = { fn = fn, file = file, line = tonumber(ln) or 0 } end
			end
		end
	end
	if not found then return nil end
	-- Whose code: the first frame that names an addon or a gamemode.
	for _, f in ipairs(found.frames) do
		local path = string.lower(string.gsub(f.file, "\\", "/"))
		local addon = string.match(path, "^addons/([^/]+)/")
		local gamemode = string.match(path, "^gamemodes/([^/]+)/")
		if addon then found.source = "the addon \"" .. addon .. "\"" break end
		if gamemode then found.source = "the gamemode \"" .. gamemode .. "\"" break end
	end
	return found
end

local function hex(n) return string.format("0x%08X", n % 4294967296) end

-- report: { time, lua, fault_module, fault_offset, fault_code } (meowy.LastCrash).
-- facts: { bits = 32 | 64, pack = version, game = build, multicore = bool,
--   server = name or address or nil, hooks_on = bool }.
-- Returns { title, what, cause, advice = { ... }, share } in plain words.
function M.explain(report, facts)
	facts = facts or {}
	local code = tonumber(report.fault_code) or 0
	local known = CODES[code]
	local codeKind = known and known[1] or (code == 0 and "none" or "other")
	local kind, who = M.module(report.fault_module)
	local lua = M.luaFrames(report.lua)
	local bits = facts.bits == 32 and 32 or (facts.bits == 64 and 64 or nil)

	local what
	if code == 0 then
		what = "Garry's Mod closed unexpectedly. The report doesn't say where."
	else
		what = "Garry's Mod stopped inside " .. who .. ": " .. (known and known[2] or ("Windows' error " .. hex(code))) .. "."
	end

	local cause, advice = nil, {}
	local function add(text) advice[#advice + 1] = text end

	if codeKind == "memory" or (bits == 32 and (codeKind == "abort" or codeKind == "exception") and kind ~= "pack" and kind ~= "addon_module") then
		-- The 32-bit game can use about 3 GB however much the PC has; running
		-- out shows as these codes in almost any module.
		if bits == 32 then
			-- Windows said so itself, or it only looks like it (the 32-bit game
			-- giving up after an allocation failed): the wording differs.
			cause = (codeKind == "memory" and "Most likely the game ran out of memory." or "Possibly the game ran out of memory: on the 32-bit game that is the usual reason for this kind of stop.")
				.. " The 32-bit game can only use about 3 GB, whatever the PC has, and big servers go past that."
			add("Switch to the 64-bit game: in Steam, right-click Garry's Mod, Properties, Betas, and pick x86-64. That is the fix for this in most cases.")
		else
			cause = "Most likely Windows ran out of memory for the game."
			add("Close other programs that use a lot of memory (browsers above all) and check that Windows' page file isn't turned off.")
		end
		add("Lower texture quality, or leave servers with very many addons sooner after a long session.")
	elseif kind == "gpu" then
		cause = "Most likely a graphics driver problem: the game stopped inside " .. who .. "."
		add("Update the graphics driver from the maker's own site, or go back one version if this started after an update.")
		add("If you use an overlay or a recording tool, try once without it.")
		if facts.multicore then add("Turn off Multicore rendering in Meowy settings, Performance, and see if it stops.") end
	elseif kind == "overlay" then
		cause = "Most likely " .. who .. ": the game stopped inside it."
		add("Close or turn off " .. who .. " for Garry's Mod and try again.")
	elseif kind == "browser" then
		cause = "Most likely the game's built-in web browser, which servers use for loading screens, menus and media players."
		add("Try again: these are often one-offs caused by a web page a server showed.")
		add("If it keeps happening on one server, it's that server's page or media player: tell its owner.")
	elseif kind == "pack" then
		cause = "The game stopped inside the Meowy Client Pack itself."
		add("Start the game holding Shift (the pack's safe mode) to play without it for now.")
		add("Please send the text below to Meowy Network: this is ours to fix.")
	elseif kind == "addon_module" then
		cause = "Most likely " .. who .. ", a program file that an addon or a server's requirement installed into the game."
		add("If you installed it yourself, update or remove it (garrysmod\\lua\\bin).")
	elseif codeKind == "stack" then
		cause = "Something called itself without end until the game ran out of room." .. (lua and lua.source and (" It happened while Lua from " .. lua.source .. " was running.") or "")
		add("If it happens again at the same moment, it's that addon's or server's code: tell its author.")
	elseif codeKind == "disk" then
		cause = "A game file couldn't be read. That points at the disk or at damaged game files, not at an addon."
		add("In Steam: Garry's Mod, Properties, Installed Files, Verify integrity of game files.")
		add("If it keeps happening, check the disk the game is on.")
	elseif codeKind == "instruction" then
		cause = "A program file used a processor feature this PC's processor doesn't have, or the file is damaged."
		add("In Steam: Garry's Mod, Properties, Installed Files, Verify integrity of game files.")
	elseif lua and lua.source then
		cause = "It happened while Lua from " .. lua.source .. " was running, which makes that code the most likely trigger."
		add("If it happens again in the same situation, tell the addon's or server's author; the text below has what they need.")
	elseif lua then
		cause = "It happened while the game was running Lua (" .. string.lower(lua.realm == "MenuSystem" and "the main menu's" or (lua.realm .. " side")) .. "), but not in a named addon."
	elseif kind == "game" or kind == "windows" then
		cause = "The game stopped in its own code with no Lua running. On its own that doesn't point at an addon, the server or the pack."
	else
		cause = "The report doesn't point at a likely cause."
	end

	-- What's worth trying whatever it was, cheapest first.
	if kind ~= "pack" and codeKind ~= "disk" then
		if bits == 32 and #advice == 0 then
			add("You're on the 32-bit game. The 64-bit one (Steam, Garry's Mod, Properties, Betas, x86-64) crashes less on big servers.")
		end
		if facts.multicore and kind == "game" and (string.find(string.lower(tostring(report.fault_module or "")), "^materialsystem") or string.find(string.lower(tostring(report.fault_module or "")), "^shaderapidx9") or string.find(string.lower(tostring(report.fault_module or "")), "^studiorender")) then
			add("It stopped in the renderer with multicore rendering on. If it happens again, turn Multicore rendering off in Meowy settings, Performance, and see if it stops.")
		end
		if #advice == 0 then
			add("If it was a one-off, there's nothing to do. If it repeats in the same place, verify the game's files in Steam.")
		end
	end

	-- The text to paste: the facts, no paths, no account.
	local share = { "Garry's Mod crash" .. (report.time and (" on " .. os.date("!%Y-%m-%d %H:%M UTC", report.time)) or "") }
	share[#share + 1] = "Stopped in: " .. (tostring(report.fault_module or "") ~= "" and tostring(report.fault_module) or "no module")
		.. string.format("+0x%X", tonumber(report.fault_offset) or 0) .. " (" .. hex(code) .. ")"
	share[#share + 1] = "Reading: " .. cause
	share[#share + 1] = "Game: " .. (bits and (bits .. "-bit") or "?") .. (facts.game and (", build " .. tostring(facts.game)) or "")
		.. "; Meowy Client Pack " .. tostring(facts.pack or "?")
		.. "; multicore rendering " .. (facts.multicore and "on" or "off")
		.. (facts.hooks_on == false and "; join speed-ups off" or "")
	if facts.server and facts.server ~= "" then share[#share + 1] = "Server: " .. tostring(facts.server) end
	if lua then
		share[#share + 1] = "Lua running (" .. lua.realm .. "):"
		for i, f in ipairs(lua.frames) do share[#share + 1] = string.format("  %d. %s - %s:%d", i, f.fn, f.file, f.line) end
	else
		share[#share + 1] = "Lua running: none"
	end

	return {
		title = "Garry's Mod closed unexpectedly",
		what = what,
		cause = cause,
		advice = advice,
		share = table.concat(share, "\n"),
		kind = kind,
		code = codeKind,
	}
end

-- How long ago, for "the last crash was ...".
function M.ago(now, t)
	local s = math.max(0, (tonumber(now) or 0) - (tonumber(t) or 0))
	if s < 90 then return "a minute ago" end
	if s < 3600 then return math.floor(s / 60 + 0.5) .. " minutes ago" end
	if s < 2 * 3600 then return "an hour ago" end
	if s < 86400 then return math.floor(s / 3600 + 0.5) .. " hours ago" end
	if s < 2 * 86400 then return "yesterday" end
	return math.floor(s / 86400 + 0.5) .. " days ago"
end

return M
