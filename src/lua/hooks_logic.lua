-- The hook registry's rules, as pure functions (no GMod API), so they can be
-- tested with a plain Lua interpreter (tests/hooks_logic_test.lua).
-- Lua 5.1 compatible: Garry's Mod runs LuaJIT.
--
-- The pack's join speed-ups each change the game in memory (docs/TRUST.md).
-- This is the one list of them, with what each is doing now and why, and the
-- guard that turns them off when joins keep crashing:
--   * As a join starts loading, the hooks that are on are written to
--     hook_guard.txt with a `joining` marker and the time. The marker is
--     cleared when the join gets in, or ends back at the main menu.
--   * A marker left behind counts as a crashed join only when Garry's Mod's
--     own crash handler wrote a report after it (meowy.CrashSince), as for
--     multicore rendering (multicore_logic.lua). A game closed or killed while
--     loading, with no report, isn't a crash.
--   * Two crashed joins in a row, with no join that got in between: the hooks
--     that were on in the last one go off, and the player is told. Which one
--     it was can't be known from a crash, so all of them go.
--   * A join that crashes with them off too means they weren't the cause:
--     back on. After a second round they stay off.
--   * A new version of the pack or of Garry's Mod starts the guard over: what
--     crashed may be fixed, and what a hook patches has changed.
--   * The player's own switch (Meowy settings) is their decision: that hook
--     comes back, and the count starts over.
-- The guard never changes a setting: its "off" is its own, kept here.
--
-- Also here: the hook-health report (meowy_hook_reports, off unless the player
-- turns it on). What every hook is doing on this build of the game, sent to
-- Meowy Network at most once a day, and again only when something changed. It
-- carries the pack's version, the game's build, each hook's state and a
-- refusal's reason: nothing about the player, the PC or the servers.

local M = {}

M.CRASHES_BEFORE_OFF = 2
M.ROUNDS = 2   -- turned off this many times: they stay off until the player, or a new version

-- id: the name in hook_guard.txt and in the status command. setting: the
-- convar that turns it on. name: what the player reads.
M.HOOKS = {
	{ id = "file_checks", setting = "meowy_fast_file_checks", name = "fast file checks" },
	{ id = "lua_lookup", setting = "meowy_fast_lua_lookup", name = "fast Lua lookup" },
	{ id = "lua_find", setting = "meowy_fast_lua_find", name = "fast file.Find" },
	{ id = "fonts", setting = "meowy_fast_fonts", name = "fast fonts" },
	{ id = "pak_cache", setting = "meowy_pak_cache", name = "pak cache" },
	{ id = "prewarm", setting = "meowy_prewarm", name = "join pre-warm" },
	{ id = "mat_reload", setting = "meowy_skipmatreload_anyserver", name = "material reload skip" },
}

local byId, bySetting = {}, {}
for _, h in ipairs(M.HOOKS) do byId[h.id], bySetting[h.setting] = h, h end
function M.byId(id) return byId[id] end
function M.bySetting(name) return bySetting[name] end

local function int(v)
	local n = tonumber(v)
	if not n or n ~= n or n < 0 then return 0 end
	return math.min(math.floor(n), 1000)
end

