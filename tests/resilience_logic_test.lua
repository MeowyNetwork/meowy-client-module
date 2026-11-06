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

-- How the connection ended -----------------------------------------------------
local function ended(timingOut, silence, restartExpected, connectionEnded)
	if connectionEnded == nil then connectionEnded = true end
	return M.classifyEnd({ connectionEnded = connectionEnded, timingOut = timingOut, silence = silence, restartExpected = restartExpected })
end
check("drop: timing out, 60 s silent", ended(true, 60, false), "timeout")
check("drop: timing out, exactly 10 s", ended(true, 10, false), "timeout")
check("drop: timing out but only 9.9 s", ended(true, 9.9, false), nil)
check("silence without IsTimingOut", ended(false, 60, false), nil)
check("kick/ban/quit: packets flowing", ended(false, 0.02, false), nil)
check("timing out flag but packets flowing", ended(true, 0.5, false), nil)
check("no sample (couldn't read)", ended(nil, nil, false), nil)
check("only silence read", ended(nil, 60, false), nil)
check("only flag read", ended(true, nil, false), nil)
check("planned restart, packets flowing", ended(false, 0.02, true), "restart")
check("planned restart, no sample", ended(nil, nil, true), "restart")
check("planned restart wins over a timeout", ended(true, 60, true), "restart")
check("connection didn't end (went elsewhere)", ended(true, 60, false, false), nil)
check("restart expected but went elsewhere", ended(false, 0, true, false), nil)
check("nothing", M.classifyEnd(nil), nil)
check("silence as a string isn't a number", ended(true, "60", false), nil)

-- Joins that don't get in --------------------------------------------------------
check("join: connected, then server went silent", M.classifyJoinEnd({ reachedOurs = true, timingOut = true, silence = 30 }), "retry")
check("join: connected, then kicked/banned", M.classifyJoinEnd({ reachedOurs = true, timingOut = false, silence = 0.1 }), "final")
check("join: connected, no sample", M.classifyJoinEnd({ reachedOurs = true }), "final")
check("join: turned away at once", M.classifyJoinEnd({ reachedOurs = false, waited = 1.5 }), "final")
check("join: no answer for 20 s", M.classifyJoinEnd({ reachedOurs = false, waited = 20 }), "retry")
check("join: nothing", M.classifyJoinEnd(nil), "final")

-- Backoff -------------------------------------------------------------------------
local bases = { 2, 4, 8, 15, 30, 30, 30 }
for n, b in ipairs(bases) do
	check("delay " .. n .. " middle", near(M.delay(n, 0.5), b), true)
	check("delay " .. n .. " low", near(M.delay(n, 0), b * 0.7), true)
	check("delay " .. n .. " high bound", M.delay(n, 0.999999) <= b * 1.3 and M.delay(n, 0.999999) > b * 1.29, true)
end

-- A timeout episode: probes fail, server answers, join ------------------------------
local s = M.start("timeout", 0, 0.5)
check("first try at 2 s", near(s.nextAt, 2), true)
check("not stopped", s.stopped, nil)
check("probe fails -> wait", M.onProbe(s, false, false, 2, 0.5), "wait")
check("second try 4 s later", near(s.nextAt, 6), true)
check("probe answers -> join", M.onProbe(s, true, false, 6, 0.5), "join")

-- Hard cap: 3 minutes for a timeout
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

-- Restart: up to 5 minutes of polling, join once it answers
s = M.start("restart", 0, 0.5)
t = 0
for _ = 1, 9 do t = s.nextAt M.onProbe(s, false, false, t, 0.5) end
check("still polling at ~3 min", s.stopped == nil and t > 150, true)
check("restart answers -> join", M.onProbe(s, true, false, t + 1, 0.5), "join")
s = M.start("restart", 0, 0.999)
t = 0
while not s.stopped do t = s.nextAt M.onProbe(s, false, false, t, 0.999) end
check("restart polling never past 5 min", t <= 300, true)
s = M.start("restart", 0, 0.5)
check("restart that answers at once -> join", M.onProbe(s, true, false, 2, 0.5), "join")

-- Password: never sent
s = M.start("timeout", 0, 0.5)
check("password -> stop", M.onProbe(s, true, true, 2, 0.5), "stop")

-- Failed joins: 3 in a row, then stop for good
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

-- Settings --------------------------------------------------------------------------
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
