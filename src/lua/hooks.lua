-- The hook registry (hooks_logic.lua as `HooksLogic`): one list of the pack's
-- join speed-ups, what each is doing and why, and the guard that turns them
-- off when joins keep crashing. The second menu chunk module.cpp runs, after
-- settings.lua: the chunks that own the hooks (module.cpp's menu Lua,
-- home.lua) ask it before they install one and tell it how that went.
--
--   meowy.HookAllowed(id)            whether the guard lets it run
--   meowy.HookResult(id, ok, why)    how installing it went (nil: taken out)
--   meowy.HookStatus()               every hook: { id, setting, name, state, why, note, describe }
--   meowy.HookOfSetting(name)        that entry for a setting's hook, or nil
--   meowy.HookPlayerChose(name)      the player turned that setting's switch
--   meowy.HookNotice()               title, body of what the guard did at start, once
--   meowy_hook_reports               the hook-health report (off unless turned on)
--   meowy_hooks_status               the list, in the console
--
-- The guard's state is hook_guard.txt (meowy.MenuFileRead/Write).
local Logic = HooksLogic
local meowy = meowy

local function log(fmt, ...) print("[Meowy Client Pack] " .. string.format(fmt, ...)) end

-- This build of the game: its version, branch and architecture.
local function gameBuild()
	if meowy.GameBuild then return meowy.GameBuild() end
	return tostring(VERSION or "?") .. "/" .. tostring(BRANCH or "?") .. "/" .. tostring(jit and jit.arch or "?")
end

local guard
local function save()
	if meowy.MenuFileWrite then meowy.MenuFileWrite("hook_guard", Logic.encode(guard)) end
end

