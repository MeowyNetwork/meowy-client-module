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
-- rejoin, unless our server had said it was restarting. Even then the end
-- only counts as the restart once the server is seen going down (it stops
-- answering): a server that keeps answering wasn't restarting, and the player
-- left, was kicked or joined another server during the notice.

local M = {}

M.BACKOFF = { 2, 4, 8, 15, 30 }   -- seconds before each try; the last repeats
M.JITTER = 0.3                    -- each delay +-30%
M.CAP = { timeout = 180, restart = 300, steam = 300 }   -- seconds from the drop, then give up
                                                         -- (steam: once the server answered but Steam was offline)
M.MAX_FAILED_JOINS = 3            -- joins that failed in a row, then stop for good
M.MIN_SILENCE = 10                -- a real drop: the engine heard nothing for this long
M.JOIN_WAIT = 20                  -- a join that has no connection after this long got no answer
M.JOIN_RETRY = 3                  -- seconds before trying again after a failed join
M.UP_WAIT = 15                    -- a restart's server still answering this long after the end: not a restart
M.TIMEOUT_CL = "60"               -- cl_timeout while the player has opted in
M.DEFAULT_CL_TIMEOUT = "30"       -- the engine's own cl_timeout

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
-- Returns "timeout", "restart" or nil (final: no automatic rejoin), and for a
-- restart whether the server must be seen going down before any join (true
-- unless the connection itself had gone silent, which is a drop either way).
function M.classifyEnd(f)
	if type(f) ~= "table" or f.connectionEnded ~= true then return nil end
	local drop = silent(f.timingOut, f.silence)
	if f.restartExpected == true then return "restart", not drop end
	if drop then return "timeout", false end
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

-- The engine's reason for a join that ended before it had a connection
-- ("Disconnected: <reason>"). Right after a drop these clear up by
-- themselves (Steam still re-validating the old session, the network
-- settling): "retry". Anything else is the server turning the player down
-- (ban, whitelist, password): "final". Unknown (nil): nil, decided by the clock.
local PASSING = { "steam validation rejected", "bad server address", "timed out", "server is full",
	"no steam logon", "steam connection lost", "connection failed", "couldn't connect" }
function M.classifyRefusal(reason)
	if type(reason) ~= "string" then return nil end
	local r = reason:lower()
	for _, p in ipairs(PASSING) do
		if r:find(p, 1, true) then return "retry" end
	end
	return "final"
end

