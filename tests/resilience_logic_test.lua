-- Connection resilience's decision rules (src/lua/resilience_logic.lua), no game:
--   lua tests/resilience_logic_test.lua        (from the repository root; Lua 5.1+)
local here = (arg and arg[0] or ""):match("^(.*)[/\\]") or "."
local M = dofile(here .. "/../src/lua/resilience_logic.lua")

local failures, count = 0, 0
local function check(name, got, want)
	count = count + 1
	if got ~= want then
		failures = failures + 1
		print(("FAIL %s: got %s, want %s"):format(name, tostring(got), tostring(want)))
	end
end
local function near(a, b) return math.abs(a - b) < 1e-9 end

-- Which drops start a rejoin -------------------------------------------------
check("engine timeout, long silence", M.classifyDrop({ "Server connection timed out." }, 60), "timeout")
check("engine timeout, newline around it", M.classifyDrop({ "\nServer connection timed out.\n" }, 31), "timeout")
check("engine timeout, silence unknown", M.classifyDrop({ "Server connection timed out." }, nil), "timeout")
check("timeout line but packets were flowing", M.classifyDrop({ "Server connection timed out." }, 0.1), nil)
check("Disconnect: timed out, long silence", M.classifyDrop({ "Disconnect: Server connection timed out." }, 45), "timeout")
check("kick that says timed out (silence ~0)", M.classifyDrop({ "Disconnect: Server connection timed out." }, 0.05), nil)
check("kick that says timed out (silence unknown)", M.classifyDrop({ "Disconnect: Server connection timed out." }, nil), nil)
check("kick text 'timed out' differently", M.classifyDrop({ "Disconnect: timed out." }, 60), nil)
check("forged bare line, then a kick", M.classifyDrop({ "Server connection timed out.", "Disconnect: Kicked by Console : timed out." }, 0.1), nil)
check("forged bare line, then a kick (silence long)", M.classifyDrop({ "Server connection timed out.", "Disconnect: Kicked by Console : bye." }, 60), nil)
check("line containing the words isn't the line", M.classifyDrop({ "[chat] Bob: Server connection timed out." }, 60), nil)
check("shutdown", M.classifyDrop({ "Disconnect: Server shutting down." }, 0), "shutdown")
check("shutdown, no period", M.classifyDrop({ "Disconnect: Server shutting down" }, nil), "shutdown")
check("ban", M.classifyDrop({ "Disconnect: You have been banned from this server." }, 0), nil)
check("bad password", M.classifyDrop({ "Disconnect: Bad password." }, 0), nil)
check("player's own disconnect", M.classifyDrop({ "Disconnect: Disconnect by user." }, 0), nil)
check("nothing printed", M.classifyDrop({}, 60), nil)
check("nil lines", M.classifyDrop(nil, 60), nil)
check("last Disconnect line decides", M.classifyDrop({ "Disconnect: Server shutting down.", "Disconnect: Kicked." }, 0), nil)

-- Backoff -------------------------------------------------------------------
local bases = { 2, 4, 8, 15, 30, 30, 30 }
for n, b in ipairs(bases) do
	check("delay " .. n .. " middle", near(M.delay(n, 0.5), b), true)
	check("delay " .. n .. " low", near(M.delay(n, 0), b * 0.7), true)
	check("delay " .. n .. " high bound", M.delay(n, 0.999999) <= b * 1.3 and M.delay(n, 0.999999) > b * 1.29, true)
end

-- A timeout episode: probes fail, server answers, join ----------------------
local s = M.start("timeout", 0, 0.5)
check("first try at 2 s", near(s.nextAt, 2), true)
check("not stopped", s.stopped, nil)
check("probe fails -> wait", M.onProbe(s, false, false, 2, 0.5), "wait")
check("second try 4 s later", near(s.nextAt, 6), true)
check("probe answers -> join", M.onProbe(s, true, false, 6, 0.5), "join")

-- Hard cap: 3 minutes for a timeout, whatever the probes say
s = M.start("timeout", 0, 0.5)
local t, steps = 0, 0
while not s.stopped and steps < 100 do
	t = s.nextAt
	M.onProbe(s, false, false, t, 0.999)
	steps = steps + 1
