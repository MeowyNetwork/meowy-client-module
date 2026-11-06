-- Connection resilience's menu glue (src/lua/resilience.lua) against a stubbed
-- Garry's Mod menu state, no game. It runs the same joined chunk module.cpp runs:
--   lua tests/resilience_menu_test.lua        (from the repository root; Lua 5.1+)
-- What it can't show: that the engine's IsTimingOut / GetTimeSinceLastReceived
-- read correctly in game, or that the menu state's Derma looks right.
local here = (arg and arg[0] or ""):match("^(.*)[/\\]") or "."
local function readFile(p) local f = assert(io.open(p, "rb")) local s = f:read("*a") f:close() return s end
-- The same order as module.cpp's ResilienceChunk: the UI kit, the rules, the glue.
local chunkText = "local UI = (function()\n" .. readFile(here .. "/../src/lua/ui.lua") .. "\nend)()\n"
	.. "local Logic = (function()\n" .. readFile(here .. "/../src/lua/resilience_logic.lua")
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
		cl_timeout = "30", pingAnswer = true, pingPass = false, buttons = {}, think = nil, jsCalls = 0,
	}
	-- Globals the menu state doesn't have: touching one fails the test, the way
	-- it fails in game ("attempt to call global 'RealTime' (a nil value)").
	local NOT_IN_MENU = { RealTime = true, CurTime = true, FrameTime = true, RealFrameTime = true,
		LocalPlayer = true, player = true, ents = true, net = true, timer = true, engine = true }
	local env = setmetatable({}, { __index = function(_, k)
		if NOT_IN_MENU[k] then error("menu state has no global '" .. k .. "'", 2) end
		return _G[k]
	end })
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
		ResilienceProbe = opts.native and function(a) w.pinged = a w.probeAsked = true return true end or nil,
		ResilienceProbeResult = opts.native and function()
			if not w.probeAsked then return nil end
			w.probeAsked = false
			return w.pingAnswer, false
		end or nil,
		ResilienceSteamOnline = opts.native and function() return w.steam end or nil,
		IsOurServer = function(a) return type(a) == "string" and a:match("^138%.201%.62%.52:%d+$") ~= nil end,
	}
	-- The jitter at its middle, so timings in these tests are the same every run.
	env.math = setmetatable({ random = function() return 0.5 end }, { __index = math })
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
	env.Color = function(r, g, b, a) return { r = r, g = g, b = b, a = a or 255 } end
	env.draw = { RoundedBox = function() end, RoundedBoxEx = function() end, SimpleText = function() end }
	env.surface = {
		CreateFont = function(name) w.fonts = (w.fonts or 0) + 1 end,
		SetFont = function(f) w.font = f end,
		GetTextSize = function(t) return #tostring(t) * 8, 16 end,
		SetDrawColor = function() end, DrawLine = function() end,
	}
	env.ScrW = function() return 1920 end
	env.ScrH = function() return 1080 end
	env.TEXT_ALIGN_CENTER = 1
	if opts.menu then
		env.pnlMainMenu = { Call = function(self, js) w.jsCalls = w.jsCalls + 1 w.js = js end }
	end
	env.Msg = function(m) w.printed = (w.printed or "") .. m end
	env.MsgN = function(m) w.logs = (w.logs or "") .. m .. "\n" end
	env.FILL, env.TOP = 1, 4
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
			if k == "GetText" then return function(self) return self.text end end
			if k == "SetVisible" then return function(self, v) self.visible = v end end
			if k == "Add" then return function(self, child) if type(child) == "string" then return env.vgui.Create(child) end return child end end
			if type(k) == "string" and k:match("^%u") then return function() end end
		end })
	end
	env.vgui = { Create = function(class) local p = panel(class) w.panels = w.panels or {} w.panels[#w.panels + 1] = p return p end }
	w.env = env
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
	-- Paints every live panel once, as Derma would: any Lua error is returned.
	function w.paintAll()
		local errs = {}
		for _, p in ipairs(w.panels or {}) do
			if not p.removed and rawget(p, "Paint") then
				local ok, err = pcall(p.Paint, p, 300, 60)
				if ok and p.meowyOverlay then p.busy = true ok, err = pcall(p.Paint, p, 300, 60) end
				if not ok then errs[#errs + 1] = (p.class or "?") .. ": " .. tostring(err) end
			end
		end
		return table.concat(errs, "; ")
	end
	function w.click(text)
		for i = #w.buttons, 1, -1 do
			local b = w.buttons[i]
			if b.text == text and b.DoClick and not b.removed then b.DoClick() return true end
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

local w
local function find(pred)
	for _, p in ipairs(w and w.panels or {}) do if not p.removed and pred(p) then return p end end
end

-- Opt-in: asked once, Yes sets cl_timeout 60 and remembers the old value
w = world({})
w.run(4)
check("asked at start", w.click("Yes, stay connected"), true)
check("cl_timeout set", w.cl_timeout, "60")
check("saved", w.settingsText, "asked=1\r\nenabled=1\r\nsaved_cl_timeout=30\r\n")
check("fonts made", (w.fonts or 0) > 0, true)
local w2 = world({ settings = w.settingsText })
w2.run(4)
check("not asked twice", w2.click("Yes, stay connected"), false)

-- No changes nothing
w = world({})
w.run(4)
w.click("No thanks")
check("No: cl_timeout untouched", w.cl_timeout, "30")
check("No: remembered", w.settingsText, "asked=1\r\nenabled=0\r\n")

-- Settings window: the toggle shows the setting; turning it off restores cl_timeout
w = world({ settings = "asked=1\r\nenabled=1\r\nsaved_cl_timeout=30\r\n" })
w.cl_timeout = "60"
w.run(4)
w.concommands.meowy_settings()
local toggle = find(function(p) return p.meowyToggle == "stay_connected" end)
check("toggle shows on", toggle and toggle.on, true)
toggle:SetOn(false)
check("off restores", w.cl_timeout, "30")
check("off saved", w.settingsText, "asked=1\r\nenabled=0\r\n")

-- Main menu entry: the menu page gets the JS, and once it says the entry is
-- there, no fallback pill
w = world({ settings = "asked=1\r\nenabled=0\r\n", menu = true })
w.run(2)
check("entry JS sent", w.jsCalls > 0, true)
check("entry goes after Options", (w.js or ""):find("OpenOptionsDialog", 1, true) ~= nil, true)
check("entry opens the settings", (w.js or ""):find("MeowyClientPackOpenSettings()", 1, true) ~= nil, true)
local pill = find(function(p) return p.text == "Meowy settings" end)
check("no pill while the entry may still show up", pill == nil or pill.visible == false, true)
w.env.MeowyClientPackMenuEntry()   -- what the page does once the entry is in
w.run(10)
pill = find(function(p) return p.text == "Meowy settings" end)
check("entry there: no pill", pill == nil or pill.visible == false, true)
w = world({ settings = "asked=1" .. string.char(13, 10) .. "enabled=0" .. string.char(13, 10), menu = true })
w.run(8)
pill = find(function(p) return p.text == "Meowy settings" end)
check("entry never confirmed: pill after 6 s", pill and pill.visible, true)

-- No menu page to add to: the pill shows, and opens the same window
w = world({ settings = "asked=1\r\nenabled=0\r\n" })
w.run(1)
pill = find(function(p) return p.text == "Meowy settings" end)
check("pill shown", pill and pill.visible, true)
check("pill opens settings", w.click("Meowy settings"), true)
check("settings window from the pill", find(function(p) return p.meowyToggle == "stay_connected" end) ~= nil, true)
local buttons = 0
for _, p in ipairs(w.panels) do if p.text == "Meowy settings" then buttons = buttons + 1 end end
check("one pill, not one per tick", buttons, 1)

local ON = "asked=1\r\nenabled=1\r\nsaved_cl_timeout=30\r\n"

-- The server went silent: rejoins the address the engine had, after asking the server
w = world({ settings = ON })
w.play(10)
w.drop(60)
w.run(1.5)
local overlayShown = false
for _, p in ipairs(w.panels) do if p.meowyOverlay and not p.removed then overlayShown = true end end
check("timeout: overlay up", overlayShown, true)
check("overlay paints", w.paintAll(), "")
w.run(3)
check("timeout: pinged our address", w.pinged, OURS)
check("timeout: joined", w.lastConnect(), "connect " .. OURS)
check("timeout: one join", w.connects(), 1)

check("timeout: logged the decision", (w.logs or ""):find("connection ended: state=none, fresh sample=true", 1, true) ~= nil, true)
check("timeout: logged rejoin", (w.logs or ""):find("-> rejoin (timeout)", 1, true) ~= nil, true)
check("timeout: logged the join", (w.logs or ""):find("reconnect: joining " .. OURS, 1, true) ~= nil, true)

-- The live test's case: the client times out, the network (and Steam) are down
-- for a while, then come back within the cap: the pack's own A2S keeps asking,
-- waits for Steam, then joins
w = world({ settings = ON, native = true })
w.play(10)
w.steam = false
w.pingAnswer = false
w.drop(60)
w.run(60)
check("network down: no join yet", w.connects(), 0)
check("network down: still trying", (w.logs or ""):find("reconnect: stopped", 1, true) == nil, true)
w.pingAnswer = true
w.run(35)
check("server back, Steam still offline: no join", w.connects(), 0)
check("logged Steam offline", (w.logs or ""):find("Steam offline", 1, true) ~= nil, true)
w.steam = true
w.run(35)
check("Steam back: joins", w.lastConnect(), "connect " .. OURS)
w.concommands.meowy_resilience_debug()
check("debug command prints the reading", (w.logs or ""):find("debug: state=", 1, true) ~= nil, true)

-- The live test's sequence: the connection is gone, but GMod still says
-- "in game" for a moment (its disconnect dialog, the teardown). That must not
-- end the rejoin it just started (it used to: "stopped (no longer needed)").
w = world({ settings = ON, native = true })
w.play(10)
w.steam = false
w.drop(60)
w.inGame = true            -- the teardown frames
w.run(0.6)
w.inGame = false
w.run(1)
check("teardown: rejoin still running", (w.logs or ""):find("reconnect: stopped", 1, true) == nil, true)
w.steam = true
w.run(5)
check("teardown: joins", w.lastConnect(), "connect " .. OURS)
w.state, w.address, w.timingOut, w.silence, w.inGame, w.loading = "ours", OURS, false, 0.02, true, false
w.run(1)
check("teardown: back in the game ends it", (w.logs or ""):find("reconnect: stopped (back in the game)", 1, true) ~= nil, true)

-- The player joins another server while it waits: the rejoin steps aside
w = world({ settings = ON })
w.play(10)
w.pingAnswer = false
w.drop(60)
w.run(3)
w.state, w.address, w.inGame = "other", nil, true
w.run(1)
check("joined elsewhere: stopped", (w.logs or ""):find("reconnect: stopped (joined another server)", 1, true) ~= nil, true)
w.pingAnswer = true
w.state, w.inGame = "none", false
w.run(60)
check("joined elsewhere: never joins ours", w.connects(), 0)

-- The player rejoins by hand while it waits: ends, no second join
w = world({ settings = ON })
w.play(10)
w.pingAnswer = false
w.drop(60)
w.run(3)
w.state, w.address, w.timingOut, w.silence = "ours", OURS, false, 0.02
w.run(1)
check("rejoined by hand: stopped", (w.logs or ""):find("reconnect: stopped (back on our server)", 1, true) ~= nil, true)

-- The second live test: after the drop GMod's disconnect dialog keeps
-- IsInLoading()/IsInGame() true; when the server answers, it still connects
-- (it used to wait "for a join already under way" forever)
w = world({ settings = ON, native = true })
w.play(10)
w.steam = true
w.pingAnswer = false
w.drop(60)
w.loading, w.inGame = true, true
w.run(20)
check("dialog up, server down: no join yet", w.connects(), 0)
w.pingAnswer = true
w.run(35)
check("dialog up, server answers: connects", w.lastConnect(), "connect " .. OURS)
check("no 'already under way' loop", (w.logs or ""):find("already under way", 1, true) == nil, true)
check("no menu-state errors", w.errors, nil)

-- The third live test: the server is back but Steam takes ~3 min to log on
-- again. The pack waits for Steam (checked every second, not counted as
-- tries) up to 5 min from the drop, then joins as soon as it's back.
w = world({ settings = ON, native = true })
w.play(10)
w.steam = false
w.pingAnswer = false
w.drop(60)
w.run(20)
w.pingAnswer = true
w.run(25)
local function overlayTitle()
	local o = find(function(p) return p.meowyOverlay end)
	return o and o.title and o.title.text
end
check("Steam offline: waiting card", overlayTitle(), "Waiting for Steam to reconnect...")
check("Steam offline: logged", (w.logs or ""):find("waiting for Steam to reconnect", 1, true) ~= nil, true)
w.run(200)
check("Steam offline 3+ min: still waiting, no join", w.connects() == 0 and (w.logs or ""):find("reconnect: stopped", 1, true) == nil, true)
w.steam = true
w.run(3)
check("Steam back: joins at once", w.lastConnect(), "connect " .. OURS)
check("Steam back: server checked once more", (w.logs or ""):find("Steam is back; checking the server once more", 1, true) ~= nil, true)

-- Steam never comes back: stops at 5 min, offers "Rejoin now"
w = world({ settings = ON, native = true })
w.play(10)
w.steam = false
w.drop(60)
w.run(320)
check("Steam never back: stopped", (w.logs or ""):find("Steam didn't reconnect", 1, true) ~= nil, true)
check("Steam never back: no join", w.connects(), 0)
w.steam = true
check("Rejoin now offered and joins", w.click("Rejoin now"), true)
check("Rejoin now: one join to our address", w.lastConnect() == "connect " .. OURS and w.connects() == 1, true)

-- Gave up (server never answered): the card stays with Rejoin now and Close
w = world({ settings = ON })
w.play(10)
w.pingAnswer = false
w.drop(60)
w.run(400)
check("gave up: card still up long after", overlayTitle(), "Couldn't reconnect to Meowy Network")
check("gave up: Close works", w.click("Close"), true)
check("gave up: card gone after Close", overlayTitle(), nil)

-- The offer goes away by itself once the player is on a server again
w = world({ settings = ON })
w.play(10)
w.pingAnswer = false
w.drop(60)
w.run(400)
w.state, w.address = "ours", OURS
w.run(1)
check("offer hidden once back on a server", overlayTitle(), nil)

-- A password (final): no Rejoin now
w = world({ settings = ON })
w.play(10)
w.pingPass = true
w.drop(60)
w.run(10)
check("password: stopped", (w.logs or ""):find("the server has a password", 1, true) ~= nil, true)
check("password: no Rejoin now", w.click("Rejoin now"), false)

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
check("packets flowing: logged why", (w.logs or ""):find("-> no rejoin: packets were still arriving", 1, true) ~= nil, true)

-- Engine sample unreadable: nothing
w = world({ settings = ON })
w.play(10)
w.timingOut, w.silence = nil, nil
w.run(1)
w.cut()
w.run(60)
check("no sample: no rejoin", w.connects(), 0)
check("no sample: logged why", (w.logs or ""):find("-> no rejoin: no engine sample", 1, true) ~= nil, true)

-- Not opted in: nothing
w = world({ settings = "asked=1\r\nenabled=0\r\n" })
w.play(10)
w.drop(60)
w.run(60)
check("opted out: no rejoin", w.connects(), 0)
check("opted out: logged why", (w.logs or ""):find("staying connected is off", 1, true) ~= nil, true)

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
check("crash card paints", w.paintAll(), "")
check("crash: card", w.click("Rejoin"), true)
check("crash: joins on click", w.lastConnect(), "connect " .. OURS)
w = world({ settings = ON, crashed = "1.2.3.4:27015" })
w.run(10)
check("crash card only for our server", w.click("Rejoin"), false)

check("no Lua errors", w.errors, nil)
print(("%d checks, %d failed"):format(count, failures))
os.exit(failures == 0 and 0 or 1)
