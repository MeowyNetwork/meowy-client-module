-- Connection resilience's menu glue (src/lua/resilience.lua) against a stubbed
-- Garry's Mod menu state, no game. It runs the same joined chunk module.cpp runs:
--   lua tests/resilience_menu_test.lua        (from the repository root; Lua 5.1+)
-- What it can't show: that the engine's IsTimingOut / GetTimeSinceLastReceived
-- read correctly in game, or that the menu state's Derma looks right.
local here = (arg and arg[0] or ""):match("^(.*)[/\\]") or "."
local function readFile(p) local f = assert(io.open(p, "rb")) local s = f:read("*a") f:close() return s end
-- The same order as module.cpp's ResilienceChunk: the UI kit, the rules, the PC doctor, the glue.
local chunkText = "local UI = (function()\n" .. readFile(here .. "/../src/lua/ui.lua") .. "\nend)()\n"
	.. "local Logic = (function()\n" .. readFile(here .. "/../src/lua/resilience_logic.lua")
	.. "\nend)()\n" .. "local Doctor = (function()\n" .. readFile(here .. "/../src/lua/doctor_logic.lua")
	.. "\nend)()\n" .. readFile(here .. "/../src/lua/doctor.lua") .. "\n"
	.. readFile(here .. "/../src/lua/resilience.lua")
	-- The Home card, as module.cpp appends it.
	.. "\nlocal Home = (function(...)\n" .. readFile(here .. "/../src/lua/home_logic.lua") .. "\nend)(Logic);\n"
	.. "(function()\n" .. readFile(here .. "/../src/lua/home.lua") .. "\nend)();\n"
	.. "(function()\n" .. readFile(here .. "/../src/lua/menu_page.lua") .. "\nend)()\n"
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

-- Garry's Mod's Lua adds `continue` as a keyword: as a bare name (a table key,
-- a field, a variable) the whole chunk fails to compile in game, though plain
-- Lua takes it.
do
	local code = chunkText:gsub("%[(=*)%[.-%]%1%]", ""):gsub("%-%-[^\n]*", ""):gsub('"[^"\n]*"', ""):gsub("'[^'\n]*'", "")
	local bad = code:match("[%w_%.]*continue%s*=[^=]") or code:match("%.continue[^%w_]") or code:match("local%s+continue[^%w_]")
	check("no `continue` used as a name (a keyword in Garry's Mod's Lua)", bad, nil)