-- A comma-separated list of known ids, each once, in M.HOOKS' order.
local function ids(text)
	local seen = {}
	for id in string.gmatch(type(text) == "string" and text or "", "[%w_]+") do
		if byId[id] then seen[id] = true end
	end
	local out = {}
	for _, h in ipairs(M.HOOKS) do if seen[h.id] then out[#out + 1] = h.id end end
	return out
end
local function has(list, id)
	for _, v in ipairs(list or {}) do if v == id then return true end end
	return false
end
-- A version or build as it's kept: one short line, nothing that ends it.
local function tag(v)
	return string.sub((string.gsub(tostring(v or ""), "[^%w%._/%-]", "")), 1, 64)
end

-- hook_guard.txt: "name=value" lines (LF; a CR before it is dropped).
-- joining: a join was loading and hasn't ended (0/1). started: when it began
-- (unix time). active: the hooks that were on in it. unclean: crashed joins in
-- a row. off: the hooks the guard has off. reverted: the guard turned them off
-- and watches whether joins still crash without them. reverts: times it turned
-- them off. pack, game: the versions all of that is about. report_day,
-- report_sig: the day (days since 1970, UTC) and content of the last
-- hook-health report sent.
function M.decode(text)
	local raw = {}
	for line in string.gmatch((type(text) == "string" and text or "") .. "\n", "([^\n]*)\n") do
		line = string.gsub(line, "\r$", "")
		local k, v = string.match(line, "^([%w_]+)=(.*)$")
		if k and raw[k] == nil then raw[k] = v end
	end
	return {
		joining = raw.joining == "1",
		started = math.max(0, math.floor(tonumber(raw.started) or 0)),
		active = ids(raw.active),
		unclean = int(raw.unclean),
		off = ids(raw.off),
		reverted = raw.reverted == "1",
		reverts = int(raw.reverts),
		pack = tag(raw.pack),
		game = tag(raw.game),
		reportDay = math.max(0, math.floor(tonumber(raw.report_day) or 0)),
		reportSig = string.match(raw.report_sig or "", "^%x%x%x%x%x%x%x%x$") or "",
	}
end

function M.encode(s)
	local function b(v) return v and "1" or "0" end
	return table.concat({
		"joining=" .. b(s.joining),
		"started=" .. string.format("%d", math.max(0, math.floor(tonumber(s.started) or 0))),
		"active=" .. table.concat(ids(table.concat(s.active or {}, ",")), ","),
		"unclean=" .. int(s.unclean),
		"off=" .. table.concat(ids(table.concat(s.off or {}, ",")), ","),
		"reverted=" .. b(s.reverted),
		"reverts=" .. int(s.reverts),
		"pack=" .. tag(s.pack),
		"game=" .. tag(s.game),
		"report_day=" .. string.format("%d", math.max(0, math.floor(tonumber(s.reportDay) or 0))),
		"report_sig=" .. (string.match(tostring(s.reportSig or ""), "^%x%x%x%x%x%x%x%x$") or ""),
	}, "\n") .. "\n"
end

local function copy(s)
	local t = {}
	for k, v in pairs(s or {}) do
		if type(v) == "table" then
			local l = {}
			for i, x in ipairs(v) do l[i] = x end
			t[k] = l
		else
			t[k] = v
		end
	end
	t.active, t.off = t.active or {}, t.off or {}
	return t
end

-- Whether the last join crashed: its marker left behind and a crash report
-- written since it started. Also why: "clean", "crashed" or "no_report".
function M.lastJoin(s, crashed)
	if not (s and s.joining == true) then return false, "clean" end
	if crashed == true then return true, "crashed" end
	return false, "no_report"
end

-- At start. s: the state read back (M.decode). crashed: Garry's Mod wrote a
-- crash report after s.started (asked only when s.joining). pack, game: this
-- start's versions.
-- Returns the new state (no join marked), what to tell the player: "off"
-- (hooks turned off after crashed joins), "back_on" (crashed without them
-- too) or nil; how the last join ended (M.lastJoin's reason); and the hooks a
-- notice is about.
function M.start(s, crashed, pack, game)
	s = copy(s)
	pack, game = tag(pack), tag(game)
	local lastCrashed, how = M.lastJoin(s, crashed)
	local lastActive = s.active
	local notice, about

	if s.pack ~= pack or s.game ~= game then
		-- A new pack or a new game: whatever was counted is about other code.
		s.off, s.reverted, s.reverts, s.unclean = {}, false, 0, 0
		s.pack, s.game = pack, game
		lastCrashed = false
	end

	if lastCrashed and #lastActive == 0 and s.reverted and #s.off > 0 then
		-- Still crashing with them off: they weren't the cause.
		about = s.off
		s.off, s.reverted, s.unclean = {}, false, 0
		notice = "back_on"
	elseif lastCrashed and #lastActive > 0 then
		s.unclean = int(s.unclean) + 1
		if s.unclean >= M.CRASHES_BEFORE_OFF then
			local off = {}
			for _, h in ipairs(M.HOOKS) do
				if has(s.off, h.id) or has(lastActive, h.id) then off[#off + 1] = h.id end
			end
			s.off, s.unclean = off, 0
			s.reverts = int(s.reverts) + 1
			-- Only the first time does it watch for crashes without them.
			s.reverted = s.reverts < M.ROUNDS
			notice, about = "off", lastActive
		end
	end

	s.joining, s.active, s.started = false, {}, 0
	return s, notice, how, about
end

-- A join starts loading with these hooks on (ids). now: unix time.
function M.joinStart(s, active, now)
	s = copy(s)
	s.joining = true
	s.active = ids(table.concat(active or {}, ","))
	s.started = math.max(0, math.floor(tonumber(now) or 0))
	return s
end

-- The join ended without a crash. reached: it got into the game (the count
-- of crashed joins in a row starts over); otherwise it ended back at the menu.
function M.joinEnd(s, reached)
	s = copy(s)
	s.joining, s.active, s.started = false, {}, 0
	if reached then s.unclean = 0 end
	return s
end

-- Whether the guard lets this hook run.
function M.allowed(s, id)
	return not has(s and s.off, id)
end

-- The player turned this hook's switch themselves: it comes back, and the
-- guard starts over.
function M.playerChose(s, id)
	s = copy(s)
	local off = {}
	for _, v in ipairs(s.off) do if v ~= id then off[#off + 1] = v end end
	s.off, s.unclean = off, 0
	if #off == 0 then s.reverted = false end
	return s
end

-- What a hook is doing: "on"; "setting_off" (the player's switch);
-- "policy_off" (Meowy Network's kill switch); "guard_off" (after crashed
-- joins); "refused" (it didn't recognise this build of the game, with the
-- reason it gave); "waiting" (on, and not asked for yet: some only go in as a
-- join starts).
-- settingOn: the player's own value. policyOff: the kill switch has it off.
-- result: nil (not tried), true, or false with `why`.
function M.state(settingOn, policyOff, guardAllows, result)
	if policyOff then return "policy_off" end
	if not settingOn then return "setting_off" end
	if not guardAllows then return "guard_off" end
	if result == false then return "refused" end
	if result == true then return "on" end
	return "waiting"
end

-- Whether a hook in this state needs the player's eye: it should be running
-- and isn't.
function M.needsAttention(state)
	return state == "refused" or state == "guard_off"
end

-- One line of the status command.
function M.line(hook, state, why, describe)
	local text
	if state == "on" then text = "on"
	elseif state == "waiting" then text = "on (goes in when a join starts)"
	elseif state == "setting_off" then text = "off (turned off in Meowy settings)"
	elseif state == "policy_off" then text = "off (switched off by Meowy Network)"
	elseif state == "guard_off" then text = "off (joins closed the game twice in a row with it on; turn it on in Meowy settings to try again)"
	else text = "not running: " .. (why and why ~= "" and tostring(why) or "it didn't recognise this version of Garry's Mod") end
	if describe and describe ~= "" and (state == "on" or state == "waiting") then
		-- Most modules' own words start with "on" too.
		local rest = string.gsub(tostring(describe), "^on[:,;]?%s*", "")
		if rest ~= "" then text = text .. "; " .. rest end
	end
	return string.format("%-22s %s", hook.name, text)
end

-- What Meowy settings adds under a hook's switch, or nil.
function M.settingsNote(state, why)
	if state == "refused" then
		return "Not running right now: it doesn't recognise this version of Garry's Mod"
			.. (why and why ~= "" and (" (" .. tostring(why) .. ")") or "")
			.. ", so the game does this its own way. It comes back with an update of the pack."
	elseif state == "guard_off" then
		return "Off right now: the game closed unexpectedly twice in a row while joining a server with it on. "
			.. "Turn it on to try again."
	elseif state == "policy_off" then
		return "Switched off by Meowy Network for now."
	end
	return nil
end

local function names(list)
	local out = {}
	for _, id in ipairs(list or {}) do if byId[id] then out[#out + 1] = byId[id].name end end
	return table.concat(out, ", ")
end

-- --- The hook-health report ------------------------------------------------------

M.REPORT_WHY_MAX = 160

-- A refusal's reason as it's sent: printable ASCII on one line, cut short.
local function reportWhy(why)
	local s = string.gsub(tostring(why or ""), "[^\32-\126]", " ")
	s = string.gsub(string.gsub(s, "%s+", " "), "^ ?(.-) ?$", "%1")
	return string.sub(s, 1, M.REPORT_WHY_MAX)
end
local function jsonString(s)
	return '"' .. string.gsub(s, '[\\"]', function(c) return "\\" .. c end) .. '"'
end

-- The report's body (JSON). entries: { id, state, why } per hook, as the glue
-- lists them. Only known hooks, and a reason only for a refusal.
function M.report(pack, game, entries)
	local hooks, whys = {}, {}
	for _, e in ipairs(entries or {}) do
		if byId[e.id] and type(e.state) == "string" and string.match(e.state, "^[a-z_]+$") then
			hooks[#hooks + 1] = jsonString(e.id) .. ":" .. jsonString(e.state)
			if e.state == "refused" then
				local why = reportWhy(e.why)
				if why ~= "" then whys[#whys + 1] = jsonString(e.id) .. ":" .. jsonString(why) end
			end
		end
	end
	if #hooks == 0 then return nil end
	return '{"v":1,"pack":' .. jsonString(tag(pack)) .. ',"game":' .. jsonString(tag(game))
		.. ',"hooks":{' .. table.concat(hooks, ",") .. '},"why":{' .. table.concat(whys, ",") .. "}}"
end

-- Eight hex digits that change when the report's content does.
function M.signature(text)
	local h = 5381
	for i = 1, #(text or "") do
		h = (h * 33 + string.byte(text, i)) % 4294967296
	end
	return string.format("%08x", h)
end

-- Days since 1970 (UTC) for a unix time.
function M.day(now)
	return math.max(0, math.floor((tonumber(now) or 0) / 86400))
end

-- Whether to send this report now. optIn: the player's setting. Once a day at
-- most for the same content; a change (a hook refused after a game update)
-- goes at once.
function M.shouldReport(s, optIn, body, now)
	if optIn ~= true or not body then return false end
	return (s and s.reportSig or "") ~= M.signature(body) or (s and s.reportDay or 0) ~= M.day(now)
end

-- The state after a report went out.
function M.reported(s, body, now)
	s = copy(s)
	s.reportDay, s.reportSig = M.day(now), M.signature(body)
	return s
end

-- The prompt for a notice from M.start: title, body.
function M.noticeText(notice, about)
	if notice == "off" then
		return "Some join speed-ups are off",
			"Garry's Mod closed unexpectedly twice in a row while joining a server, so the pack turned off the "
			.. "speed-ups that were on (" .. names(about) .. "). Joins take a little longer without them. "
			.. "Meowy settings, Performance, turns each one back on."
	elseif notice == "back_on" then
		return "The join speed-ups are back on",
			"Garry's Mod closed unexpectedly while joining without them too, so they weren't the cause. "
			.. "They're on again (" .. names(about) .. ")."
	end
	return nil
end

return M
