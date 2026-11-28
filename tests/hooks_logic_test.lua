-- The hook registry's rules (src/lua/hooks_logic.lua) and its menu glue
-- (src/lua/hooks.lua) against a stubbed menu state, no game:
--   lua tests/hooks_logic_test.lua        (from the repository root; Lua 5.1+)
-- What it can't show: that a hook's own Install refuses a game build it
-- doesn't know (the *_smoke tests), or how the settings window looks.
local here = (arg and arg[0] or ""):match("^(.*)[/\\]") or "."
local function readFile(p) local f = assert(io.open(p, "rb")) local s = f:read("*a") f:close() return s end
local M = dofile(here .. "/../src/lua/hooks_logic.lua")
local loadstring = loadstring or load

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
local function list(t) return table.concat(t or {}, ",") end

local PACK, GAME = "1.9.0", "250101/unknown/x64"
local ALL = {}
for _, h in ipairs(M.HOOKS) do ALL[#ALL + 1] = h.id end

-- --- The file ------------------------------------------------------------------
do
	local s = M.decode("")
	check("empty: no join marked", s.joining, false)
	check("empty: nothing off", #s.off, 0)
	local text = M.encode({ joining = true, started = 1700000000, active = { "lua_find", "file_checks" }, unclean = 1,
		off = { "fonts" }, reverted = true, reverts = 1, pack = PACK, game = GAME })
	local back = M.decode(text)
	check("round trip: joining", back.joining, true)
	check("round trip: started", back.started, 1700000000)
	check("round trip: active, in the registry's order", list(back.active), "file_checks,lua_find")
	check("round trip: off", list(back.off), "fonts")
	check("round trip: unclean", back.unclean, 1)
	check("round trip: reverted", back.reverted, true)
	check("round trip: pack", back.pack, PACK)
	check("round trip: game", back.game, GAME)
	check("encode is stable", M.encode(back), text)

	local odd = M.decode("joining=1\r\nactive=nope,fonts,fonts,../x\nunclean=-4\noff=lua_find;rm\npack=1.9.0\nrm -rf\ngame=a b\tc\n")
	check("CR dropped", odd.joining, true)
	check("unknown ids dropped, each once", list(odd.active), "fonts")
	check("a negative count reads as 0", odd.unclean, 0)
	check("only ids are read from a list", list(odd.off), "lua_find")
	check("a version keeps no spaces", odd.game, "abc")
	check("every setting has one hook", M.bySetting("meowy_pak_cache").id, "pak_cache")
	check("an unknown setting has none", M.bySetting("meowy_multicore"), nil)
end

-- --- The guard -----------------------------------------------------------------
local function fresh() return (M.start(M.decode(""), false, PACK, GAME)) end
-- One join that crashes while loading, then the next start.
local function crashedJoin(s, active)
	return M.start(M.decode(M.encode(M.joinStart(s, active, 100))), true, PACK, GAME)
end

do
	local s = fresh()
	check("fresh: everything allowed", M.allowed(s, "lua_lookup"), true)

	-- A join that gets in leaves no marker.
	local j = M.joinStart(s, ALL, 100)
	check("join start: marked", j.joining, true)
	check("join start: its hooks", list(j.active), list(ALL))
	local done = M.joinEnd(j, true)
	check("join end: no marker", done.joining, false)
	local after, notice, how = M.start(M.decode(M.encode(done)), true, PACK, GAME)
	check("a clean join isn't a crash, whatever the crash folder holds", how, "clean")
	check("clean: no notice", notice, nil)
	check("clean: nothing off", #after.off, 0)

	-- A marker with no crash report: closed while loading.
	local s2, n2, how2 = M.start(M.decode(M.encode(j)), false, PACK, GAME)
	check("no report: counted as clean", how2, "no_report")
	check("no report: not counted", s2.unclean, 0)
	check("no report: no notice", n2, nil)

	-- One crashed join: counted, nothing off yet.
	local s3, n3, how3 = crashedJoin(s, ALL)
	check("one crash: how", how3, "crashed")
	check("one crash: counted", s3.unclean, 1)
	check("one crash: nothing off", #s3.off, 0)
	check("one crash: no notice", n3, nil)
	check("one crash: the marker is gone", s3.joining, false)

	-- A join that gets in between starts the count over.
	local between = M.joinEnd(M.joinStart(s3, ALL, 200), true)
	check("a good join resets the count", between.unclean, 0)
	local s4 = crashedJoin(between, ALL)
	check("then one crash is one again", s4.unclean, 1)
	check("and nothing is off", #s4.off, 0)

	-- A join that ends back at the menu doesn't reset it.
	check("a failed join keeps the count", M.joinEnd(M.joinStart(s3, ALL, 200), false).unclean, 1)

	-- Two in a row: the hooks that were on go off.
	local active = { "file_checks", "lua_lookup", "pak_cache" }
	local s5, n5, _, about5 = crashedJoin(s3, active)
	check("two crashes: notice", n5, "off")
	check("two crashes: those hooks off", list(s5.off), "file_checks,lua_lookup,pak_cache")
	check("two crashes: about them", list(about5), "file_checks,lua_lookup,pak_cache")
	check("two crashes: the others stay", M.allowed(s5, "fonts"), true)
	check("two crashes: off isn't allowed", M.allowed(s5, "pak_cache"), false)
	check("two crashes: watching", s5.reverted, true)
	check("two crashes: count starts over", s5.unclean, 0)
	local title, body = M.noticeText(n5, about5)
	check("the notice names them", title ~= nil and body:find("pak cache", 1, true) ~= nil)

	-- Crashing with none of them on: not the cause, back on.
	local s6, n6, _, about6 = crashedJoin(s5, {})
	check("crash without them: back on", n6, "back_on")
	check("crash without them: nothing off", #s6.off, 0)
	check("crash without them: about what came back", list(about6), "file_checks,lua_lookup,pak_cache")
	check("crash without them: not watching", s6.reverted, false)

	-- A crash with only the others on, while some are off, is counted for those.
	local s6b = crashedJoin(s5, { "fonts" })
	check("crash with other hooks on: no back-on", list(s6b.off), "file_checks,lua_lookup,pak_cache")
	check("crash with other hooks on: counted", s6b.unclean, 1)

	-- Second round: they stay off.
	local s7 = crashedJoin(s6, active)
	local s8, n8 = crashedJoin(s7, active)
	check("second round: off again", n8, "off")
	check("second round: not watching any more", s8.reverted, false)
	local s9, n9 = crashedJoin(s8, {})
	check("second round: a crash without them doesn't bring them back", n9, nil)
	check("second round: still off", list(s9.off), "file_checks,lua_lookup,pak_cache")

	-- The player's switch.
	local p = M.playerChose(s8, "lua_lookup")
	check("player: that hook is back", M.allowed(p, "lua_lookup"), true)
	check("player: the others aren't", M.allowed(p, "pak_cache"), false)
	p = M.playerChose(M.playerChose(p, "pak_cache"), "file_checks")
	check("player: all back", #p.off, 0)
	check("player: no watch left", p.reverted, false)

	-- A new pack or a new game starts over, and a crash from the old one doesn't count.
	local n1 = M.start(M.decode(M.encode(M.joinStart(s8, {}, 300))), true, "1.9.1", GAME)
	check("new pack: nothing off", #n1.off, 0)
	check("new pack: rounds start over", n1.reverts, 0)
	check("new pack: the old crash isn't counted", n1.unclean, 0)
	check("new pack: remembered", n1.pack, "1.9.1")
	local n2b = M.start(M.decode(M.encode(s8)), false, PACK, "250202/unknown/x64")
	check("new game build: nothing off", #n2b.off, 0)
	local same = M.start(M.decode(M.encode(s8)), false, PACK, GAME)
	check("same versions: still off", list(same.off), "file_checks,lua_lookup,pak_cache")
end

-- --- States and wording --------------------------------------------------------
do
	check("policy wins", M.state(true, true, true, true), "policy_off")
	check("setting off", M.state(false, false, true, true), "setting_off")
	check("guard off", M.state(true, false, false, true), "guard_off")
	check("refused", M.state(true, false, true, false), "refused")
	check("on", M.state(true, false, true, true), "on")
	check("not asked yet", M.state(true, false, true, nil), "waiting")
	check("refused needs attention", M.needsAttention("refused"), true)
	check("guard off needs attention", M.needsAttention("guard_off"), true)
	check("the player's own off doesn't", M.needsAttention("setting_off"), false)
	check("on has no note", M.settingsNote("on"), nil)
	check("the player's own off has no note", M.settingsNote("setting_off"), nil)
	check("refused: the note carries the reason", M.settingsNote("refused", "pattern not found"):find("pattern not found", 1, true) ~= nil)
	check("guard off: the note says how to try again", M.settingsNote("guard_off"):find("Turn it on", 1, true) ~= nil)
	local h = M.byId("fonts")
	check("status line: refused", M.line(h, "refused", "gdi32 import not found"):find("not running: gdi32 import not found", 1, true) ~= nil)
	check("status line: on, with the module's own words", M.line(h, "on", nil, "412 loads skipped"):find("on; 412 loads skipped", 1, true) ~= nil)
	check("status line: the module's own \"on\" isn't said twice", M.line(h, "on", nil, "on: 459 loads saved"):find("on; 459 loads saved", 1, true) ~= nil)
check("status line: off has no describe", M.line(h, "setting_off", nil, "x"):find("; x", 1, true), nil)
end

-- --- The glue, in a stubbed menu state -----------------------------------------
local chunk = "local HooksLogic = (function()\n" .. readFile(here .. "/../src/lua/hooks_logic.lua") .. "\nend)()\n"
	.. readFile(here .. "/../src/lua/hooks.lua")
local function world(opts)
	opts = opts or {}
	local w = { file = opts.file or "", loading = false, inGame = false, now = 1000, printed = {}, settings = {}, hooks = {}, cmds = {} }
	local env = setmetatable({}, { __index = _G })
	env.VERSION, env.BRANCH, env.jit = 250101, "unknown", { arch = "x64" }
	env.print = function(s) w.printed[#w.printed + 1] = tostring(s) end
	env.istable = function(v) return type(v) == "table" end
	env.isstring = function(v) return type(v) == "string" end
	env.isfunction = function(v) return type(v) == "function" end
	env.IsInLoading = function() return w.loading end
	env.IsInGame = function() return w.inGame end
	env.os = setmetatable({ time = function() return w.now end }, { __index = os })
	env.hook = { Add = function(_, id, f) w.hooks[id] = f end }
	env.concommand = { Add = function(name, f) w.cmds[name] = f end }
	for _, h in ipairs(M.HOOKS) do w.settings[h.setting] = true end
	env.meowy = {
		VERSION = opts.pack or PACK,
		MenuFileRead = function(kind) w.readKind = kind return w.file end,
		MenuFileWrite = function(kind, text) w.writeKind = kind w.file = text return true end,
		CrashSince = function(since) w.crashAsked = since return opts.crashReport == true end,
		SettingOf = function(name)
			if w.settings[name] == nil then return nil end
			return { GetBool = function() return w.settings[name] == true and not (w.policy or {})[name] end }
		end,
		PolicyState = function()
			local off = {}
			for n in pairs(w.policy or {}) do off[#off + 1] = n end
			return { pack_enabled = true, off = off }
		end,
		FastFontsState = function() return "412 loads skipped" end,
	}
	local f = assert(loadstring(chunk, "hooks"))
	if setfenv then setfenv(f, env) else f = assert(load(chunk, "hooks", "t", env)) end
	f()
	w.env, w.meowy = env, env.meowy
	w.think = function() w.hooks["MeowyClientPack.HookGuard"]() end
	w.state = function(id)
		for _, e in ipairs(env.meowy.HookStatus()) do if e.id == id then return e.state, e end end
	end
	return w
end

do
	local w = world()
	check("glue: reads its own file", w.readKind, "hook_guard")
	check("glue: writes it at start", w.writeKind, "hook_guard")
	check("glue: this start's versions are kept", M.decode(w.file).game, GAME)
	check("glue: every hook listed", #w.meowy.HookStatus(), #M.HOOKS)
	check("glue: on and not asked for yet", (w.state("lua_lookup")), "waiting")
	w.meowy.HookResult("lua_lookup", true)
	w.meowy.HookResult("fonts", false, "gdi32 import not found")
	check("glue: installed", (w.state("lua_lookup")), "on")
	local st, e = w.state("fonts")
	check("glue: refused", st, "refused")
	check("glue: refused carries the reason", e.why, "gdi32 import not found")
	check("glue: refused has a note for settings", e.note ~= nil and e.attention == true)
	check("glue: by setting", w.meowy.HookOfSetting("meowy_fast_fonts").state, "refused")
	check("glue: a setting without a hook", w.meowy.HookOfSetting("meowy_multicore"), nil)
	w.meowy.HookResult("fonts", nil)
	check("glue: taken out again, the result goes", (w.state("fonts")), "waiting")
	w.settings.meowy_pak_cache = false
	check("glue: the player's switch", (w.state("pak_cache")), "setting_off")
	w.policy = { meowy_fast_lua_find = true }
	check("glue: the kill switch", (w.state("lua_find")), "policy_off")
	w.settings.meowy_prewarm = nil
	check("glue: a setting this build doesn't have isn't listed", (w.state("prewarm")), nil)

	-- A join: marked with what's on, cleared when it gets in.
	w.meowy.HookResult("fonts", false, "x")
	w.loading = true
	w.think()
	local marked = M.decode(w.file)
	check("glue: the join is marked", marked.joining, true)
	check("glue: with the time", marked.started, 1000)
	check("glue: with the hooks that are on", list(marked.active), "file_checks,lua_lookup,mat_reload")
	w.loading, w.inGame = false, true
	w.think()
	check("glue: in game, the marker is gone", M.decode(w.file).joining, false)

	w.cmds.meowy_hooks_status()
	local out = table.concat(w.printed, "\n")
	check("status command: the game build", out:find(GAME, 1, true) ~= nil)
	check("status command: a refusal", out:find("fast fonts%s+not running: x") ~= nil)
	check("status command: a kill switch", out:find("fast file.Find%s+off %(switched off by Meowy Network%)") ~= nil)
end

do
	-- Two joins crash while loading: at the third start those hooks are off.
	local w = world()
	w.loading = true
	w.think()
	local w2 = world({ file = w.file, crashReport = true })
	check("glue: the crash is asked about from the join's start", w2.crashAsked, 1000)
	check("glue: one crash turns nothing off", w2.meowy.HookAllowed("lua_lookup"), true)
	check("glue: one crash, no notice", w2.meowy.HookNotice(), nil)
	w2.loading = true
	w2.think()
	local w3 = world({ file = w2.file, crashReport = true })
	check("glue: two crashes, off", w3.meowy.HookAllowed("lua_lookup"), false)
	check("glue: the state says so", (w3.state("lua_lookup")), "guard_off")
	local title = w3.meowy.HookNotice()
	check("glue: the player is told", title, "Some join speed-ups are off")
	check("glue: once", w3.meowy.HookNotice(), nil)
	-- With them off, the next join is marked with nothing on.
	w3.loading = true
	w3.think()
	check("glue: a join with them off has none active", #M.decode(w3.file).active, 0)
	w3.loading = false
	w3.think()
	-- The player turns one back on.
	w3.meowy.HookPlayerChose("meowy_fast_lua_lookup")
	check("glue: the player's switch brings it back", w3.meowy.HookAllowed("lua_lookup"), true)
	check("glue: and it's saved", M.allowed(M.decode(w3.file), "lua_lookup"), true)
	check("glue: the others stay off", w3.meowy.HookAllowed("fonts"), false)
	-- A new version of the pack starts over.
	local w4 = world({ file = w3.file, pack = "1.9.1" })
	check("glue: a new pack version has everything on", w4.meowy.HookAllowed("fonts"), true)
	-- The same join closed without a crash report isn't counted.
	local w5 = world({ file = w.file, crashReport = false })
	check("glue: no crash report, not counted", M.decode(w5.file).unclean, 0)
end

print(("%d checks, %d failures"):format(count, failures))
os.exit(failures == 0 and 0 or 1)