end

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
		LocalPlayer = true, player = true, ents = true, net = true, timer = true,
		TEXT_ALIGN_LEFT = true, TEXT_ALIGN_CENTER = true, TEXT_ALIGN_RIGHT = true, TEXT_ALIGN_TOP = true, TEXT_ALIGN_BOTTOM = true }
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
		ResilienceSession = function(on, name) w.session = on w.sessionName = name w.marks = (w.marks or 0) + 1 end,
		ResilienceCrashedSession = function() return w.crashed, w.crashedName end,
		-- Continue: native keeps the engine's address (w.otherAddress on another server).
		LastServerMark = function(name)
			w.lastMarks = (w.lastMarks or 0) + 1
			local address = w.state == "ours" and OURS or w.otherAddress
			if not address then return false end
			w.last = { address, name, w.osTime }
			return true
		end,
		LastServer = function() if w.last then return w.last[1], w.last[2], w.last[3] end end,
		-- The Home card: our address, and the game's own query over w.lists[type].
		OurServerAddress = function() return OURS end,
		MenuShellJs = function() return opts.noPage and "" or "/*shell*/" end,
		-- The engine's connection now: our address while on ours, w.otherNow elsewhere.
		ServerNow = function()
			if w.state == "ours" then return OURS, w.latency, w.loss, w.connected end
			if w.state == "other" then return w.otherNow, w.latency, w.loss, w.connected end
			return nil
		end,
		StockServerQuery = function(data)
			w.queried = w.queried or {}
			w.queried[data.Type] = (w.queried[data.Type] or 0) + 1
			for _, s in ipairs((w.lists or {})[data.Type] or {}) do
				local more
				if s.failed then more = data.CallbackFailed(s.address)
				else more = data.Callback(s.ping or 30, s.name, "", s.map or "gm_x", s.players or 0, s.max or 32, s.bots or 0, s.pass or false, s.last or 0, s.address) end
				if more == false then break end
			end
			data.Finished()
		end,
		LastServerForget = function() w.last = nil w.forgot = (w.forgot or 0) + 1 end,
		-- Join pre-warm: what was recorded (w.recorded[address]) and what was asked.
		PrewarmBeginJoin = function() w.pwBegins = (w.pwBegins or 0) + 1 end,
		PrewarmEndJoin = function() w.pwEnds = (w.pwEnds or 0) + 1 return 12 end,
		PrewarmRecorded = function(a) return (w.recorded or {})[a] == true, 100 end,
		PrewarmStart = function(a) w.pwStarts = w.pwStarts or {} w.pwStarts[#w.pwStarts + 1] = a return true end,
		PrewarmCancel = function() w.pwCancels = (w.pwCancels or 0) + 1 end,
		PrewarmState = function() return "idle" end,
		ServerQueryStart = function() return 1 end,
		PrewarmWorkshop = function(a) local ids = {} for i, id in ipairs((w.workshop or {})[a] or {}) do ids[i] = id end return ids end,
		-- Steam's view of an item: installed, needsUpdate, busy, legacy, updated
		-- (w.wsState[id] = false: Steam can't say; default: installed, the version of 1600000000).
		WorkshopItemState = function(id)
			local s = (w.wsState or {})[id]
			if s == false then return nil end
			s = s or { true, false, false, false, 1600000000 }
			return s[1], s[2], s[3], s[4], s[5]
		end,
		-- Menu settings (module.cpp's meowy.Setting): a convar-like value.
		Setting = function(name, default)
			w.convars = w.convars or {}
			-- Unless a test says otherwise, a player who chose Garry's Mod's main
			-- screen after the home screen left preview (opts.fresh: nothing saved).
			local saved = not opts.fresh and ({ meowy_menu = "0", meowy_menu_version = "2" })[name] or nil
			if w.convars[name] == nil then w.convars[name] = (opts.convars and opts.convars[name]) or saved or default end
			-- As in game: SetBool on these is refused ("not created by Lua"); the
			-- console sets them (RunConsoleCommand below).
			return {
				GetBool = function() return w.convars[name] == "1" end,
				GetString = function() return w.convars[name] end,
				SetBool = function() error("calling 'SetBool' on bad self (attempted to modify ConVar not created by Lua)", 2) end,
			}
		end,
		DoctorFacts = function() return opts.facts or { arch = "x64", gpu_time_known = false, adapters = {} } end,
		DoctorOpen = function(id) w.opened = id return true end,
		ResilienceProbe = opts.native and function(a) w.pinged = a w.probeAsked = true return true end or nil,
		ResilienceProbeResult = opts.native and function()
			if not w.probeAsked then return nil end
			w.probeAsked = false
			return w.pingAnswer, false
		end or nil,
		ResilienceSteamOnline = opts.native and function() return w.steam end or nil,
		IsOurServer = function(a) return type(a) == "string" and a == "138.201.62.52:27015" end,
		-- The start-up line (opts.startup: the stubs exist; w.startup: it's in menu.lua).
		StartupHook = opts.startup ~= nil and function() return w.startup, not w.startup end or nil,
		SetStartupHook = opts.startup ~= nil and function(on) w.startup = on return true end or nil,
		-- The engine's last "Disconnected: <reason>" (w.refuse sets it).
		ResilienceLastDisconnect = function()
			if not w.refusedAt then return nil end
			return w.refusal, w.now - w.refusedAt
		end,
	}
	-- The jitter at its middle, so timings in these tests are the same every run.
	env.math = setmetatable({ random = function() return 0.5 end,
		Clamp = function(v, lo, hi) return v < lo and lo or (v > hi and hi or v) end }, { __index = math })
	env.SysTime = function() return w.now end
	w.osTime = 1000000
	env.os = setmetatable({ time = function() return w.osTime end }, { __index = os })
	env.IsInGame = function() return w.inGame end
	env.IsInLoading = function() return w.loading end
	-- Convars other parts of the pack own (module.cpp's chunks), only when a test asks.
	env.GetConVar = opts.engineConVars and function(n)
		if w.cvars[n] ~= nil then return { GetBool = function() return w.cvars[n] == "1" end } end
	end or nil
	-- Engine convars the Escape card reads and sets.
	w.cvars = { volume = "0.7", voice_scale = "1", fov_desired = "90" }
	env.GetConVarString = function(n) if n == "cl_timeout" then return w.cl_timeout end return w.cvars[n] or "" end
	env.RunConsoleCommand = function(cmd, ...)
		w.commands[#w.commands + 1] = table.concat({ cmd, ... }, " ")
		if cmd == "cl_timeout" then w.cl_timeout = (...) end
		if w.cvars[cmd] ~= nil then w.cvars[cmd] = (...) end
		if w.convars and w.convars[cmd] ~= nil then w.convars[cmd] = (...) end
	end
	env.SetClipboardText = function(t) w.clipboard = t end
	env.RunGameUICommand = function(c) w.gameUI = c end
	-- The Escape menu is open (in game, the menu shows only then).
	env.gui = { IsGameUIVisible = function() return w.escOpen == true end }
	-- Every Think hook runs each frame, in the order added (resilience, then the Home card).
	local thinks, order = {}, {}
	w.hooks = {}
	env.hook = { Add = function(event, id, fn)
		if event ~= "Think" then
			w.hooks[event] = w.hooks[event] or {}
			w.hooks[event][id] = fn
			return
		end
		if not thinks[id] then order[#order + 1] = id end
		thinks[id] = fn
	end }
	function w.fire(event, ...) for _, fn in pairs(w.hooks[event] or {}) do fn(...) end end
	-- Steam's Workshop, as the menu has it: each DownloadUGC waits in w.ugc until
	-- the test answers it (w.answer).
	w.ugc, w.infos = {}, {}
	env.steamworks = {
		DownloadUGC = function(id, cb) w.ugc[#w.ugc + 1] = { id = id, cb = cb } end,
		FileInfo = function(id, cb) w.infos[#w.infos + 1] = { id = id, cb = cb } end,
	}
	function w.answer(i, path)
		local q = table.remove(w.ugc, i or 1)
		if q then q.cb(path ~= false and (path or ("cache/workshop/" .. q.id .. ".gma")) or nil, nil) end
		return q and q.id
	end
	-- The Workshop's current version of each asked item (w.published[id]); by
	-- default the installed one, rounded the way FileInfo's float rounds it.
	function w.answerInfos()
		while #w.infos > 0 do
			local q = table.remove(w.infos, 1)
			q.cb({ updated = (w.published or {})[q.id] or 1600000064 })
		end
	end
	-- The game's files: everything there but w.missing[path].
	env.file = { Exists = function(p, where) w.existsAsked = where return not (w.missing or {})[p] end }
	env.istable = function(v) return type(v) == "table" end
	-- The menu state's engine library: your subscribed addons (w.addons).
	env.engine = { GetAddons = function() return w.addons or {} end }
	w.think = function() for _, id in ipairs(order) do thinks[id]() end end
	env.hook.Run = function(event) w.hookRuns = w.hookRuns or {} w.hookRuns[#w.hookRuns + 1] = event end
	-- What Garry's Mod's menu Lua has (mainmenu.lua), for refilling its page.
	env.VERSIONSTR, env.NETVERSIONSTR, env.BRANCH = "2026.09.16", "2026.09.16", "x86-64"
	env.LanguageChanged = function(lang) w.languageReplays = (w.languageReplays or 0) + 1 end
	-- Garry's Mod adds JavascriptSafe to the string library (so s:JavascriptSafe() works).
	string.JavascriptSafe = string.JavascriptSafe or function(s) return (s:gsub("[\\'\"]", "\\%0")) end
	env.isfunction = function(v) return type(v) == "function" end
	env.serverlist = { PingServer = function(addr, cb)
		w.pinged = addr
		if w.pingAnswer then cb(30, "Meowy", "", "map", 10, 64, 0, w.pingPass) end
	end }
	if opts.joinServer then
		env.JoinServer = function(addr) w.joinServer = (w.joinServer or 0) + 1 w.joinedVia = addr end
	end
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
	if opts.menu then
		-- The menu page: w.js is the last menu-entry call, w.homeJS the last Home card call.
		env.pnlMainMenu = { Call = function(self, js)
			w.jsCalls = w.jsCalls + 1
			if js:find("window.MeowyHome(", 1, true) then w.homeJS = js w.homeCalls = (w.homeCalls or 0) + 1
			elseif js:find("window.MeowyEsc(", 1, true) then w.escJS = js
			elseif js:find("window.MeowyAddons", 1, true) then w.addonsJS = js w.addonsSends = (w.addonsSends or 0) + 1
			elseif js:find("window.MeowyShell(", 1, true) then w.shellJS = js w.shellSends = (w.shellSends or 0) + 1
			elseif js:find("style.zoom", 1, true) then w.zoomCalls = w.zoomCalls or {} w.zoomCalls[#w.zoomCalls + 1] = js
			else w.js = js end
		end }
		-- The menu's browser (DHTML): its callbacks, the bridge, what it loads.
		local stockRun = function() w.stockRuns = (w.stockRuns or 0) + 1 end
		env.pnlMainMenu.HTML = {
			Callbacks = { ["lua.Run"] = stockRun },
			AddFunction = function(self, obj, name, fn) self.Callbacks[obj .. "." .. name] = fn w.bridges = (w.bridges or 0) + 1 end,
			OpenURL = function(self, url) w.loads = w.loads or {} w.loads[#w.loads + 1] = url w.url = url w.page = url:find("meowy/home", 1, true) and "ours" or "stock" end,
			SetHTML = function(self, html) w.loads = w.loads or {} w.loads[#w.loads + 1] = "html" w.page = "ours" w.html = html end,
		}
		w.stockRun = stockRun
		-- What the page would post through meowy.post.
		function w.post(kind, value) return env.pnlMainMenu.HTML.Callbacks["meowy.post"](kind, value) end
	end
	env.Msg = function(m) w.printed = (w.printed or "") .. m end
	env.MsgN = function(m) w.logs = (w.logs or "") .. m .. "\n" end
	env.FILL, env.TOP = 1, 4
	env.isnumber = function(v) return type(v) == "number" end
	env.isstring = function(v) return type(v) == "string" end
	env.ErrorNoHalt = function(m) w.errors = (w.errors or "") .. m end
	env.IsValid = function(p) return type(p) == "table" and not p.removed end
	local function panel(class)
		local p = { class = class }
		return setmetatable(p, { __index = function(t, k)
			if k == "SetText" then return function(self, s) self.text = s if self.class == "DButton" then w.buttons[#w.buttons + 1] = self end end end
			if k == "SetTitle" then return function(self, s) self.title = s end end
			if k == "Remove" or k == "Close" then return function(self) self.removed = true end end
			if k == "SetValue" then return function(self, v) self.value = v end end
			if k == "GetText" then return function(self) return self.text end end
			-- Position and size, as real panels keep them (the layout reads them back).
			if k == "SetPos" then return function(self, x, y) self.px, self.py = x, y end end
			if k == "GetPos" then return function(self) return self.px or 0, self.py or 0 end end
			if k == "SetSize" then return function(self, x, y) self.sw, self.sh = x, y end end
			if k == "SetWide" then return function(self, x) self.sw = x end end
			if k == "SetTall" then return function(self, y) self.sh = y end end
			if k == "GetWide" then return function(self) return self.sw or 0 end end
			if k == "GetTall" then return function(self) return self.sh or 0 end end
			if k == "GetSize" then return function(self) return self.sw or 0, self.sh or 0 end end
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
	function w.firstConnect()
		for _, c in ipairs(w.commands) do if c:match("^connect ") then return c end end
	end
	function w.lastConnect()
		for i = #w.commands, 1, -1 do if w.commands[i]:match("^connect ") then return w.commands[i] end end
	end
	function w.ran(command)
		local n = 0
		for _, c in ipairs(w.commands) do if c == command then n = n + 1 end end
		return n
	end
	-- On our server, in game, packets flowing.
	function w.play(seconds)
		w.state, w.address, w.timingOut, w.silence, w.inGame, w.loading = "ours", OURS, false, 0.02, true, false
		w.run(seconds or 5)
	end
	-- The server goes silent; the engine gives up after `after` seconds.
	-- The engine refuses the join under way: "Disconnected: <reason>".
	function w.refuse(reason) w.refusal, w.refusedAt = reason, w.now end
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
check("asked at start", w.click("Stay connected"), true)
check("cl_timeout set", w.cl_timeout, "60")
check("saved", w.settingsText, "asked=1\r\nenabled=1\r\nsaved_cl_timeout=30\r\n")
check("config.cfg never rewritten by the pack", w.ran("host_writeconfig"), 0)
check("fonts made", (w.fonts or 0) > 0, true)
local w2 = world({ settings = w.settingsText })
w2.cl_timeout = "60"
w2.run(4)
check("not asked twice", w2.click("Stay connected"), false)
check("still 60 at start: nothing run", #w2.commands, 0)

-- No changes nothing
w = world({})
w.run(4)
w.click("Not now")
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
toggle:SetOn(true)
check("on again: 60", w.cl_timeout, "60")
check("on and off: config.cfg never rewritten", w.ran("host_writeconfig"), 0)

-- On, but the game crashed before the engine saved cl_timeout 60 (or a config
-- file set it back): at start it's the old value again, so 60 is set again.
w = world({ settings = "asked=1\r\nenabled=1\r\nsaved_cl_timeout=45\r\n" })
w.cl_timeout = "45"
w.run(2)
check("lost 60: not before config.cfg has run", w.cl_timeout, "45")
w.run(2)
check("lost 60 (saved value): set again", w.cl_timeout, "60")
check("lost 60: config.cfg not rewritten (set again at each start instead)", w.ran("host_writeconfig"), 0)
check("lost 60: saved value kept for turning it off", w.settingsText, "asked=1\r\nenabled=1\r\nsaved_cl_timeout=45\r\n")
check("lost 60: logged", (w.logs or ""):find("cl_timeout is 45 again", 1, true) ~= nil, true)
w.run(10)
check("lost 60: once", w.ran("cl_timeout 60"), 1)
w = world({ settings = "asked=1\r\nenabled=1\r\n" })
w.run(4)
check("lost 60 (the default, nothing saved): set again", w.cl_timeout, "60")
w = world({ settings = "asked=1\r\nenabled=1\r\nsaved_cl_timeout=30\r\n" })
w.cl_timeout = "120"
w.run(4)
check("player's own value since: left alone", w.cl_timeout, "120")
w = world({ settings = "asked=1\r\nenabled=0\r\n" })
w.run(4)
check("off: cl_timeout left alone", w.cl_timeout, "30")

-- Start with the game: shows whether the line is in menu.lua, and flips it
w = world({ settings = "asked=1\r\nenabled=1\r\n", startup = true })
w.startup = true
w.run(4)
w.concommands.meowy_settings()
local start = find(function(p) return p.meowyToggle == "start_with_game" end)
check("start-up toggle shows on", start and start.on, true)
start:SetOn(false)
check("start-up off: line removed", w.startup, false)
check("settings with start-up paint", w.paintAll(), "")

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

-- PC doctor: a tab with a badge in the settings window, and meowy_doctor
w = world({ settings = "asked=1\r\nenabled=0\r\n", facts = {
	arch = "x64", gpu_time_known = true, on_battery = false, battery_saver = false, power_mode = "balanced",
	adapters = {
		{ name = "NVIDIA GeForce RTX 3070 Ti Laptop GPU", vram_mb = 8017, perf_rank = 1, power_rank = 2, share = 0 },
		{ name = "AMD Radeon(TM) Graphics", vram_mb = 485, perf_rank = 2, power_rank = 1, share = 1 },
	} } })
w.run(1)
w.concommands.meowy_settings()
local docTab = find(function(p) return p.tabId == "doctor" end)
check("doctor tab", docTab and docTab.text, "PC doctor")
check("doctor tab badge", docTab and docTab.badge, "1 tip")
check("general tab", find(function(p) return p.tabId == "general" end) ~= nil, true)
check("open the doctor tab", w.click("PC doctor"), true)
check("a card for the GPU tip", find(function(p) return p.meowyTip == "gpu" end) ~= nil, true)
check("doctor button opens Graphics settings", w.click("Open Graphics settings"), true)
check("opened by id", w.opened, "graphics_settings")
w.concommands.meowy_doctor()
check("meowy_doctor prints", (w.printed or ""):match("^[^\n]+"), "[Meowy Client Pack] PC doctor: 1 tip")
check("settings and doctor cards paint", w.paintAll(), "")

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

-- With the engine's JoinServer (the menu state has it): joins through it, the
-- way the server browser does, not RunConsoleCommand("connect").
w = world({ settings = ON, joinServer = true })
w.play(10)
w.drop(60)
w.run(5)
check("JoinServer: used", w.joinServer, 1)
check("JoinServer: our address", w.joinedVia, OURS)
check("JoinServer: no connect command", w.connects(), 0)

-- A join refused at once for a reason that clears up (Steam re-validating the
-- old session, 6 Nov Wi-Fi run 3): tries again within seconds, not after 20.
w = world({ settings = ON })
w.play(10)
w.drop(60)
w.run(5)
check("refused: first join", w.connects(), 1)
w.refuse("STEAM validation rejected")
w.run(5)
check("refused (Steam): joined again within 5 s", w.connects(), 2)
check("refused (Steam): logged", (w.logs or ""):find("join refused after", 1, true) ~= nil, true)

-- Refused for any other reason (a ban, the whitelist): stops, no Rejoin now.
w = world({ settings = ON })
w.play(10)
w.drop(60)
w.run(5)
w.refuse("You are banned from this server")
w.run(30)
check("refused (ban): one join only", w.connects(), 1)
check("refused (ban): no Rejoin now", w.click("Rejoin now"), false)

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
check("Steam offline: waiting card", overlayTitle(), "Waiting for Steam to reconnect")
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

-- During a restart notice the player clicks Disconnect (packets flowing, the
-- server stays up): never rejoined, nothing shown, nothing offered
w = world({ settings = ON })
w.play(10)
w.restartUntil = w.now + 120
w.run(5)
w.cut()
w.run(3)
check("own disconnect in a restart notice: no card while it checks", overlayTitle(), nil)
check("own disconnect in a restart notice: no join yet", w.connects(), 0)
w.run(60)
check("own disconnect in a restart notice: never joins", w.connects(), 0)
check("own disconnect in a restart notice: no card", overlayTitle(), nil)
check("own disconnect in a restart notice: no Rejoin now", w.click("Rejoin now"), false)
check("own disconnect in a restart notice: logged", (w.logs or ""):find("reconnect: stopped (the server never went down", 1, true) ~= nil, true)
check("own disconnect in a restart notice: flag spent", w.restartUntil, nil)
w.pingAnswer = false   -- the server restarts later, after the episode ended
w.run(60)
w.pingAnswer = true
w.run(120)
check("own disconnect, server restarts later: still no join", w.connects(), 0)

-- Kicked during the notice ("Disconnected: Kicked by ..."; packets flowing, the
-- server stays up): never rejoined
w = world({ settings = ON, native = true })
w.steam = true
w.play(10)
w.restartUntil = w.now + 300
w.run(5)
w.refuse("Kicked by Console : AFK")
w.cut()
w.run(120)
check("kicked in a restart notice: never joins", w.connects(), 0)
check("kicked in a restart notice: no Rejoin now", w.click("Rejoin now"), false)
check("kicked in a restart notice: no card", overlayTitle(), nil)

-- The real restart: kicked, the old instance answers a few more seconds while
-- it saves, then it's down, then back: joins once it answers again
w = world({ settings = ON, native = true })
w.steam = true
w.play(10)
w.restartUntil = w.now + 120
w.run(5)
w.cut()
w.run(6)
check("restart, old instance still up: no join", w.connects(), 0)
check("restart, old instance still up: no card yet", overlayTitle(), nil)
w.pingAnswer = false
w.run(60)
check("restart, down: no join", w.connects(), 0)
check("restart, down: reconnect card", overlayTitle(), "Reconnecting to Meowy Network")
w.pingAnswer = true
w.run(40)
check("restart, back: joins", w.lastConnect(), "connect " .. OURS)
check("restart, back: one join", w.connects(), 1)

-- Joining another server during the notice: the old connection closes first
-- (no connection for a moment, past the first probe), then the other server's
-- comes up. Ours stays up: never joined, even during the slow handshake.
w = world({ settings = ON })
w.play(10)
w.restartUntil = w.now + 120
w.run(5)
w.cut()
w.loading = true
w.run(4)
check("joining elsewhere in a restart notice: no join during the handshake", w.connects(), 0)
check("joining elsewhere in a restart notice: no card", overlayTitle(), nil)
w.state, w.address, w.inGame, w.loading = "other", nil, true, false
w.run(2)
check("joining elsewhere in a restart notice: stopped", (w.logs or ""):find("reconnect: stopped (joined another server)", 1, true) ~= nil, true)
w.run(120)
check("joining elsewhere in a restart notice: never joins ours", w.connects(), 0)
-- ... and when that join fails and the player is back at the menu: still nothing
w = world({ settings = ON })
w.play(10)
w.restartUntil = w.now + 120
w.run(5)
w.cut()
w.loading = true
w.run(4)
w.loading = false
w.run(120)
check("join elsewhere failed, ours up: never joins ours", w.connects(), 0)
check("join elsewhere failed, ours up: no Rejoin now", w.click("Rejoin now"), false)

-- A real drop (the connection went silent) during a restart notice: as before,
-- joins as soon as the server answers, without waiting to see it go down
w = world({ settings = ON })
w.play(10)
w.restartUntil = w.now + 300
w.drop(60)
w.run(4)
check("silent end in a restart notice: joins", w.lastConnect(), "connect " .. OURS)

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
-- Another server: a card with its name, one click connects, nothing on its own.
w = world({ settings = ON, crashed = "1.2.3.4:27015" })
w.crashedName = "Foo RP | 24/7"
w.run(10)
check("other server's crash: no join on its own", w.connects(), 0)
check("other server's crash card paints", w.paintAll(), "")
check("other server's crash: card", w.click("Rejoin"), true)
check("other server's crash: joins that server on click", w.lastConnect(), "connect 1.2.3.4:27015")
-- No name stored: the address stands in for it, still a card.
w = world({ settings = ON, crashed = "5.6.7.8:27015" })
w.run(10)
check("unnamed server's crash: card", w.click("Rejoin"), true)

-- On another server: marked (with the name once it's known), cleared on leaving.
w = world({ settings = ON })
w.state = "other"
w.run(2)
check("other server: marked", w.session, true)
w.env.meowy.JoiningName = "Bar TTT"
w.run(2)
check("other server: marked again with its name", w.sessionName, "Bar TTT")
local marks = w.marks
w.run(5)
check("other server: not marked every frame", w.marks, marks)
w.state = "none"
w.run(2)
check("left the other server: cleared", w.session, false)

-- Continue: the main menu's entry for the last server the player was in the game on.
local function continueJS(w) return (w.js or ""):match("window%.MeowyContinue = (.-);\n") end
w = world({ settings = ON, menu = true })
w.run(4)
check("continue: nothing kept, none shown", continueJS(w), "null")
check("continue: the page code is still sent", (w.js or ""):find("OpenOptionsDialog", 1, true) ~= nil, true)
w.env.meowy.JoiningName = "Meowy Network | DarkRP"
w.state, w.address, w.loading = "ours", OURS, true
w.run(3)
check("continue: not kept while loading", w.lastMarks, nil)
w.play(5)
check("continue: kept once in the game", w.lastMarks, 1)
check("continue: with the server's name", w.last and w.last[2], "Meowy Network | DarkRP")
w.run(5)
check("continue: kept once, not every frame", w.lastMarks, 1)
check("continue: not shown in game", continueJS(w), "null")
w.osTime = w.osTime + 7200
w.cut()
w.run(2)
check("continue: shown in the menu after leaving", continueJS(w), '["Continue","Meowy Network | DarkRP"," \194\183 2 h ago"]')
check("continue: the entry goes before Start New Game", (w.js or ""):find("#/newgame/", 1, true) ~= nil, true)
check("continue: nothing joined on its own", w.connects(), 0)
w.env.MeowyClientPackContinue()
check("continue: click joins our server", w.lastConnect(), "connect " .. OURS)
check("continue: logged", (w.logs or ""):find("continue: joining " .. OURS, 1, true) ~= nil, true)
w.loading = true
local before = w.connects()
w.env.MeowyClientPackContinue()
check("continue: not while a join loads", w.connects(), before)
w.loading = false
-- Through the engine's JoinServer when the menu has it, like the rejoin.
w = world({ settings = ON, menu = true, joinServer = true })
w.last = { OURS, "Meowy", w.osTime }
w.run(2)
w.env.MeowyClientPackContinue()
check("continue: joins through JoinServer", w.joinedVia, OURS)
-- Any other public server too, by the engine's address; the name only as text.
w = world({ settings = ON, menu = true })
w.otherAddress = "1.2.3.4:27015"
w.env.meowy.JoiningName = [[Evil "RP" </script><script>lua.Run('x')]]
w.state, w.inGame = "other", true
w.run(2)
check("continue: another server kept", w.last and w.last[1], "1.2.3.4:27015")
w.state, w.inGame = "none", false
w.run(2)
local data = continueJS(w) or ""
check("continue: the name can't break out of the script", data:find("</script>", 1, true) == nil and data:find("'", 1, true) == nil, true)
check("continue: the name escaped", data:find([[Evil \x22RP\x22 \x3c/script\x3e]], 1, true) ~= nil, true)
w.env.MeowyClientPackContinue()
check("continue: click joins that server", w.lastConnect(), "connect 1.2.3.4:27015")
-- The console command does the same.
w.commands = {}
w.concommands.meowy_continue()
check("continue: meowy_continue joins it", w.lastConnect(), "connect 1.2.3.4:27015")
-- Not shown while a rejoin is under way (its own card has the buttons).
w = world({ settings = ON, menu = true })
w.play(10)
check("continue: kept for our server", w.last ~= nil, true)
w.drop(60)
w.run(1)
check("continue: hidden during a rejoin", continueJS(w), "null")
-- Off: nothing kept, nothing shown, and the kept one forgotten.
w = world({ settings = ON, menu = true, convars = { meowy_menu_continue = "0" } })
w.play(5)
w.cut()
w.run(2)
check("continue off: nothing kept", w.lastMarks, nil)
check("continue off: none shown", continueJS(w), "null")
w.last = { OURS, "Meowy", w.osTime }
w.env.MeowyClientPackContinue()
check("continue off: the command does nothing", w.connects(), 0)
w = world({ settings = ON, menu = true })
w.play(5)
w.cut()
w.run(2)
w.concommands.meowy_settings()
local ct = find(function(p) return p.meowyToggle == "menu_continue" end)
check("continue: a toggle in settings, on", ct and ct.on, true)
if ct then ct:SetOn(false) end
check("continue: toggled off", w.convars.meowy_menu_continue, "0")
check("continue: toggled off forgets the server", w.last == nil and w.forgot == 1, true)
w.run(2)
check("continue: toggled off, none shown", continueJS(w), "null")
check("continue: settings window paints", w.paintAll(), "")

-- The Home card: our server, favourites and recent, live, on the main page.
local function homeData(w) return (w.homeJS or ""):match("window%.MeowyHome%((.*),%d+%);\nif %(window%.MeowyShellLists%)") end
local LISTS = {
	favorite = {
		{ address = "1.1.1.1:27015", name = "Small", players = 2, max = 16 },
		{ address = "2.2.2.2:27015", name = "Big RP", players = 40, max = 64, bots = 4 },
		{ address = "3.3.3.3:27015", failed = true },
	},
	history = {
		{ address = "5.5.5.5:27015", name = "Fresh", players = 5, max = 10, last = 1000000 - 7200 },
	},
	friends = {
		{ address = "8.8.4.4:27015", name = "Pals TTT", players = 9, max = 24 },
	},
}
w = world({ settings = ON, menu = true })
w.lists = LISTS
w.run(2)
local data = homeData(w) or ""
check("home: sent to the page", data ~= "" and data ~= "null", true)
check("home: our server first, live", data:find('"title":"Meowy Network"', 1, true) ~= nil and data:find('"name":"Meowy"', 1, true) ~= nil, true)
check("home: our count", data:find('"stats":"10/64 \194\183 30 ms"', 1, true) ~= nil, true)
check("home: favourites, busiest first", (data:find('"Big RP"', 1, true) or 1e9) < (data:find('"Small"', 1, true) or 0), true)
check("home: bots left out", data:find("36/64", 1, true) ~= nil, true)
check("home: recent, with when", data:find('"detail":"gm_x \194\183 2 h ago"', 1, true) ~= nil, true)
check("home: asked once each", w.queried.favorite == 1 and w.queried.history == 1 and w.queried.friends == 1, true)
check("home: friends' servers, after ours", (data:find('"Friends are playing on"', 1, true) or 1e9) < (data:find('"Favourites"', 1, true) or 0)
	and data:find('"Pals TTT"', 1, true) ~= nil, true)
check("home: each section says how many to show", data:find('"show":5', 1, true) ~= nil and data:find('"show":3', 1, true) ~= nil, true)
check("home: never through the honest-count wrapper", w.env.serverlist.Query == nil, true)
check("home: nothing joined on its own", w.connects(), 0)
w.run(30)
check("home: not asked again within a minute", w.queried.favorite, 1)
w.run(31)
check("home: asked again after a minute", w.queried.favorite, 2)
local calls = w.homeCalls
w.run(3)
check("home: unchanged data not resent every frame", w.homeCalls - calls <= 1, true)
-- Clicks: a row number in, the address from Lua's own list.
w.env.MeowyClientPackHomeJoin(1)
check("home: row 1 joins our server", w.lastConnect(), "connect " .. OURS)
w.env.MeowyClientPackHomeJoin(2)
check("home: row 2 joins the friends' server", w.lastConnect(), "connect 8.8.4.4:27015")
w.env.MeowyClientPackHomeJoin(3)
check("home: row 3 joins the busiest favourite", w.lastConnect(), "connect 2.2.2.2:27015")
local before = w.connects()
w.env.MeowyClientPackHomeJoin(99)
w.env.MeowyClientPackHomeJoin("2); RunConsoleCommand('quit'")
w.env.MeowyClientPackHomeJoin(nil)
check("home: unknown or junk row numbers join nothing", w.connects(), before)
check("home: logged", (w.logs or ""):find("home: joining 2.2.2.2:27015", 1, true) ~= nil, true)
w.logs = ""
w.concommands.meowy_home()
check("home: meowy_home lists numbered rows", (w.logs or ""):find("  3. Big RP", 1, true) ~= nil, true)
check("home: meowy_home shows the silent count", (w.logs or ""):find("1 more isn't answering", 1, true) ~= nil, true)
w.commands = {}
w.concommands.meowy_home_join(nil, "meowy_home_join", { "4" })
check("home: meowy_home_join joins that row", w.lastConnect(), "connect 1.1.1.1:27015")
w.commands = {}
w.concommands.meowy_home_join(nil, "meowy_home_join", { "x" })
w.concommands.meowy_home_join(nil, "meowy_home_join", {})
check("home: meowy_home_join with junk joins nothing", w.connects(), 0)
-- In game: the card goes (the Escape menu is the same page), and clicks do nothing.
w.play(3)
check("home: hidden in game", homeData(w), "null")
before = w.connects()
w.env.MeowyClientPackHomeJoin(1)
check("home: no join from in game", w.connects(), before)
w.cut()
w.run(2)
check("home: back after leaving", (homeData(w) or "null") ~= "null", true)
-- A hostile server name is text, not code.
w = world({ settings = ON, menu = true })
w.lists = { favorite = { { address = "6.6.6.6:27015", name = "</script><img src=x onerror=\"lua.Run('quit')\">", players = 1, max = 2 } }, history = {} }
w.run(2)
data = homeData(w) or ""
check("home: hostile name escaped", data:find("<", 1, true) == nil and data:find("'", 1, true) == nil and data:find("\\x3c/script\\x3e", 1, true) ~= nil, true)
-- Off in settings: card removed, nothing asked.
w = world({ settings = ON, menu = true })
w.lists = LISTS
w.run(2)
w.concommands.meowy_settings()
local ht = find(function(p) return p.meowyToggle == "menu_home" end)
check("home: a toggle in settings, on", ht and ht.on, true)
if ht then ht:SetOn(false) end
w.run(2)
check("home: off removes the card", homeData(w), "null")
local asked = w.queried.favorite
w.run(70)
check("home: off asks nothing", w.queried.favorite, asked)
check("home: settings window paints", w.paintAll(), "")
-- No menu page (another menu, or none yet): nothing asked, nothing sent.
w = world({ settings = ON })
w.lists = LISTS
w.run(5)
check("home: no menu page, nothing asked", w.queried, nil)

-- The Escape menu's card: the server you're on, Reconnect, Copy, quick sliders.
local function escData(w) return (w.escJS or ""):match("window%.MeowyEsc%((.*),%d+%);\n$") end
w = world({ settings = ON, menu = true })
w.latency, w.loss, w.connected = 45.6, 0.013, 4320
w.play(3)
check("escape: nothing while the Escape menu is closed", escData(w), nil)
w.escOpen = true
w.run(1)
data = escData(w) or ""
check("escape: shown when opened", data ~= "" and data ~= "null", true)
check("escape: the server's own name", data:find('"name":"Meowy"', 1, true) ~= nil, true)
check("escape: map and players from its answer", data:find('"line1":"map \194\183 10/64 players"', 1, true) ~= nil, true)
check("escape: the engine's ping, loss, time here", data:find('"line2":"ping 46 ms \194\183 loss 1% \194\183 1 h 12 min here"', 1, true) ~= nil, true)
check("escape: the address", data:find('"address":"' .. OURS .. '"', 1, true) ~= nil, true)
check("escape: quick settings with current values", data:find('"label":"Master volume"', 1, true) ~= nil and data:find('"value":0.7', 1, true) ~= nil, true)
check("escape: the Home card isn't shown in game", (homeData(w) or "null"), "null")
w.env.MeowyClientPackEscAction("reconnect")
check("escape: Reconnect retries", w.ran("retry"), 1)
w.env.MeowyClientPackEscAction("copy")
check("escape: Copy puts the address on the clipboard", w.clipboard, OURS)
local before = #w.commands
w.env.MeowyClientPackEscAction("quit")
w.env.MeowyClientPackEscAction(nil)
check("escape: other actions do nothing", #w.commands, before)
w.env.MeowyClientPackEscSet(1, 0.42)
check("escape: volume set, snapped", w.cvars.volume, "0.40")
w.env.MeowyClientPackEscSet(3, 999)
check("escape: FOV clamped", w.cvars.fov_desired, "100")
local n = #w.commands
w.env.MeowyClientPackEscSet(7, 1)
w.env.MeowyClientPackEscSet("1); RunConsoleCommand('quit'", 1)
w.env.MeowyClientPackEscSet(2, "loud")
check("escape: unknown rows and junk set nothing", #w.commands, n)
w.run(1)
check("escape: new value shown", (escData(w) or ""):find('"value":0.4', 1, true) ~= nil, true)
w.escOpen = false
w.run(1)
check("escape: closed, card gone", escData(w), "null")
-- Not on a public server (LAN, P2P, singleplayer): no server section, sliders still.
w = world({ settings = ON, menu = true })
w.state, w.inGame, w.escOpen, w.otherNow = "other", true, true, nil
w.run(1)
data = escData(w) or ""
check("escape: no public address: no server section", data:find('"server"', 1, true) == nil and data:find('"settings":[{', 1, true) ~= nil, true)
w.env.MeowyClientPackEscAction("copy")
check("escape: nothing to copy", w.clipboard, nil)
-- At the main menu the buttons do nothing.
w = world({ settings = ON, menu = true })
w.run(1)
w.env.MeowyClientPackEscAction("reconnect")
w.env.MeowyClientPackEscSet(1, 0.1)
check("escape: out of game, nothing runs", #w.commands, 0)
-- Off in settings.
w = world({ settings = ON, menu = true })
w.play(2)
w.escOpen = true
w.run(1)
w.concommands.meowy_settings()
local et = find(function(p) return p.meowyToggle == "menu_escape" end)
check("escape: a toggle in settings, on", et and et.on, true)
if et then et:SetOn(false) end
w.run(1)
check("escape: off removes the card", escData(w), "null")
w.env.MeowyClientPackEscAction("reconnect")
check("escape: off, buttons do nothing", w.ran("retry"), 0)
check("escape: settings window paints", w.paintAll(), "")

-- The Meowy home screen (meowy_menu): our home inside the stock page, off by default.
local function shellState(w) return w.shellJS and w.shellJS:match("window%.MeowyShell%((%a+)%);\n$") end
w = world({ settings = ON, menu = true })
w.run(3)
check("shell: turned off, nothing sent", w.shellJS, nil)
check("shell: the bridge is on the browser anyway", w.bridges, 1)
check("shell: a post while it's off is ignored", (function() w.post("quit") return w.gameUI end)(), nil)
check("shell: the stock page is never reloaded", w.loads, nil)
-- On: the script and MeowyShell(true), again every second; nothing reloaded.
local PAGE_ON = { meowy_menu = "1" }
w = world({ settings = ON, menu = true, convars = PAGE_ON })
w.lists = LISTS
w.run(1)
check("shell: on, sent with its script", shellState(w) == "true" and w.shellJS:find("/*shell*/", 1, true) ~= nil, true)
local sends = w.shellSends
w.run(3)
check("shell: sent again each second (the page may be rebuilt)", w.shellSends - sends >= 2, true)
-- A page that's just loaded (the game starting, a language change) gets the
-- script on the next frame, not up to a second later; the stock callback still runs.
local readyCalls = 0
w.env.pnlMainMenu.HTML.OnDocumentReady = function() readyCalls = readyCalls + 1 end
w.env.pnlMainMenu.HTML = setmetatable({}, { __index = w.env.pnlMainMenu.HTML })   -- a new browser panel
w.env.pnlMainMenu.HTML.Callbacks = { ["lua.Run"] = w.stockRun }
w.run(0.9)   -- bridged and wrapped, and a send since
sends = w.shellSends
w.env.pnlMainMenu.HTML:OnDocumentReady("asset://garrysmod/html/menu.html")
check("shell: the stock page's ready callback still runs", readyCalls, 1)
w.run(0.1)
check("shell: a page just loaded gets it on the next frame", w.shellSends - sends, 1)
check("shell: the new panel bridged too", w.env.pnlMainMenu.HTML.Callbacks["meowy.post"] ~= nil, true)
check("shell: never a reload", w.loads, nil)
check("shell: lua.Run left alone (it's the stock page's)", w.env.pnlMainMenu.HTML.Callbacks["lua.Run"], w.stockRun)
check("shell: the lists reach it", (w.homeJS or ""):find("MeowyShellLists", 1, true) ~= nil and (homeData(w) or ""):find('"Meowy Network"', 1, true) ~= nil, true)
check("shell: Continue reaches it", (w.js or ""):find("MeowyShellContinue", 1, true) ~= nil, true)
check("shell: no settings pill", (function() local p = find(function(p) return p.text == "Meowy settings" end) return p == nil or p.visible == false end)(), true)
-- Its messages: a fixed table, each value checked.
w.post("join", 1)
check("shell: join by row number", w.lastConnect(), "connect " .. OURS)
w.post("options")
check("shell: Options", w.gameUI, "OpenOptionsDialog")
w.post("quit")
check("shell: Quit", w.gameUI, "QuitNoConfirm")
w.gameUI = nil
local n = #w.commands
w.post("RunString", "x")
w.post("join", string.rep("9", 40))
w.post(nil)
w.post("open", "servers")
w.post("settings", { "table" })
check("shell: unknown, junk or bad values do nothing", w.gameUI == nil and #w.commands == n, true)
w.last = { "1.2.3.4:27015", "Foo", w.osTime }
w.post("continue")
check("shell: Continue", w.lastConnect(), "connect 1.2.3.4:27015")
w.concommands.meowy_menu_status()
check("shell: meowy_menu_status", (w.logs or ""):find("menu page: our home, switch on", 1, true) ~= nil, true)
-- In game: off (the Escape menu is the stock one); back at the menu: on again.
w.play(2)
check("shell: in game, off", shellState(w), "false")
w.post("quit")
check("shell: in game, posts ignored", w.gameUI, nil)
w.cut()
w.run(1)
check("shell: back at the menu, on", shellState(w), "true")
check("shell: still never a reload", w.loads, nil)
-- "Use Garry's Mod's menu": off for the rest of this start; turning the switch off and on again brings it back.
w.post("stock")
w.run(3)
check("shell: the escape hatch", shellState(w), "false")
w.convars.meowy_menu = "0"
w.run(1)
w.convars.meowy_menu = "1"
w.run(1)
check("shell: switched off and on, back", shellState(w), "true")
-- Off while it shows: taken out.
w.convars.meowy_menu = "0"
w.run(1)
check("shell: off takes it out", shellState(w), "false")
local before = w.shellSends
w.run(5)
check("shell: off, not sent every second", w.shellSends, before)
-- No script in the DLL: nothing sent.
w = world({ settings = ON, menu = true, convars = PAGE_ON, noPage = true })
w.run(3)
check("shell: none built in, nothing sent", w.shellJS, nil)
-- The toggle in settings.
w = world({ settings = ON, menu = true })
w.run(1)
w.concommands.meowy_settings()
local mt = find(function(p) return p.meowyToggle == "menu_page" end)
check("shell: a toggle in settings, off", mt ~= nil and mt.on, false)
if mt then mt:SetOn(true) end
w.run(1)
check("shell: turned on from settings (through the console)", shellState(w), "true")

-- The menu's text size (meowy_menu_scale): a zoom on the page, on or off.
local function zoomJS(w)
	local z
	for _, js in ipairs(w.zoomCalls or {}) do z = js:match("style%.zoom = '([%d%.]+)'") end
	return z
end
w = world({ settings = ON, menu = true })
w.run(4)
check("zoom: auto at 1080p, nothing sent", zoomJS(w), nil)
w = world({ settings = ON, menu = true })
w.env.ScrH = function() return 1440 end
w.run(1)
check("zoom: auto at 1440p", zoomJS(w), "1.33")
local zc = #w.zoomCalls
w.run(4)
check("zoom: sent again every few seconds (the page may reload)", #w.zoomCalls > zc, true)
w.play(2)
check("zoom: in game too (the Escape menu)", zoomJS(w), "1.33")
w = world({ settings = ON, menu = true, convars = { meowy_menu_scale = "150" } })
w.run(1)
check("zoom: a chosen size", zoomJS(w), "1.50")
w.concommands.meowy_settings()
local sc = find(function(p) return p.meowyChoice == "menu_scale" end)
check("zoom: a choice in settings, showing the size", sc and sc.value, "150")
check("zoom: six choices", sc and #sc.options, 6)
check("zoom: settings window paints", w.paintAll(), "")
-- Which segment a click lands on: 600 wide, 18 padding, 6 segments of 94.
if sc then
	sc:Fit(600)
	check("zoom: a click on the 4th segment is 150%", sc.options[sc:SegmentAt(18 + 94 * 3 + 10, sc.segY + 5)][2], "150")
	check("zoom: a click on the first is Auto", sc.options[sc:SegmentAt(20, sc.segY + 5)][2], "0")
	check("zoom: a click on the text above picks nothing", sc:SegmentAt(300, sc.segY - 5), nil)
	check("zoom: a click in the padding picks nothing", sc:SegmentAt(5, sc.segY + 5), nil)
end
if sc then sc:SetValue("100") end
w.run(1)
check("zoom: chosen through the console", w.convars.meowy_menu_scale, "100")
check("zoom: back to 1", zoomJS(w), "1.00")
local n2 = #w.zoomCalls
w.run(5)
check("zoom: 1 isn't sent again and again", #w.zoomCalls, n2)

-- Join pre-warm: recorded at every join; warmed for the Continue server after
-- a few seconds at the menu, or for a row the pointer rests on.
w = world({ settings = ON, menu = true })
w.lists = LISTS
w.recorded = { [OURS] = true, ["2.2.2.2:27015"] = true }
w.state, w.address, w.loading = "ours", OURS, true
w.run(2)
check("prewarm: a join starts recording (and stops any warming)", w.pwBegins, 1)
w.play(3)
check("prewarm: in game, the list is kept once", w.pwEnds, 1)
w.run(3)
check("prewarm: not kept again", w.pwEnds, 1)
check("prewarm: nothing warmed in game", w.pwStarts, nil)
w.cut()
w.last = { OURS, "Meowy", w.osTime }
w.run(2)
check("prewarm: not at once at the menu", w.pwStarts, nil)
w.run(3)
check("prewarm: the Continue server after a few seconds", w.pwStarts and w.pwStarts[1], OURS)
w.run(10)
check("prewarm: once per visit", #w.pwStarts, 1)
w.env.MeowyClientPackHomeIntent(3)   -- row 3: the busiest favourite, 2.2.2.2
check("prewarm: a row the pointer rests on", w.pwStarts[2], "2.2.2.2:27015")
w.run(5)
check("prewarm: the Continue server doesn't take it back", #w.pwStarts, 2)
w.env.MeowyClientPackHomeIntent(4)   -- 1.1.1.1: never recorded
w.env.MeowyClientPackHomeIntent(99)
w.env.MeowyClientPackHomeIntent("x")
check("prewarm: not recorded, unknown or junk rows warm nothing", #w.pwStarts, 2)
check("prewarm: logged", (w.logs or ""):find("prewarm: warming 2.2.2.2:27015 (pointer on its row)", 1, true) ~= nil, true)
-- A join that never gets in keeps nothing.
w.loading = true
w.run(1)
w.loading = false
w.run(1)
check("prewarm: a join that didn't get in keeps nothing", w.pwBegins == 2 and w.pwEnds == 1, true)
-- Off: nothing warmed; the toggle cancels.
w = world({ settings = ON, menu = true, convars = { meowy_prewarm = "0" } })
w.recorded = { [OURS] = true }
w.last = { OURS, "Meowy", w.osTime }
w.run(6)
check("prewarm off: nothing warmed", w.pwStarts, nil)
w.concommands.meowy_settings()
local pt = find(function(p) return p.meowyToggle == "prewarm" end)
check("prewarm: a toggle in settings, off", pt ~= nil and pt.on, false)
if pt then pt:SetOn(true) end
check("prewarm: turned on through the console", w.convars.meowy_prewarm, "1")
if pt then pt:SetOn(false) end
check("prewarm: turning it off stops any warming", w.pwCancels, 1)
w.concommands.meowy_prewarm_status()
check("prewarm: status command", (w.logs or ""):find("prewarm: idle", 1, true) ~= nil, true)
check("prewarm: Workshop items not asked for when there are none", #w.ugc, 0)

-- Workshop items: of the server's last join's items, only those that need it
-- go to Steam's download, one at a time, at the same moments.
local IDS = { "101", "102", "103", "104", "105", "106" }
local function wsWorld(opts2)
	local ww = world(opts2 or { settings = ON, menu = true })
	ww.lists = LISTS
	ww.recorded = { [OURS] = true, ["2.2.2.2:27015"] = true }
	ww.workshop = { [OURS] = IDS, ["2.2.2.2:27015"] = IDS }
	ww.wsState = {
		["102"] = { false, false, false, false, 0 },            -- not installed
		["103"] = { true, true, false, false, 1600000000 },     -- Steam knows of an update
		["104"] = { true, false, false, true, 1600000000 },     -- legacy, its unpacked copy gone
		["106"] = false,                                        -- Steam can't say
	}
	ww.missing = { ["cache/workshop/104.gma"] = true }
	ww.published = { ["105"] = 1600003600 }   -- 105 republished an hour later; 101 the same, rounded up 64 s
	ww.last = { OURS, "Meowy", ww.osTime }
	return ww
end
w = wsWorld()
w.run(2)
check("workshop: nothing asked before the Continue moment", #w.ugc + #w.infos, 0)
w.run(2)
check("workshop: one download at a time", #w.ugc, 1)
check("workshop: the first that needs it", w.ugc[1].id, "102")
check("workshop: the Workshop asked for the versions of installed items", #w.infos == 2 and w.infos[1].id .. w.infos[2].id, "101105")
check("workshop: the game's own files looked in", w.existsAsked, "MOD")
check("workshop: files warmed as well", w.pwStarts and w.pwStarts[1], OURS)
w.answer()
check("workshop: an answer starts the next", w.ugc[1] and w.ugc[1].id, "103")
w.answer()
check("workshop: a legacy item without its unpacked copy", w.ugc[1] and w.ugc[1].id, "104")
w.answer(1, false)   -- Steam couldn't get it
check("workshop: nothing else known to need it yet", #w.ugc, 0)
w.answerInfos()
check("workshop: a newer Workshop version is fetched", w.ugc[1] and w.ugc[1].id, "105")
w.answer()
check("workshop: up-to-date and unknown items left alone", #w.ugc, 0)
check("workshop: logged",
	(w.logs or ""):find("checked " .. OURS .. ": 6 Workshop items, 4 needed an update or unpack, 3 done, 1 failed", 1, true) ~= nil, true)
w.concommands.meowy_prewarm_status()
check("workshop: in the status command", (w.logs or ""):find("prewarm workshop: checked " .. OURS, 1, true) ~= nil, true)
w.run(10)
check("workshop: once per visit", #w.ugc + #w.infos, 0)
-- A join stops it asking; what's in flight is left to Steam (and the join).
w.env.MeowyClientPackHomeIntent(3)   -- 2.2.2.2
check("workshop: a row the pointer rests on", w.ugc[1] and w.ugc[1].id, "102")
w.loading = true
w.run(1)
w.answer()
w.answerInfos()
check("workshop: no new asks once a join starts", #w.ugc, 0)
check("workshop: logged as stopped", (w.logs or ""):find("stopped checking 2.2.2.2:27015: 6 Workshop items, 3 needed", 1, true) ~= nil, true)
-- Steam never answers: given up after two minutes.
w = wsWorld()
w.wsState = {}   -- all installed and current, as Steam sees them
w.run(10)
check("workshop: only version checks pending", #w.ugc == 0 and #w.infos, 6)
w.run(125)
check("workshop: given up when Steam never answers", (w.logs or ""):find("stopped checking " .. OURS .. ": 6 Workshop items, 0 needed", 1, true) ~= nil, true)
w.answerInfos()
check("workshop: late answers ignored", #w.ugc, 0)
-- Its own switch: off asks nothing, while the files are still warmed.
w = wsWorld({ settings = ON, menu = true, convars = { meowy_prewarm_workshop = "0" } })
w.run(6)
check("workshop off: nothing asked", #w.ugc + #w.infos, 0)
check("workshop off: files still warmed", w.pwStarts and w.pwStarts[1], OURS)
w.concommands.meowy_settings()
local wt = find(function(p) return p.meowyToggle == "prewarm_workshop" end)
check("workshop: a toggle in settings, off", wt ~= nil and wt.on, false)
if wt then wt:SetOn(true) end
check("workshop: turned on through the console", w.convars.meowy_prewarm_workshop, "1")
-- With file warming off, the Workshop check still runs.
w = wsWorld({ settings = ON, menu = true, convars = { meowy_prewarm = "0" } })
w.run(6)
check("files off: Workshop items still checked", #w.ugc, 1)
check("files off: nothing warmed", w.pwStarts, nil)

-- The home screen left preview: on by default, and once for everyone (a saved
-- "0" from the preview days was the old default, not a choice).
w = world({ settings = ON, menu = true, fresh = true })
w.run(1)
check("home default: on with nothing saved", shellState(w), "true")
check("home default: the change marked as applied", w.convars.meowy_menu_version, "2")
w = world({ settings = ON, menu = true, fresh = true, convars = { meowy_menu = "0" } })
w.run(1)
check("home default: a preview-days 0 turned on once", w.convars.meowy_menu, "1")
check("home default: and shown", shellState(w), "true")
w = world({ settings = ON, menu = true })   -- turned off after release
w.run(1)
check("home default: a choice made since is kept", w.convars.meowy_menu .. "/" .. tostring(shellState(w)), "0/nil")
-- Its search gets your subscribed addons: sent when they change or the page reloads.
w = world({ settings = ON, menu = true, convars = PAGE_ON })
w.addons = { { title = "Wiremod", wsid = "160250458", mounted = true }, { title = "<b>Bad</b>\"", wsid = "1", mounted = false }, { title = "" } }
w.run(1)
check("addons: sent to the page", w.addonsJS ~= nil and w.addonsJS:find("Wiremod", 1, true) ~= nil, true)
check("addons: a quote in a title escaped", (w.addonsJS or ""):find("Bad", 1, true) ~= nil and (w.addonsJS or ""):find('</b>"', 1, true) == nil, true)
check("addons: empty titles left out", (w.addonsJS or ""):find("New one", 1, true) == nil and select(2, (w.addonsJS or ""):gsub("160250458", "")) == 1, true)
local sendsNow = w.addonsSends or 0
w.run(15)
check("addons: not sent again while unchanged", w.addonsSends, sendsNow)
w.addons[#w.addons + 1] = { title = "New one", wsid = "2", mounted = true }
w.run(11)
check("addons: sent again when they change", w.addonsSends, sendsNow + 1)
w.env.pnlMainMenu.HTML.OnDocumentReady(w.env.pnlMainMenu.HTML, "asset://garrysmod/html/menu.html")
w.run(1.1)
check("addons: sent again after the page reloads", w.addonsSends, sendsNow + 2)
-- Find servers, faster: its switch in settings (the setting is module.cpp's).
w = world({ settings = ON, menu = true, engineConVars = true })
w.cvars.meowy_fast_serverlist = "1"
w.concommands.meowy_settings()
local ft = find(function(p) return p.meowyToggle == "fast_serverlist" end)
check("fast server list: a toggle in settings, on", ft ~= nil and ft.on, true)
if ft then ft:SetOn(false) end
check("fast server list: turned off through the console", w.cvars.meowy_fast_serverlist, "0")
w = world({ settings = ON, menu = true })
w.concommands.meowy_settings()
check("fast server list: no toggle without the setting", find(function(p) return p.meowyToggle == "fast_serverlist" end), nil)

check("no Lua errors", w.errors, nil)
print(("%d checks, %d failed"):format(count, failures))
os.exit(failures == 0 and 0 or 1)