-- Seconds before try n (1-based), with rand in [0, 1).
function M.delay(n, rand)
	local base = M.BACKOFF[math.min(math.max(n, 1), #M.BACKOFF)]
	local r = type(rand) == "number" and rand or 0.5
	return base * (1 + (r * 2 - 1) * M.JITTER)
end

-- One rejoin episode, after a drop classified as `mode` at time `now`.
-- needDown (classifyEnd's second value): no join until a probe has gone
-- unanswered. A restart needs it unless needDown is false.
function M.start(mode, now, rand, needDown)
	local s = { mode = mode, startedAt = now, attempt = 0, failedJoins = 0,
		needDown = mode == "restart" and needDown ~= false }
	return M.schedule(s, now, rand)
end

-- The cap for this episode: the mode's, or Steam's longer one once the pack
-- has waited for Steam (it may take minutes to log back on after a drop).
function M.capFor(s)
	local cap = M.CAP[s.mode] or 0
	if s.steamWait then cap = math.max(cap, M.CAP.steam) end
	return cap
end

local function gaveUp(s, now, extra)
	return "gave up after " .. math.floor(now - s.startedAt + 0.5) .. " s" .. (extra or "")
end

-- Stops the episode. kind: "gave_up" (the cap, joins that kept failing: the
-- card offers "Rejoin now"), "final" (a password, a join turned down: no
-- offer), "cancelled" (the player) or "not_restart" (the server never went
-- down: nothing to rejoin, and nothing to say).
function M.stop(s, why, kind)
	if not s.stopped then s.stopped, s.stopKind = why, kind or "gave_up" end
	return s
end

-- Whether the card should offer "Rejoin now" after this episode stopped.
function M.offerRejoin(s)
	return s.stopped ~= nil and s.stopKind == "gave_up"
end

-- Whether the card should say the episode stopped: not for the player's own
-- Cancel, nor for an end that turned out not to be the restart (the player
-- left, was kicked or joined another server: no reconnect was ever shown).
function M.announceStop(s)
	return s.stopped ~= nil and s.stopKind ~= "cancelled" and s.stopKind ~= "not_restart"
end

-- Whether the episode is still waiting to see the server go down (nothing is
-- shown meanwhile: it may not be a restart at all).
function M.confirmingDown(s)
	return s.needDown == true and not s.sawDown and not s.stopped
end

-- Plans the next try, or stops when it would come after the cap.
function M.schedule(s, now, rand)
	if s.stopped then return s end
	s.attempt = s.attempt + 1
	s.nextAt = now + M.delay(s.attempt, rand)
	if s.nextAt - s.startedAt > M.capFor(s) then M.stop(s, gaveUp(s, now)) end
	return s
end

-- The server was asked (A2S) whether it's up. Returns "join", "wait", "steam"
-- or "stop". answered: it replied; locked: the reply says it has a password;
-- steamOffline: Steam itself says it isn't logged on (a join would fail its
-- Steam check): "steam" means wait for Steam (onSteamCheck), not the server.
-- A restart (needDown) joins only after an unanswered probe: until then an
-- answer means "ask again soon", and still answering UP_WAIT seconds after the
-- end means it wasn't the restart: "stop", with nothing offered.
function M.onProbe(s, answered, locked, now, rand, steamOffline)
	if s.stopped then return "stop" end
	s.waitingFor = nil
	if not answered then
		s.sawDown = true
		s.waitingFor = "server"
		M.schedule(s, now, rand)
		return s.stopped and "stop" or "wait"
	end
	if locked then
		M.stop(s, "the server has a password", "final")   -- passwords are never stored or sent
		return "stop"
	end
	if s.needDown and not s.sawDown then
		if now - s.startedAt >= M.UP_WAIT then
			M.stop(s, "the server never went down: not a restart (left, kicked or joined elsewhere)", "not_restart")
			return "stop"
		end
		-- Not a try: the backoff starts once it's down.
		s.waitingFor, s.nextAt = "down", now + M.delay(1, rand)
		return "wait"
	end
	-- Past the cap, but a join already reached the server: MAX_FAILED_JOINS decides.
	if s.failedJoins == 0 and now - s.startedAt > M.capFor(s) then
		M.stop(s, gaveUp(s, now))
		return "stop"
	end
	if steamOffline == true then
		s.waitingFor, s.steamWait = "steam", true
		if now - s.startedAt > M.capFor(s) then
			M.stop(s, gaveUp(s, now, " (Steam didn't reconnect)"))
			return "stop"
		end
		return "steam"
	end
	return "join"
end

-- While waiting for Steam (checked about once a second, no server query, not
-- a try): "probe" when it's back (ask the server once more, then join),
-- "wait", or "stop" past Steam's cap from the drop.
function M.onSteamCheck(s, online, now)
	if s.stopped then return "stop" end
	if online ~= false then   -- true, or unknown: don't hold the join on it
		s.waitingFor = nil
		s.nextAt = now
		return "probe"
	end
	if now - s.startedAt > M.capFor(s) then
		M.stop(s, gaveUp(s, now, " (Steam didn't reconnect)"))
		return "stop"
	end
	return "wait"
end

-- A join didn't get in. kind: classifyJoinEnd's verdict.
function M.onJoinFailed(s, kind, now, rand)
	if s.stopped then return s end
	s.failedJoins = s.failedJoins + 1
	if kind ~= "retry" then return M.stop(s, "the server turned the join down", "final") end
	if s.failedJoins >= M.MAX_FAILED_JOINS then return M.stop(s, "3 joins in a row failed") end
	-- The server answered just before this join, so try again soon, and past
	-- the cap: MAX_FAILED_JOINS bounds these, not the clock. (A join right after
	-- the network came back can fail at once while the server is fine.)
	s.attempt = s.attempt + 1
	s.nextAt = now + M.JOIN_RETRY
	return s
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

-- With staying connected on: whether cl_timeout lost our 60 and should get it
-- again. The engine writes config.cfg only at a clean exit, so a crash (or a
-- config file) puts back the player's previous value or the default. Any other
-- value is one the player chose since: left alone.
function M.reapplyTimeout(current, saved)
	local n = tonumber(current)
	if not n or n == tonumber(M.TIMEOUT_CL) then return false end
	return (M.validTimeout(saved) and n == tonumber(saved)) or n == tonumber(M.DEFAULT_CL_TIMEOUT)
end

-- The main menu's "Continue" ---------------------------------------------------

-- How long ago, for the entry's second line. nil for a time ahead of now (the
-- clock was moved) or not a number.
function M.ago(seconds)
	if type(seconds) ~= "number" or seconds ~= seconds or seconds < 0 then return nil end
	if seconds < 60 then return "just now" end
	if seconds < 3600 then return math.floor(seconds / 60) .. " min ago" end
	if seconds < 86400 then return math.floor(seconds / 3600) .. " h ago" end
	local days = math.floor(seconds / 86400)
	return days == 1 and "1 day ago" or (days .. " days ago")
end

-- Any text as a JavaScript string literal for the menu page: quotes,
-- backslashes, control characters, < > & and U+2028/U+2029 escaped, so a
-- server's name can never end the literal or the script around it.
function M.jsString(s)
	s = string.gsub(tostring(s == nil and "" or s), "[%c\"'\\<>&]", function(c) return string.format("\\x%02x", string.byte(c)) end)
	s = string.gsub(s, "\226\128\168", "\\u2028")
	s = string.gsub(s, "\226\128\169", "\\u2029")
	return '"' .. s .. '"'
end

-- The entry for the last server the player was in the game on, or nil for
-- none: its title, the server (shortened on the page when long) and when ("" if
-- unknown), kept apart so a long name never hides the time. address: the
-- engine's (checked natively); name: the server's own (cleaned natively, may
-- be empty); since, now: unix times.
function M.continueEntry(address, name, since, now)
	if type(address) ~= "string" or address == "" then return nil end
	local label = (type(name) == "string" and string.find(name, "%S")) and name or address
	local ago = type(since) == "number" and type(now) == "number" and M.ago(now - since) or nil
	return { title = "Continue", name = label, ago = ago and (" \194\183 " .. ago) or "" }
end

return M