end
check("timeout episode stops", s.stopped ~= nil, true)
check("never past 3 min", t <= 180, true)
check("last planned try was past the cap", s.nextAt > 180, true)

-- Restart: 5 minutes of polling allowed, join once it answers
s = M.start("shutdown", 0, 0.5)
t = 0
for _ = 1, 9 do t = s.nextAt M.onProbe(s, false, false, t, 0.5) end
check("still polling at ~3 min", s.stopped == nil and t > 150, true)
check("restart answers -> join", M.onProbe(s, true, false, t + 1, 0.5), "join")
s = M.start("shutdown", 0, 0.999)
t = 0
while not s.stopped do t = s.nextAt M.onProbe(s, false, false, t, 0.999) end
check("restart polling never past 5 min", t <= 300, true)

-- "Server shutting down" from a server that never went down: a kick in disguise
s = M.start("shutdown", 0, 0.5)
check("shutdown but answers at once -> stop", M.onProbe(s, true, false, 2, 0.5), "stop")
check("reason", s.stopped, "the server didn't restart")

-- Password: never sent
s = M.start("timeout", 0, 0.5)
check("password -> stop", M.onProbe(s, true, true, 2, 0.5), "stop")

-- Joins that fail ---------------------------------------------------------
check("join failure: timeout", M.classifyJoinFailure({ "Server connection timed out." }), "retry")
check("join failure: no answer", M.classifyJoinFailure({ "Connection failed after 4 retries." }), "retry")
check("join failure: server restarting", M.classifyJoinFailure({ "Disconnect: Server shutting down." }), "retry")
check("join failure: banned", M.classifyJoinFailure({ "Disconnect: You are banned." }), "final")
check("join failure: password", M.classifyJoinFailure({ "Disconnect: Bad password." }), "final")
check("join failure: kick saying timed out", M.classifyJoinFailure({ "Disconnect: Kicked: timed out" }), "final")
check("join failure: nothing printed", M.classifyJoinFailure({}), "final")

s = M.start("timeout", 0, 0.5)
M.onJoinFailed(s, "retry", 3, 0.5)
check("1 failed join: continues", s.stopped, nil)
M.onJoinFailed(s, "retry", 10, 0.5)
check("2 failed joins: continues", s.stopped, nil)
M.onJoinFailed(s, "retry", 20, 0.5)
check("3 failed joins: stops", s.stopped, "3 joins in a row failed")
check("stopped episode never joins", M.onProbe(s, true, false, 30, 0.5), "stop")

s = M.start("timeout", 0, 0.5)
M.onJoinFailed(s, "final", 3, 0.5)
check("final failure stops at once", s.stopped, "the server turned the join down")

s = M.start("timeout", 0, 0.5)
M.stop(s, "cancelled")
check("cancel sticks", M.onProbe(s, true, false, 5, 0.5), "stop")
check("cancel reason", s.stopped, "cancelled")

-- Settings ------------------------------------------------------------------
local st = M.parseSettings("asked=1\r\nenabled=1\r\nsaved_cl_timeout=30\r\n")
check("parse asked", st.asked, true)
check("parse enabled", st.enabled, true)
check("parse saved", st.saved_cl_timeout, "30")
st = M.parseSettings("asked=1\nenabled=0\nsaved_cl_timeout=30; quit\n")
check("junk timeout dropped", st.saved_cl_timeout, nil)
check("parse off", st.enabled, false)
check("empty file", M.parseSettings(nil).asked, false)
check("round trip", M.formatSettings({ asked = true, enabled = true, saved_cl_timeout = "45" }), "asked=1\r\nenabled=1\r\nsaved_cl_timeout=45\r\n")
check("restore when still ours", M.restoreTimeout("60", "30"), "30")
check("player changed it since: leave it", M.restoreTimeout("120", "30"), nil)
check("nothing saved", M.restoreTimeout("60", nil), nil)
check("engine float form", M.restoreTimeout("60.000000", "30.000000"), "30.000000")

print(("%d checks, %d failed"):format(count, failures))
os.exit(failures == 0 and 0 or 1)
