-- Multicore rendering's crash guard, as pure functions (no GMod API), so it
-- can be tested with a plain Lua interpreter (tests/multicore_logic_test.lua).
-- Lua 5.1 compatible: Garry's Mod runs LuaJIT.
--
-- The pack turns gmod_mcore_test on at every start (multicore.lua). The same
-- guard as Meowy Network's FPS Booster (its cl_menu.lua), kept in
-- multicore_guard.txt:
--   * A `running` marker with the mode (whether multicore was on), written at
--     start and cleared at a clean exit. Still set at the next start: that
--     session ended uncleanly (a crash, or the game killed).
--   * Two unclean sessions in a row with it on: off, and the player is told.
--   * Garry's Mod crashes for plenty of other reasons, and multicore off nearly
--     doubles frame time (2.65 -> 5.30 ms on our map). So an unclean session
--     with it off, after the guard turned it off, means it wasn't the cause:
--     back on. After a second round of two crashes with it on, it stays off.
--   * The player's own toggle (Meowy settings, the Meowy console) is their
--     decision: the guard starts over (no back-on watch, a fresh count).
-- The guard never changes the player's setting (meowy_multicore): its "off"
-- is its own, kept here, so the kill switch and the player's value stay apart.

local M = {}

M.CRASHES_BEFORE_REVERT = 2
M.ROUNDS = 2   -- turned off this many times: it stays off until the player turns it on

-- multicore_guard.txt: "name=value" lines (LF; a CR before it is dropped).
-- running: the session hasn't ended cleanly (0/1). mode: multicore was on in
-- it ("on"/"off"). unclean: unclean sessions in a row with it on. off: the
-- guard has it off. reverted: the guard turned it off and watches whether the
-- game still crashes without it. reverts: times the guard turned it off.
local function int(v)
	local n = tonumber(v)
	if not n or n ~= n or n < 0 then return 0 end
	return math.min(math.floor(n), 1000)
end

function M.decode(text)
	local raw = {}
	for line in string.gmatch((type(text) == "string" and text or "") .. "\n", "([^\n]*)\n") do
		line = string.gsub(line, "\r$", "")
		local k, v = string.match(line, "^([%w_]+)=(.*)$")
		if k and raw[k] == nil then raw[k] = v end
	end
	return {
		running = raw.running == "1",
		mode = raw.mode == "on" and "on" or "off",
		unclean = int(raw.unclean),
		off = raw.off == "1",
		reverted = raw.reverted == "1",
		reverts = int(raw.reverts),
	}
end

function M.encode(s)
	local function b(v) return v and "1" or "0" end
	return table.concat({
		"running=" .. b(s.running),
		"mode=" .. (s.mode == "on" and "on" or "off"),
		"unclean=" .. int(s.unclean),
		"off=" .. b(s.off),
		"reverted=" .. b(s.reverted),
		"reverts=" .. int(s.reverts),
	}, "\n") .. "\n"
end

local function copy(s)
	local t = {}
	for k, v in pairs(s or {}) do t[k] = v end
	return t
end

-- The same state as a clean exit leaves it (what's written at exit).
function M.cleanExit(s)
	local t = copy(s)
	t.running = false
	return t
end

-- mat_queue_mode 0 or 1 is the player's own choice (the engine's default is
-- -1, and 2 is what multicore uses): the pack leaves multicore alone then.
-- Anything it can't read isn't a choice.
function M.queueIsChoice(queue)
	local n = tonumber(queue)
	return n == 0 or n == 1
end

-- At start. s: the state read back (M.decode). wanted: the player's setting
-- is on (read through the policy) and the queue mode isn't their choice.
-- Returns the new state (running, with this session's mode), whether
-- multicore goes on now, and what to tell the player: "off" (turned off after
-- crashes), "back_on" (crashed without it too), or nil.
function M.start(s, wanted)
	s = copy(s)
	local lastCrashed = s.running == true
	local lastMode = s.mode == "on" and "on" or "off"
	local notice

	if lastCrashed and lastMode == "off" and s.reverted and s.off and wanted then
		-- Still crashing without multicore: it wasn't the cause.
		s.off, s.reverted = false, false
		notice = "back_on"
	end

	local on = wanted and not s.off
	if on and lastCrashed and lastMode == "on" then
		s.unclean = int(s.unclean) + 1
	else
		s.unclean = 0
	end

	if on and s.unclean >= M.CRASHES_BEFORE_REVERT then
		s.reverts = int(s.reverts) + 1
		s.off, on, s.unclean = true, false, 0
		-- Only the first time does it watch for crashes without it; the second
		-- time it stays off unless the player turns it on again.
		s.reverted = s.reverts < M.ROUNDS
		notice = "off"
	end

	s.running = true
	s.mode = on and "on" or "off"
	return s, on, notice
end

-- The player turned it on or off themselves: no back-on watch, a fresh count,
-- and the guard's own "off" goes (their setting decides).
function M.playerChose(s)
	s = copy(s)
	s.off, s.reverted, s.unclean = false, false, 0
	return s
end

-- Whether the pack wants multicore on now: the player's setting (policy
-- included), the guard, the queue mode, and a config file that turned it off
-- this start (configOff).
function M.want(settingOn, s, queue, configOff)
	return settingOn == true and not (s and s.off) and not M.queueIsChoice(queue) and not configOff
end

-- What the PC doctor says about it (doctor_logic.lua's cvars.pack):
-- "managed" (the pack turned it on), "setting_off" (the player turned the
-- setting off), "guard_off" (the guard turned it off), or nil (the pack
-- leaves it alone: the queue mode, a config file, the kill switch).
function M.status(applied, settingOn, s, policyOff)
	if applied then return "managed" end
	if policyOff then return nil end
	if not settingOn then return "setting_off" end
	if s and s.off then return "guard_off" end
	return nil
end

-- The prompt for a notice from M.start: title, body.
function M.noticeText(notice)
	if notice == "off" then
		return "Multicore rendering is off",
			"Garry's Mod closed unexpectedly twice in a row with multicore rendering on, so the pack turned it off. "
			.. "To try it again, turn on Multicore rendering in Meowy settings."
	elseif notice == "back_on" then
		return "Multicore rendering is back on",
			"Garry's Mod closed unexpectedly without it too, so multicore rendering wasn't the cause. It's on again: "
			.. "it makes the game much smoother. Turn it off in Meowy settings if you'd rather not."
	end
	return nil
end

return M