local notice, about
do
	local last = Logic.decode(meowy.MenuFileRead and meowy.MenuFileRead("hook_guard") or "")
	-- A marker left behind is a crash only with a crash report since that join began.
	local crashed = false
	if last.joining and meowy.CrashSince then
		local ok, r = pcall(meowy.CrashSince, last.started)
		crashed = ok and r == true
	end
	local how
	guard, notice, how, about = Logic.start(last, crashed, meowy.VERSION, gameBuild())
	save()
	if how == "crashed" then
		log("hooks: the last join crashed (Garry's Mod wrote a crash report) with these on: %s",
			#last.active > 0 and table.concat(last.active, ", ") or "none")
	elseif how == "no_report" then
		log("hooks: the last join didn't finish, but Garry's Mod wrote no crash report in it: counted as clean")
	end
	if notice == "off" then
		log("hooks: joins closed the game twice in a row, so these are off: %s (round %d of %d)",
			table.concat(guard.off, ", "), guard.reverts, Logic.ROUNDS)
	elseif notice == "back_on" then
		log("hooks: a join crashed without them too, so they're back on: %s", table.concat(about or {}, ", "))
	end
end

function meowy.HookAllowed(id)
	return Logic.allowed(guard, id)
end

-- id -> { ok = true | false, why = <reason> }; nil while it isn't installed.
local results = {}
function meowy.HookResult(id, ok, why)
	if not Logic.byId(id) then return end
	if ok == nil then results[id] = nil return end
	results[id] = { ok = ok == true, why = ok ~= true and tostring(why or "") or nil }
end

-- What each hook's own module says about itself.
local DESCRIBE = {
	file_checks = "FastFileChecksState", lua_lookup = "FastLuaLookupState", lua_find = "FastLuaFindState",
	fonts = "FastFontsState", pak_cache = "PakCacheState", prewarm = "PrewarmState", mat_reload = "MaterialReloadState",
}

local function policyOff(name)
	-- settings.lua's own answer: the per-build lines for this game included.
	if meowy.PolicyOff then return meowy.PolicyOff(name) end
	local st = meowy.PolicyState and meowy.PolicyState()
	if not istable(st) then return false end
	if st.pack_enabled == false then return true end
	for _, n in ipairs(st.off or {}) do
		if n == name then return true end
	end
	return false
end

local function entry(h)
	local o = meowy.SettingOf and meowy.SettingOf(h.setting)
	if not o then return nil end   -- this build doesn't have it
	local pOff = policyOff(h.setting)
	local r = results[h.id]
	local state = Logic.state(o:GetBool(), pOff, Logic.allowed(guard, h.id), r and r.ok)
	local why = r and r.why or nil
	return {
		id = h.id, setting = h.setting, name = h.name, state = state, why = why,
		note = Logic.settingsNote(state, why),
		attention = Logic.needsAttention(state),
	}
end

function meowy.HookStatus()
	local out = {}
	for _, h in ipairs(Logic.HOOKS) do
		local e = entry(h)
		if e then out[#out + 1] = e end
	end
	return out
end

function meowy.HookOfSetting(name)
	local h = Logic.bySetting(name)
	return h and entry(h) or nil
end

function meowy.HookPlayerChose(name)
	local h = Logic.bySetting(name)
	if not h then return end
	if Logic.allowed(guard, h.id) and guard.unclean == 0 then return end
	guard = Logic.playerChose(guard, h.id)
	save()
end

function meowy.HookNotice()
	if not notice then return nil end
	local title, body = Logic.noticeText(notice, about)
	notice, about = nil, nil
	return title, body
end

concommand.Add("meowy_hooks_status", function()
	print("[Meowy Client Pack] hooks, on Garry's Mod " .. gameBuild() .. ":")
	for _, e in ipairs(meowy.HookStatus()) do
		local f = meowy[DESCRIBE[e.id] or ""]
		local ok, describe = pcall(function() return isfunction(f) and f() or nil end)
		print("  " .. Logic.line(e, e.state, e.why, ok and isstring(describe) and describe or nil))
	end
end)

-- The hook-health report: what each hook is doing on this build of the game,
-- to Meowy Network. Off unless the player turns it on (the welcome window asks,
-- and Meowy settings, Privacy, has the switch). Sent once the states are known
-- (a join got in, or two minutes at the menu), at most once a day for the same
-- content. No SteamID, no install id: hooks_logic.lua's M.report is all of it.
local reports = meowy.Setting and meowy.Setting("meowy_hook_reports", "0",
	"Meowy Client Pack: tell Meowy Network which join speed-ups run on this version of Garry's Mod (the pack's version, the game's build, each speed-up's state and why one isn't running; nothing about you) (0 = off)")
local REPORT_AFTER = 120   -- seconds at the menu before the states count as known
local startedAt, joinedOnce, nextReportCheck = SysTime(), false, 0
local function maybeReport()
	if not (reports and reports:GetBool() and meowy.HookReport) then return end
	local body = Logic.report(meowy.VERSION, gameBuild(), meowy.HookStatus())
	if not Logic.shouldReport(guard, true, body, os.time()) then return end
	if meowy.HookReport(body) ~= true then return end
	guard = Logic.reported(guard, body, os.time())
	save()
	log("hooks: told Meowy Network what the join speed-ups are doing on Garry's Mod %s (meowy_hook_reports 0 turns that off)", gameBuild())
end

-- The join's marker: written as loading starts, with the hooks that are on;
-- cleared when the join gets in, or ends back at the menu.
local marked = false
hook.Add("Think", "MeowyClientPack.HookGuard", function()
	local loading, inGame = IsInLoading(), IsInGame()
	local now = SysTime()
	if now >= nextReportCheck and not loading then
		nextReportCheck = now + 15
		if joinedOnce or now - startedAt > REPORT_AFTER then maybeReport() end
	end
	if loading and not marked then
		marked = true
		local active = {}
		for _, e in ipairs(meowy.HookStatus()) do
			if e.state == "on" or e.state == "waiting" then active[#active + 1] = e.id end
		end
		guard = Logic.joinStart(guard, active, os.time())
		save()
	elseif marked and not loading then
		marked = false
		guard = Logic.joinEnd(guard, inGame)
		save()
		if inGame then joinedOnce, nextReportCheck = true, 0 end
	end
end)
