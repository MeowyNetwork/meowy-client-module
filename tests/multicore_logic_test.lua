-- Multicore rendering's crash guard (src/lua/multicore_logic.lua), no game:
--   lua tests/multicore_logic_test.lua        (from the repository root; Lua 5.1+)
local here = (arg and arg[0] or ""):match("^(.*)[/\\]") or "."
local M = dofile(here .. "/../src/lua/multicore_logic.lua")

local failures, count = 0, 0
local function check(name, got, want)
	count = count + 1
	if got ~= want then
		failures = failures + 1
		print(("FAIL %s: got %s, want %s"):format(name, tostring(got), tostring(want)))
	end
end

-- The file -------------------------------------------------------------------
local s = M.decode("")
check("decode: nothing, not running", s.running, false)
check("decode: nothing, mode off", s.mode, "off")
check("decode: nothing, no count", s.unclean, 0)
check("decode: nothing, guard not off", s.off, false)
s = M.decode("running=1\r\nmode=on\r\nunclean=1\r\noff=0\r\nreverted=1\r\nreverts=1\r\n")
check("decode: CRLF running", s.running, true)
check("decode: mode", s.mode, "on")
check("decode: unclean", s.unclean, 1)
check("decode: reverted", s.reverted, true)
check("decode: reverts", s.reverts, 1)
s = M.decode("unclean=-4\nreverts=abc\nmode=sideways\nrunning=yes\nunclean=9")
check("decode: a negative count is 0", s.unclean, 0)
check("decode: junk count is 0", s.reverts, 0)
check("decode: unknown mode is off", s.mode, "off")
check("decode: only 1 is running", s.running, false)
s = M.decode("unclean=1\nunclean=7")
check("decode: the first line for a name counts", s.unclean, 1)
local round = { running = true, mode = "on", unclean = 1, off = true, reverted = true, reverts = 1 }
check("encode: round trip", M.encode(M.decode(M.encode(round))), M.encode(round))
check("encode: lines", M.encode(round), "running=1\nmode=on\nunclean=1\noff=1\nreverted=1\nreverts=1\n")
check("cleanExit: running cleared", M.cleanExit(round).running, false)
check("cleanExit: mode kept", M.cleanExit(round).mode, "on")
check("cleanExit: a copy", round.running, true)

-- The queue mode ------------------------------------------------------------
check("queue -1: not a choice", M.queueIsChoice("-1"), false)
check("queue 2: not a choice", M.queueIsChoice("2"), false)
check("queue 0: a choice", M.queueIsChoice("0"), true)
check("queue 1: a choice", M.queueIsChoice("1"), true)
check("queue 0.000: a choice", M.queueIsChoice("0.000"), true)
check("queue unreadable: not a choice", M.queueIsChoice(""), false)
check("queue nil: not a choice", M.queueIsChoice(nil), false)

-- Start ----------------------------------------------------------------------
-- A session: start, then either a clean exit or a crash (the file as left).
local function clean(st) return M.decode(M.encode(M.cleanExit(st))) end
local function crash(st) return M.decode(M.encode(st)) end

-- First start: on, running, mode on, no notice.
local st, on, notice = M.start(M.decode(""), true)
check("first: on", on, true)
check("first: running", st.running, true)
check("first: mode on", st.mode, "on")
check("first: no notice", notice, nil)

-- Clean exits never count.
for _ = 1, 5 do st, on, notice = M.start(clean(st), true) end
check("clean exits: still on", on, true)
check("clean exits: no count", st.unclean, 0)

-- One crash with it on: counted, still on.
st, on, notice = M.start(crash(st), true)
check("one crash: still on", on, true)
check("one crash: counted", st.unclean, 1)
check("one crash: no notice", notice, nil)
-- A clean exit in between resets the count (two in a row needed).
st, on = M.start(clean(st), true)
check("clean between: count reset", st.unclean, 0)
st, on = M.start(crash(st), true)
check("crash again: 1", st.unclean, 1)

-- Second crash in a row: off, the player told, watching for crashes without it.
st, on, notice = M.start(crash(st), true)
check("two crashes: off", on, false)
check("two crashes: notice", notice, "off")
check("two crashes: guard off", st.off, true)
check("two crashes: watching", st.reverted, true)
check("two crashes: first round", st.reverts, 1)
check("two crashes: mode off", st.mode, "off")
check("two crashes: running", st.running, true)
check("two crashes: count reset", st.unclean, 0)
check("want: the guard has it off", M.want(true, st, "-1", false), false)

