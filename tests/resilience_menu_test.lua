-- Connection resilience's menu glue (src/lua/resilience.lua) against a stubbed
-- Garry's Mod menu state, no game. It runs the same joined chunk module.cpp runs:
--   lua tests/resilience_menu_test.lua        (from the repository root; Lua 5.1+)
-- What it can't show: that the engine's IsTimingOut / GetTimeSinceLastReceived
-- read correctly in game, or that the menu state's Derma looks right.
local here = (arg and arg[0] or ""):match("^(.*)[/\\]") or "."
local function readFile(p) local f = assert(io.open(p, "rb")) local s = f:read("*a") f:close() return s end
local chunkText = "local Logic = (function()\n" .. readFile(here .. "/../src/lua/resilience_logic.lua")
	.. "\nend)()\n" .. readFile(here .. "/../src/lua/resilience.lua")
local loadstring = loadstring or load

local failures, count = 0, 0
local function check(name, got, want)
	count = count + 1
	if got ~= want then
		failures = failures + 1
		print(("FAIL %s: got %s, want %s"):format(name, tostring(got), tostring(want)))
	end
end

local OURS = "138.201.62.52:27015"

-- A fresh fake menu state per scenario.
local function world(opts)
	local w = {
		now = 0, state = "none", address = nil, timingOut = nil, silence = nil, inGame = false, loading = false,
		restartUntil = nil, commands = {}, settingsText = opts.settings or "", crashed = opts.crashed,
		cl_timeout = "30", pingAnswer = true, pingPass = false, buttons = {}, think = nil,
	}
	local env = setmetatable({}, { __index = _G })
	env.meowy = {
		ResilienceWatch = function()
			if w.state == "ours" then return "ours", w.address, w.timingOut, w.silence end
			return w.state
		end,
		ResilienceRestartExpected = function() return w.restartUntil ~= nil and w.now <= w.restartUntil end,
		ResilienceClearRestart = function() w.restartUntil = nil end,
		ResilienceSettingsRead = function() return w.settingsText end,
		ResilienceSettingsWrite = function(t) w.settingsText = t return true end,
		ResilienceSession = function(on) w.session = on end,
		ResilienceCrashedSession = function() return w.crashed end,
		IsOurServer = function(a) return type(a) == "string" and a:match("^138%.201%.62%.52:%d+$") ~= nil end,
	}
	env.SysTime = function() return w.now end
	env.IsInGame = function() return w.inGame end
	env.IsInLoading = function() return w.loading end
	env.GetConVar = nil
	env.GetConVarString = function(n) return n == "cl_timeout" and w.cl_timeout or "" end
	env.RunConsoleCommand = function(cmd, ...)
		w.commands[#w.commands + 1] = table.concat({ cmd, ... }, " ")
		if cmd == "cl_timeout" then w.cl_timeout = (...) end
	end
	env.hook = { Add = function(event, id, fn) if event == "Think" then w.think = fn end end }
	env.serverlist = { PingServer = function(addr, cb)
		w.pinged = addr
		if w.pingAnswer then cb(30, "Meowy", "", "map", 10, 64, 0, w.pingPass) end
	end }
	env.concommand = { Add = function(name, fn) w.concommands = w.concommands or {} w.concommands[name] = fn end }
	env.Color = function(r, g, b, a) return { r, g, b, a } end
	env.draw = { RoundedBox = function() end }
	env.ScrW = function() return 1920 end
	env.isnumber = function(v) return type(v) == "number" end
	env.isstring = function(v) return type(v) == "string" end
	env.ErrorNoHalt = function(m) w.errors = (w.errors or "") .. m end
	env.IsValid = function(p) return p ~= nil and not p.removed end
	local function panel(class)
		local p = { class = class }
		return setmetatable(p, { __index = function(t, k)
			if k == "SetText" then return function(self, s) self.text = s if self.class == "DButton" then w.buttons[#w.buttons + 1] = self end end end
			if k == "SetTitle" then return function(self, s) self.title = s end end
			if k == "Remove" or k == "Close" then return function(self) self.removed = true end end
			if k == "SetValue" then return function(self, v) self.value = v end end
			if type(k) == "string" and k:match("^%u") then return function() end end
		end })
	end
	env.vgui = { Create = function(class) local p = panel(class) w.panels = w.panels or {} w.panels[#w.panels + 1] = p return p end }
	local fn = assert(loadstring(chunkText, "resilience"))
	if setfenv then setfenv(fn, env) else fn = assert(load(chunkText, "resilience", "t", env)) end
	fn()

	function w.run(seconds)
		local stop = w.now + seconds
		while w.now < stop do
			w.now = w.now + 0.1
			w.think()
		end
	end
	function w.click(text)
		for i = #w.buttons, 1, -1 do
			local b = w.buttons[i]
			if b.text == text and b.DoClick then b.DoClick() return true end
		end
		return false
	end
	function w.connects()
		local n = 0
		for _, c in ipairs(w.commands) do if c:match("^connect ") then n = n + 1 end end
		return n
	end
	function w.lastConnect()
		for i = #w.commands, 1, -1 do if w.commands[i]:match("^connect ") then return w.commands[i] end end
	end
	-- On our server, in game, packets flowing.
	function w.play(seconds)
		w.state, w.address, w.timingOut, w.silence, w.inGame, w.loading = "ours", OURS, false, 0.02, true, false
		w.run(seconds or 5)
	end
	-- The server goes silent; the engine gives up after `after` seconds.
	function w.drop(after)
		local t = 0
		while t < (after or 60) do
			w.run(0.5)
			t = t + 0.5
			w.silence, w.timingOut = t, t > 4
		end
		w.state, w.inGame = "none", false
	end
	-- Packets flowing right up to the end (kick, ban, quit, map change).
	function w.cut() w.state, w.inGame = "none", false end
	return w
end

-- Opt-in: asked once, Yes sets cl_timeout 60 and remembers the old value
local w = world({})
w.run(4)
check("asked at start", w.click("Yes"), true)
check("cl_timeout set", w.cl_timeout, "60")
check("saved", w.settingsText, "asked=1\r\nenabled=1\r\nsaved_cl_timeout=30\r\n")
local w2 = world({ settings = w.settingsText })
w2.run(4)
check("not asked twice", w2.click("Yes"), false)

-- No changes nothing
w = world({})
w.run(4)
w.click("No thanks")
check("No: cl_timeout untouched", w.cl_timeout, "30")
check("No: remembered", w.settingsText, "asked=1\r\nenabled=0\r\n")

-- Settings window: turning it off restores cl_timeout
w = world({ settings = "asked=1\r\nenabled=1\r\nsaved_cl_timeout=30\r\n" })
w.cl_timeout = "60"
w.run(4)
w.concommands.meowy_settings()
local box
for _, p in ipairs(w.panels) do if p.class == "DCheckBoxLabel" then box = p end end
check("checkbox shows on", box.value, true)
box:OnChange(false)
check("off restores", w.cl_timeout, "30")
check("off saved", w.settingsText, "asked=1\r\nenabled=0\r\n")

local ON = "asked=1\r\nenabled=1\r\nsaved_cl_timeout=30\r\n"

-- The server went silent: rejoins the address the engine had, after asking the server
w = world({ settings = ON })
w.play(10)
w.drop(60)
w.run(1.5)
local overlayShown = false
for _, p in ipairs(w.panels) do if p.class == "DPanel" and not p.removed then overlayShown = true end end
check("timeout: overlay up", overlayShown, true)
w.run(3)
check("timeout: pinged our address", w.pinged, OURS)
check("timeout: joined", w.lastConnect(), "connect " .. OURS)
check("timeout: one join", w.connects(), 1)

-- Silent only briefly (under 10 s) when it ended: not a drop
w = world({ settings = ON })
w.play(10)
w.drop(5)
w.run(60)
check("short silence: no rejoin", w.connects(), 0)

-- Kick, ban, quit, map change: packets were flowing, nothing
w = world({ settings = ON })
w.play(10)
w.cut()
w.run(60)
check("packets flowing: no rejoin", w.connects(), 0)

-- Engine sample unreadable: nothing
w = world({ settings = ON })
w.play(10)
w.timingOut, w.silence = nil, nil
w.run(1)
w.cut()
w.run(60)
check("no sample: no rejoin", w.connects(), 0)

-- Not opted in: nothing
w = world({ settings = "asked=1\r\nenabled=0\r\n" })
w.play(10)
w.drop(60)
w.run(60)
check("opted out: no rejoin", w.connects(), 0)

-- Another server: nothing (the address never comes from anywhere else)
w = world({ settings = ON })
w.state, w.address, w.timingOut, w.silence, w.inGame = "other", nil, true, 60, true
w.run(5)
w.cut()
w.run(60)
check("other server: no rejoin", w.connects(), 0)

-- Cancel stops it for good
w = world({ settings = ON })
w.play(10)
w.pingAnswer = false
w.drop(60)
w.run(1.5)
check("cancel button there", w.click("Cancel"), true)
w.pingAnswer = true
w.run(200)
check("cancelled: no join", w.connects(), 0)

-- Server never answers: gives up within 3 minutes, no join
w = world({ settings = ON })
w.play(10)
w.pingAnswer = false
w.drop(60)
w.run(400)
check("no answer: no join", w.connects(), 0)
check("no answer: says so", w.click("Close"), true)

-- Planned restart: our server said so, the connection ends with packets flowing;
-- polls while it's down and joins once it answers (~4 min in)
w = world({ settings = ON })
w.play(10)
w.restartUntil = w.now + 120
w.run(30)
w.pingAnswer = false
w.cut()
w.run(240)
check("restart: no join while down", w.connects(), 0)
check("restart: flag spent", w.restartUntil, nil)
w.pingAnswer = true
w.run(40)
check("restart: joins when it answers", w.lastConnect(), "connect " .. OURS)

-- Restart notice that ran out before the connection ended: a plain kick
w = world({ settings = ON })
w.play(10)
w.restartUntil = w.now + 30
w.run(40)
w.cut()
w.run(60)
check("expired restart: no rejoin", w.connects(), 0)

-- Joins whose connection keeps going silent: at most 3, then it stops
w = world({ settings = ON })
w.play(10)
w.drop(60)
local seen = 0
for _ = 1, 3000 do
	w.run(0.1)
	if w.connects() > seen then
		seen = w.connects()
		w.state, w.address, w.timingOut, w.silence, w.loading = "ours", OURS, false, 0.02, true
		w.run(1)
		w.drop(30)
		w.loading = false
	end
end
w.run(200)
check("at most 3 joins", w.connects(), 3)

-- A join turned away before any connection (banned, password): stops at once
w = world({ settings = ON })
w.play(10)
w.drop(60)
w.run(5)
check("first join", w.connects(), 1)
w.loading = true
w.run(1)
w.loading = false
w.run(200)
check("turned away: no more joins", w.connects(), 1)

-- Kicked during the rejoin's loading (packets flowing): stops at once
w = world({ settings = ON })
w.play(10)
w.drop(60)
w.run(5)
w.state, w.address, w.timingOut, w.silence, w.loading = "ours", OURS, false, 0.02, true
w.run(2)
w.cut()
w.loading = false
w.run(200)
check("kicked while joining: no more joins", w.connects(), 1)

-- Crash card: offered, never joins without the click
w = world({ settings = ON, crashed = OURS })
w.run(10)
check("crash: no join on its own", w.connects(), 0)
check("crash: card", w.click("Rejoin"), true)
check("crash: joins on click", w.lastConnect(), "connect " .. OURS)
w = world({ settings = ON, crashed = "1.2.3.4:27015" })
w.run(10)
check("crash card only for our server", w.click("Rejoin"), false)

check("no Lua errors", w.errors, nil)
print(("%d checks, %d failed"):format(count, failures))
os.exit(failures == 0 and 0 or 1)
