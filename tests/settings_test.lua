-- The pack's settings (src/lua/settings.lua) against a stubbed menu state and
-- console, no game:
--   lua tests/settings_test.lua        (from the repository root; Lua 5.1+)
-- The console is the engine's: RunConsoleCommand and meowy.ConsoleRun queue a
-- line, and a frame runs the queue before the menu's Think, the way the game
-- does; a server's client Lua queues lines the same way (w.server below).
-- What it can't show: the engine's exact handling of a command's quotes (both
-- forms are covered), or that Garry's Mod runs the menu's Think every frame.
local here = (arg and arg[0] or ""):match("^(.*)[/\\]") or "."
local function readFile(p) local f = assert(io.open(p, "rb")) local s = f:read("*a") f:close() return s end
local source = readFile(here .. "/../src/lua/settings.lua")
local loadstring = loadstring or load

local failures, count = 0, 0
local function check(name, got, want)
	count = count + 1
	if got ~= want then
		failures = failures + 1
		print(("FAIL %s: got %s, want %s"):format(name, tostring(got), tostring(want)))
	end
end

-- One console line as the engine runs it: commands split at ";" or a line
-- break outside quotes; a convar's value is the rest of its command, one pair
-- of surrounding quotes and trailing spaces dropped (Source's IsCommand).
local function execLine(w, line)
	local cur, quoted = {}, false
	local function finish()
		local name, rest = string.match(table.concat(cur), "^%s*(%S+)%s*(.-)$")
		cur = {}
		if not name then return end
		name = string.lower(name)
		if w.convars[name] == nil then return end
		local v = rest:gsub("%s+$", "")
		if v:sub(1, 1) == '"' then
			v = v:sub(2)
			if v:sub(-1) == '"' then v = v:sub(1, -2) end
		end
		w.convars[name] = v
		w.sets[#w.sets + 1] = name .. "=" .. v
	end
	for ch in string.gmatch(line, ".") do
		if ch == '"' then quoted = not quoted end
		if (ch == ";" and not quoted) or ch == "\n" then finish() else cur[#cur + 1] = ch end
	end
	finish()
end
-- RunConsoleCommand's line: each argument in quotes.
local function commandLine(cmd, ...)
	local parts = { cmd }
	for i = 1, select("#", ...) do parts[#parts + 1] = '"' .. tostring((select(i, ...))) .. '"' end
	return table.concat(parts, " ")
end

local function world(opts)
	opts = opts or {}
	local w = { now = 100, inGame = false, loading = false, convars = {}, flags = {}, queue = {}, prints = {}, sets = {},
		hooks = {}, concommands = {}, settingsText = opts.settings or "", policy = opts.policy, written = nil }
	local env = {
		string = string, table = table, math = math, pairs = pairs, ipairs = ipairs, tostring = tostring, tonumber = tonumber,
		select = select, type = type, setmetatable = setmetatable, pcall = pcall, error = error,
		isstring = function(v) return type(v) == "string" end, istable = function(v) return type(v) == "table" end,
		isfunction = function(v) return type(v) == "function" end,
		SysTime = function() return w.now end,
		print = function(s) w.prints[#w.prints + 1] = s end,
		IsInLoading = function() return w.loading end,
		IsInGame = function() return w.inGame end,
		FCVAR_ARCHIVE = 128,
		hook = { Add = function(_, id, fn) w.hooks[id] = fn end },
		concommand = { Add = function(n, fn) w.concommands[n] = fn end },
	}
	env.CreateConVar = function(name, value, flags)
		w.convars[name] = tostring(value)
		w.flags[name] = flags
		return {
			GetString = function() return w.convars[name] end,
			GetBool = function() return (tonumber(w.convars[name]) or 0) ~= 0 end,
			GetDefault = function() return "default" end,
			GetName = function() return name end,
		}
	end
	-- The menu's own RunConsoleCommand: queued, run with the next frame.
	env.RunConsoleCommand = function(cmd, ...) w.queue[#w.queue + 1] = commandLine(cmd, ...) end
	env.meowy = {
		MenuSettingsRead = function() return w.settingsText end,
		MenuSettingsWrite = function(t) w.written = t return true end,
		PolicyState = function() return w.policy end,
		ConsoleRun = function(line) w.queue[#w.queue + 1] = line return true end,
	}
	w.env = env
	local fn = assert(loadstring(source, "settings"))
	if setfenv then setfenv(fn, env) else fn = assert(load(source, "settings", "t", env)) end
	fn()
	-- A server's client Lua: its own RunConsoleCommand, queued on the same console.
	function w.server(cmd, ...) w.queue[#w.queue + 1] = commandLine(cmd, ...) end
	-- A frame: the console's queue, then the menu's Think.
	function w.frame(n)
		for _ = 1, n or 1 do
			local q = w.queue
			w.queue = {}
			for _, line in ipairs(q) do execLine(w, line) end
			w.hooks["MeowyClientPack.SaveSettings"]()
			w.now = w.now + 0.02
		end
	end
	function w.seconds(s) w.frame(math.ceil(s / 0.02)) end
	function w.saved(name) return (w.written or ""):match(name .. "=([^\n]*)") end
	function w.told(name)
		for _, p in ipairs(w.prints) do if p:find(name, 1, true) and p:find("put back", 1, true) then return true end end
		return false
	end
	return w
end

local function settings(w)
	local m = w.env.meowy
	return {
		sandbox = m.Setting("meowy_data_sandbox", "1", "help"),
		updates = m.Setting("meowy_updates", "1", "help"),
		guard = m.Setting("meowy_settings_guard", "1", "help"),
		always = m.Setting("meowy_settings_guard_always", "", "help"),
		party = m.Setting("meowy_party", "1", "help"),
		reports = m.Setting("meowy_serverlist_reports", "1", "help"),
		loading = m.Setting("meowy_loadingscreen", "1", "help"),
	}
end

-- Saved values come back; the first frame writes nothing.
local w = world({ settings = "meowy_updates=1\nmeowy_loadingscreen=0\nmeowy_settings_guard_always=1.2.3.4:27015" })
local s = settings(w)
check("saved: read back", w.convars.meowy_loadingscreen, "0")
check("saved: the list", s.always:GetString(), "1.2.3.4:27015")
w.frame()
check("saved: nothing written at start", w.written, nil)

-- Before any join this start, a change from anywhere is the player's (their
-- config files, the game's console): no server Lua has run yet.
w.queue[#w.queue + 1] = "meowy_updates 0"
w.frame()
check("before a join: the console's change taken", s.updates:GetBool(), false)
w.seconds(1.1)
check("before a join: saved", w.saved("meowy_updates"), "0")
w.queue[#w.queue + 1] = "meowy_updates 1"
w.frame()
check("before a join: and back", s.updates:GetBool(), true)

-- In game, a server's client Lua sets them: read as before at once, put back
-- through the console, never saved, logged once for the join.
w.inGame = true
w.frame()
w.server("meowy_data_sandbox", "0")
w.server("meowy_updates", "0")
w.server("meowy_settings_guard", "0")
w.server("meowy_settings_guard_always", "")
w.server("meowy_party", "0")
w.server("MEOWY_SERVERLIST_REPORTS", "0")
w.server("meowy_loadingscreen", "1")
local q = w.queue
w.queue = {}
for _, line in ipairs(q) do execLine(w, line) end
check("server: the sandbox still reads on, before any Think", s.sandbox:GetBool(), true)
check("server: updates still on", s.updates:GetBool(), true)
check("server: guard still on", s.guard:GetBool(), true)
check("server: the list kept", s.always:GetString(), "1.2.3.4:27015")
check("server: parties still on", s.party:GetBool(), true)
check("server: reports still on (any case)", s.reports:GetBool(), true)
check("server: an unguarded setting changes", s.loading:GetBool(), true)
w.seconds(1.1)
check("server: put back in the console too", w.convars.meowy_data_sandbox .. w.convars.meowy_updates .. w.convars.meowy_settings_guard .. w.convars.meowy_party, "1111")
check("server: the list put back", w.convars.meowy_settings_guard_always, "1.2.3.4:27015")
check("server: saved values unchanged", w.saved("meowy_data_sandbox") .. w.saved("meowy_updates") .. w.saved("meowy_settings_guard") .. w.saved("meowy_party"), "1111")
check("server: the list saved unchanged", w.saved("meowy_settings_guard_always"), "1.2.3.4:27015")
check("server: logged", w.told("meowy_data_sandbox"), true)
local told = #w.prints
for _ = 1, 20 do w.server("meowy_updates", "0") w.frame() end
check("server: the same setting again: logged once a join", #w.prints, told)
check("server: still on after 20 tries", s.updates:GetBool(), true)
w.seconds(0.5)
check("server: and the console back to on", w.convars.meowy_updates, "1")

-- A server that queues `disconnect` first: the change runs at the main menu
-- and is put back all the same.
w.server("disconnect")
w.server("meowy_data_sandbox", "0")
w.inGame = false   -- the engine ran `disconnect`
w.frame()
check("disconnect first: still on", s.sandbox:GetBool(), true)
w.frame(3)
check("disconnect first: put back", w.convars.meowy_data_sandbox, "1")
-- ... or `wait`s until long after, while the game's console is open.
w.seconds(30)
w.queue[#w.queue + 1] = "meowy_updates 0"
w.frame(3)
check("after a join: not from the menu, put back", s.updates:GetBool(), true)
check("after a join: the console back on", w.convars.meowy_updates, "1")

-- The menu's own changes: Meowy settings and the prompts (RunConsoleCommand),
-- the Meowy console (meowy.ConsoleRun); in game too.
w.inGame = true
w.frame()
w.env.RunConsoleCommand("meowy_updates", "0")
w.frame()
check("menu: Meowy settings' change taken", s.updates:GetBool(), false)
w.env.RunConsoleCommand("meowy_settings_guard_always", "1.2.3.4:27015 5.6.7.8:27016")
w.frame()
check("menu: the prompt's list taken", s.always:GetString(), "1.2.3.4:27015 5.6.7.8:27016")
w.env.meowy.ConsoleRun('echo hi; meowy_party "0";meowy_settings_guard 0')
w.frame()
check("menu: the Meowy console's line taken", tostring(s.party:GetBool()) .. " " .. tostring(s.guard:GetBool()), "false false")
w.env.meowy.ConsoleRun("meowy_settings_guard_always 9.9.9.9:1 8.8.8.8:2")
w.frame()
check("menu: a typed list, no quotes", s.always:GetString(), "9.9.9.9:1 8.8.8.8:2")
w.env.RunConsoleCommand("meowy_settings_guard_always", "7.7.7.7:1", "6.6.6.6:2")
w.frame()
check("menu: arguments the engine runs into one value with quotes inside: not kept", s.always:GetString(), "9.9.9.9:1 8.8.8.8:2")
w.frame(20)
check("menu: and put back", w.convars.meowy_settings_guard_always, "9.9.9.9:1 8.8.8.8:2")
w.seconds(1.1)
check("menu: saved", w.saved("meowy_updates") .. "/" .. w.saved("meowy_party") .. "/" .. w.saved("meowy_settings_guard_always"), "0/0/9.9.9.9:1 8.8.8.8:2")

-- An approval is for the value the menu set: a server setting another value
-- in the same frame is put back.
w.env.RunConsoleCommand("meowy_updates", "1")
w.server("meowy_updates", "5")
w.frame(2)
check("approval: only its own value", s.updates:GetString(), "0")
w.env.RunConsoleCommand("meowy_updates", "1")
w.frame()
check("approval: then its own value", s.updates:GetString(), "1")
-- ... and runs out: a later server change of that value is put back.
w.env.RunConsoleCommand("meowy_party", "1")
w.queue = {}    -- never ran, so the approval runs out unused
w.seconds(12)
w.server("meowy_party", "1")
w.frame(2)
check("approval: runs out", s.party:GetBool(), false)

-- A value the pack would never set (a second command in it) isn't kept even
-- from the menu, and isn't put back with it.
w.inGame = false
w.env.meowy.ConsoleRun('meowy_settings_guard_always "1.2.3.4:1;bind w kill"')
w.frame(3)
check("value: a list with a command in it isn't kept", s.always:GetString(), "9.9.9.9:1 8.8.8.8:2")
local q2 = table.concat(w.queue, "|")
check("value: what's put back is the kept value", q2 == "" or q2:find("bind", 1, true) == nil, true)

-- The policy kill switch still reads as off, and the player's own value is kept.
w = world({ settings = "meowy_party=1", policy = { off = { "meowy_party" }, seq = 1 } })
s = settings(w)
w.frame()
check("policy: a setting it turns off reads off", s.party:GetBool(), false)
check("policy: as a string", s.party:GetString(), "0")
w.loading = true
w.frame()
w.server("meowy_party", "0")
w.frame(2)
w.loading = false
w.seconds(1.1)
check("policy: the player's value kept", w.written == nil or w.saved("meowy_party") == "1", true)
check("policy: and still theirs", w.convars.meowy_party, "1")

-- A saved protected value the pack couldn't have set starts from the default;
-- the others are taken as saved.
w = world({ settings = 'meowy_settings_guard_always=1.2.3.4:1";bind w "kill\nmeowy_updates=abc\nmeowy_loadingscreen=2' })
s = settings(w)
check("saved: a bad list starts empty", s.always:GetString(), "")
check("saved: a bad number starts from the default", s.updates:GetString(), "1")
check("saved: others as saved", s.loading:GetString(), "2")

-- The menu's RunConsoleCommand still runs what it's given.
w = world()
s = settings(w)
w.env.RunConsoleCommand("cl_timeout", "120")
check("menu: other commands still sent", w.queue[1], 'cl_timeout "120"')

print(("%d checks, %d failed"):format(count, failures))
os.exit(failures == 0 and 0 or 1)
