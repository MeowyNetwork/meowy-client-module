-- Connection resilience: the decisions, as pure functions (no GMod API), so
-- they can be tested with a plain Lua interpreter (tests/resilience_logic_test.lua).
-- Lua 5.1 compatible: Garry's Mod runs LuaJIT.
--
-- The menu glue (resilience.lua) supplies the facts, all from the engine:
--   * the server we were on: the engine's own net-channel address (server_gate),
--     recorded while connected, never a string a server's Lua could set;
--   * how long the engine had heard nothing from the server just before the
--     drop (INetChannelInfo::GetTimeSinceLastReceived), nil if unknown;
--   * the engine's console lines around the drop (tier0's logging listener).
-- A drop may start an automatic rejoin only when its reason is exactly the
-- engine's own timeout or the server shutting down. Everything else (a kick, a
-- ban, a wrong password, the player's own disconnect, anything unknown) is final.

local M = {}

M.BACKOFF = { 2, 4, 8, 15, 30 }   -- seconds before each try; the last repeats
M.JITTER = 0.3                    -- each delay +-30%
M.CAP = { timeout = 180, shutdown = 300 }   -- seconds from the drop, then give up
M.MAX_FAILED_JOINS = 3            -- joins that failed in a row, then stop for good
M.MIN_SILENCE = 10                -- a real timeout: the engine heard nothing for this long
M.TIMEOUT_CL = "60"               -- cl_timeout while the player has opted in

local function trim(s)
	return (string.gsub(s, "^%s+", ""):gsub("%s+$", ""))
end

-- The engine's own lines, exactly (trailing period optional).
local TIMEOUT_BARE = { ["Server connection timed out"] = true }
local DISCONNECT_TIMEOUT = { ["Server connection timed out"] = true }
local DISCONNECT_SHUTDOWN = { ["Server shutting down"] = true }

local function stripDot(s)
	return (string.gsub(s, "%.$", ""))
end

-- "Disconnect: <reason>" -> reason (without the trailing period), else nil.
local function disconnectReason(line)
	local r = string.match(line, "^Disconnect: (.*)$")
	return r and stripDot(trim(r)) or nil
end

-- lines: the engine console lines around the drop, oldest first.
-- silence: seconds the engine had heard nothing from the server just before
-- the drop, or nil when it couldn't be read.
-- Returns "timeout", "shutdown" or nil (final: no automatic rejoin).
function M.classifyDrop(lines, silence)
	local bareTimeout, decisive = false, nil
	for _, raw in ipairs(lines or {}) do
		if type(raw) == "string" then
			local line = trim(raw)
			local reason = disconnectReason(line)
			if reason then
				decisive = reason          -- the last "Disconnect:" line decides
			elseif TIMEOUT_BARE[stripDot(line)] then
				bareTimeout = true
			end
		end
	end
	local quiet = type(silence) == "number" and silence >= M.MIN_SILENCE
	if decisive then
		if DISCONNECT_SHUTDOWN[decisive] then return "shutdown" end
		-- The same words as a kick message are only believed when the engine
		-- itself had heard nothing from the server (a kick arrives as a packet).
		if DISCONNECT_TIMEOUT[decisive] and quiet then return "timeout" end
		return nil
	end
	if bareTimeout and (silence == nil or quiet) then return "timeout" end
	return nil
end

-- Why a rejoin attempt didn't get in: "retry" only for no answer / timeouts;
-- anything else (kicked, banned, password, full, the player cancelling,
-- nothing printed at all) is "final".
local RETRY_DISCONNECT = {
	["Server connection timed out"] = true,
	["Server shutting down"] = true,
}
function M.classifyJoinFailure(lines)
	local verdict = "final"
	for _, raw in ipairs(lines or {}) do
		if type(raw) == "string" then
			local line = trim(raw)
			local reason = disconnectReason(line)
			if reason then
				verdict = RETRY_DISCONNECT[reason] and "retry" or "final"
			elseif TIMEOUT_BARE[stripDot(line)] or string.match(line, "^Connection failed after %d+ retries%.?$") then
				verdict = "retry"
			end
		end
	end
	return verdict
end

-- Seconds before try n (1-based), with rand in [0, 1).
function M.delay(n, rand)
	local base = M.BACKOFF[math.min(math.max(n, 1), #M.BACKOFF)]
	local r = type(rand) == "number" and rand or 0.5
	return base * (1 + (r * 2 - 1) * M.JITTER)
end

-- One rejoin episode, after a drop classified as `mode` at time `now`.
function M.start(mode, now, rand)
	local s = { mode = mode, startedAt = now, attempt = 0, failedJoins = 0, sawDown = false }
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
		s.sawDown = true
		M.schedule(s, now, rand)
		return s.stopped and "stop" or "wait"
	end
	if locked then
		M.stop(s, "the server has a password")   -- passwords are never stored or sent
		return "stop"
	end
	-- "Server shutting down" is also what a kick can say. A real shutdown means
	-- the server stopped answering for a while; one that answers straight away
	-- didn't restart, so this was a kick in disguise.
	if s.mode == "shutdown" and not s.sawDown then
		M.stop(s, "the server didn't restart")
		return "stop"
	end
	if now - s.startedAt > (M.CAP[s.mode] or 0) then
		M.stop(s, "gave up after " .. math.floor(now - s.startedAt + 0.5) .. " s")
		return "stop"
	end
	return "join"
end

-- A join didn't get in. kind: classifyJoinFailure's verdict.
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
