-- Connection resilience: the decisions, as pure functions (no GMod API), so
-- they can be tested with a plain Lua interpreter (tests/resilience_logic_test.lua).
-- Lua 5.1 compatible: Garry's Mod runs LuaJIT.
--
-- The menu glue (resilience.lua) supplies only native facts and our own
-- server's word, never text:
--   * the server we were on: the engine's own net-channel address (server_gate),
--     recorded while connected, never a string a server's Lua could set;
--   * the engine's last sample of that connection before it went away, taken
--     every frame: INetChannelInfo::IsTimingOut and GetTimeSinceLastReceived
--     (nil when they couldn't be read);
--   * whether our server had said it was about to restart (meowy.ExpectRestart,
--     callable only while connected to our server).
-- The server went silent before the connection ended: a drop, rejoin. Packets
-- were still flowing when it ended: a kick, ban, quit or map change, never
-- rejoin, unless our server had said it was restarting.

local M = {}

M.BACKOFF = { 2, 4, 8, 15, 30 }   -- seconds before each try; the last repeats
M.JITTER = 0.3                    -- each delay +-30%
M.CAP = { timeout = 180, restart = 300 }   -- seconds from the drop, then give up
M.MAX_FAILED_JOINS = 3            -- joins that failed in a row, then stop for good
M.MIN_SILENCE = 10                -- a real drop: the engine heard nothing for this long
M.JOIN_WAIT = 20                  -- a join that has no connection after this long got no answer
M.TIMEOUT_CL = "60"               -- cl_timeout while the player has opted in

-- The engine had given up hearing from the server: timing out, and silent long enough.
local function silent(timingOut, silence)
	return timingOut == true and type(silence) == "number" and silence >= M.MIN_SILENCE
end

-- How the connection to our server ended. f = {
--   connectionEnded = true,  -- the engine's connection to our server went away
--   timingOut = bool|nil,    -- last sample before that: IsTimingOut
--   silence = number|nil,    -- last sample before that: seconds since a packet
--   restartExpected = bool,  -- our server had said it was restarting, still in force
-- }
-- Returns "timeout", "restart" or nil (final: no automatic rejoin).
function M.classifyEnd(f)
	if type(f) ~= "table" or f.connectionEnded ~= true then return nil end
	if f.restartExpected == true then return "restart" end
	if silent(f.timingOut, f.silence) then return "timeout" end
	return nil
end

-- Why a rejoin attempt didn't get in. f = {
--   reachedOurs = bool,     -- the engine's connection to our server came up
--   timingOut, silence,     -- its last sample, if it did
--   waited = seconds,       -- how long the join had been going without a connection
-- }
-- "retry" when the server went silent, or never answered for JOIN_WAIT seconds;
-- "final" otherwise (turned away, kicked on the way in, the player cancelled).
function M.classifyJoinEnd(f)
	if type(f) ~= "table" then return "final" end
	if f.reachedOurs then
		return silent(f.timingOut, f.silence) and "retry" or "final"
	end
	if type(f.waited) == "number" and f.waited >= M.JOIN_WAIT then return "retry" end
	return "final"
end

-- Seconds before try n (1-based), with rand in [0, 1).
function M.delay(n, rand)
	local base = M.BACKOFF[math.min(math.max(n, 1), #M.BACKOFF)]
	local r = type(rand) == "number" and rand or 0.5
	return base * (1 + (r * 2 - 1) * M.JITTER)
end

-- One rejoin episode, after a drop classified as `mode` at time `now`.
function M.start(mode, now, rand)
	local s = { mode = mode, startedAt = now, attempt = 0, failedJoins = 0 }
	return M.schedule(s, now, rand)
end

-- Plans the next try, or stops when it would come after the cap.
function M.schedule(s, now, rand)
	if s.stopped then return s end
	s.attempt = s.attempt + 1
	s.nextAt = now + M.delay(s.attempt, rand)
	if s.nextAt - s.startedAt > (M.CAP[s.mode] or 0) then
		s.stopped = "gave up after " .. math.floor(now - s.startedAt + 0.5) .. " s"
	end
	return s
end

function M.stop(s, why)
	if not s.stopped then s.stopped = why end
	return s
end

-- The server was asked (A2S) whether it's up. Returns "join", "wait" or "stop".
-- answered: it replied; locked: the reply says it has a password.
function M.onProbe(s, answered, locked, now, rand)
	if s.stopped then return "stop" end
	if not answered then
		M.schedule(s, now, rand)
		return s.stopped and "stop" or "wait"
	end
	if locked then
		M.stop(s, "the server has a password")   -- passwords are never stored or sent
		return "stop"
	end
	if now - s.startedAt > (M.CAP[s.mode] or 0) then
		M.stop(s, "gave up after " .. math.floor(now - s.startedAt + 0.5) .. " s")
		return "stop"
	end
	return "join"
end

-- A join didn't get in. kind: classifyJoinEnd's verdict.
function M.onJoinFailed(s, kind, now, rand)
	if s.stopped then return s end
	s.failedJoins = s.failedJoins + 1
	if kind ~= "retry" then return M.stop(s, "the server turned the join down") end
	if s.failedJoins >= M.MAX_FAILED_JOINS then return M.stop(s, "3 joins in a row failed") end
	return M.schedule(s, now, rand)
end

-- Settings file (%LOCALAPPDATA%\MeowyClientPack\resilience.txt): key=value lines.
function M.parseSettings(text)
	local out = { asked = false, enabled = false, saved_cl_timeout = nil }
	if type(text) ~= "string" then return out end
	for line in string.gmatch(text, "[^\r\n]+") do
		local k, v = string.match(line, "^%s*([%w_]+)%s*=%s*(.-)%s*$")
		if k == "asked" then out.asked = v == "1"
		elseif k == "enabled" then out.enabled = v == "1"
		elseif k == "saved_cl_timeout" and M.validTimeout(v) then out.saved_cl_timeout = v end
	end
	return out
end

function M.formatSettings(t)
	local lines = {
		"asked=" .. (t.asked and "1" or "0"),
		"enabled=" .. (t.enabled and "1" or "0"),
	}
	if M.validTimeout(t.saved_cl_timeout) then lines[#lines + 1] = "saved_cl_timeout=" .. t.saved_cl_timeout end
	return table.concat(lines, "\r\n") .. "\r\n"
end

-- A cl_timeout value worth restoring: a plain number of seconds.
function M.validTimeout(v)
	return type(v) == "string" and string.match(v, "^%d+%.?%d*$") ~= nil and #v <= 12
end

-- What to put cl_timeout back to when the player turns it off: their own
-- value, but only if it's still the one we set (they may have changed it since).
function M.restoreTimeout(current, saved)
	if M.validTimeout(saved) and tonumber(current) == tonumber(M.TIMEOUT_CL) then return saved end
	return nil
end

return M