-- Clean exits with it off: stays off, still watching.
st, on, notice = M.start(clean(st), true)
check("off, clean: still off", on, false)
check("off, clean: no notice", notice, nil)
check("off, clean: still watching", st.reverted, true)

-- A crash with it off: it wasn't the cause, back on.
st, on, notice = M.start(crash(st), true)
check("crash without it: back on", on, true)
check("crash without it: notice", notice, "back_on")
check("crash without it: guard on", st.off, false)
check("crash without it: no longer watching", st.reverted, false)
check("crash without it: mode on", st.mode, "on")
check("crash without it: no count yet", st.unclean, 0)

-- Second round: two crashes with it on, off for good (no watch).
st, on = M.start(crash(st), true)
check("round 2: one crash, on", on, true)
st, on, notice = M.start(crash(st), true)
check("round 2: off", on, false)
check("round 2: notice", notice, "off")
check("round 2: reverts", st.reverts, 2)
check("round 2: not watching", st.reverted, false)
-- Crashes without it now don't bring it back.
st, on, notice = M.start(crash(st), true)
check("round 2: crash without it, stays off", on, false)
check("round 2: no notice", notice, nil)
st, on = M.start(crash(st), true)
check("round 2: still off", on, false)

-- The player's toggle: the guard starts over.
st = M.playerChose(st)
check("player: guard off cleared", st.off, false)
check("player: not watching", st.reverted, false)
check("player: count reset", st.unclean, 0)
st, on = M.start(crash(st), true)
check("player: back on", on, true)
check("player: last crash was with it off, no count", st.unclean, 0)

-- The player chose while it was watching: no back-on (their setting decides).
local w = { running = false, mode = "off", unclean = 0, off = true, reverted = true, reverts = 1 }
w = M.playerChose(w)
st, on, notice = M.start(crash(M.start(w, false)), false)
check("player off, crash without it: stays off", on, false)
check("player off: no notice", notice, nil)

-- Setting off (not wanted): never counted, mode off.
st = { running = true, mode = "on", unclean = 1, off = false, reverted = false, reverts = 0 }
st, on, notice = M.start(st, false)
check("not wanted: off", on, false)
check("not wanted: count reset", st.unclean, 0)
check("not wanted: no notice", notice, nil)
check("not wanted: mode off", st.mode, "off")

-- Watching, but not wanted now (the queue mode, the kill switch): no back-on,
-- the guard's state kept for later.
st = { running = true, mode = "off", unclean = 0, off = true, reverted = true, reverts = 1 }
local kept
kept, on, notice = M.start(st, false)
check("watching, not wanted: off", on, false)
check("watching, not wanted: no notice", notice, nil)
check("watching, not wanted: still watching", kept.reverted, true)
check("watching, not wanted: guard still off", kept.off, true)

-- Older guard files: a crash with mode "on" counts from the file's count.
st, on = M.start(M.decode("running=1\nmode=on\nunclean=1\n"), true)
check("file: second crash from disk, off", on, false)

-- Not mutated: start and playerChose return copies.
local orig = { running = true, mode = "on", unclean = 1, off = false, reverted = false, reverts = 0 }
M.start(orig, true)
M.playerChose(orig)
check("start: input untouched", orig.unclean, 1)
check("start: input running untouched", orig.running, true)

-- What the pack wants -------------------------------------------------------
local g = { off = false }
check("want: on", M.want(true, g, "-1", false), true)
check("want: queue 2", M.want(true, g, "2", false), true)
check("want: setting off", M.want(false, g, "-1", false), false)
check("want: queue 0", M.want(true, g, "0", false), false)
check("want: queue 1", M.want(true, g, "1", false), false)
check("want: a config file turned it off", M.want(true, g, "-1", true), false)
check("want: no guard state", M.want(true, nil, "-1", false), true)

-- The PC doctor's status ----------------------------------------------------
check("status: applied", M.status(true, true, g, false), "managed")
check("status: setting off", M.status(false, false, g, false), "setting_off")
check("status: guard off", M.status(false, true, { off = true }, false), "guard_off")
check("status: policy off", M.status(false, false, g, true), nil)
check("status: left alone (queue mode)", M.status(false, true, g, false), nil)

-- Notices -------------------------------------------------------------------
local title, body = M.noticeText("off")
check("notice off: title", title, "Multicore rendering is off")
check("notice off: says how to try again", body:find("Meowy settings", 1, true) ~= nil, true)
title = M.noticeText("back_on")
check("notice back on: title", title, "Multicore rendering is back on")
check("notice none", M.noticeText(nil), nil)

print(("%d checks, %d failed"):format(count, failures))
os.exit(failures == 0 and 0 or 1)
