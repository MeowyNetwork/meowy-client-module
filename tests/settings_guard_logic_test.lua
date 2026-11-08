-- The settings guard's rules (src/lua/settings_guard_logic.lua), no game:
--   lua tests/settings_guard_logic_test.lua        (from the repository root; Lua 5.1+)
local here = (arg and arg[0] or ""):match("^(.*)[/\\]") or "."
local Guard = dofile(here .. "/../src/lua/settings_guard_logic.lua")

local failures, count = 0, 0
local function check(name, got, want)
	count = count + 1
	if got ~= want then
		failures = failures + 1
		print(("FAIL %s: got %s, want %s"):format(name, tostring(got), tostring(want)))
	end
end

-- The list: no duplicates, labels everywhere, none of what the pack changes itself.
local seen, dup, unlabeled = {}, nil, nil
for _, w in ipairs(Guard.WATCH) do
	if seen[w[1]] then dup = w[1] end
	seen[w[1]] = true
	if type(w[2]) ~= "string" or w[2] == "" then unlabeled = w[1] end
end
check("watch list: no setting twice", dup, nil)
check("watch list: every setting labelled", unlabeled, nil)
for _, own in ipairs({ "cl_timeout", "fps_max_nofocus", "fps_max_menu", "voice_scale", "volume", "sensitivity", "fov_desired", "fps_max" }) do
	check("watch list: leaves out " .. own, seen[own], nil)
end
for name in pairs(Guard.GMOD_CHECKS) do check("watch list: has Garry's Mod's check " .. name, seen[name], true) end
local ascii = true
for _, w in ipairs(Guard.WATCH) do ascii = ascii and not w[2]:find("[\128-\255]") end
check("labels are plain ASCII (the menu's fonts)", ascii, true)

-- Snapshot and diff over a fake convar table.
local cvars = { mat_specular = "1", r_3dsky = "1", cl_interp = "0.100000", gmod_mcore_test = "0", rate = "786432" }
local function get(n) return cvars[n] or "" end
local snap = Guard.snapshot(get)
check("snapshot: only settings the game has", snap.mat_bumpmap, nil)
check("snapshot: values as text", snap.cl_interp, "0.100000")
check("nothing changed: no changes", #Guard.diff(snap, get), 0)
cvars.cl_interp = "0.1"
check("same number written differently: not a change", #Guard.diff(snap, get), 0)
cvars.mat_specular, cvars.r_3dsky, cvars.gmod_mcore_test, cvars.cl_interp = "0", "0", "1", "0.0152"
local changes = Guard.diff(snap, get)
check("three settings and interpolation changed", #changes, 4)
check("in the list's order: Garry's Mod's checks first", changes[1].name, "mat_specular")
check("was and now kept", changes[1].was .. ">" .. changes[1].now, "1>0")
cvars.rate = ""
check("a setting gone isn't a change", #Guard.diff(snap, get), 4)

-- How values and the list read.
check("whole numbers without decimals", Guard.show("1.000000"), "1")
check("decimals trimmed", Guard.show("0.015200"), "0.0152")
check("text left as it is", Guard.show("abc"), "abc")
check("a change reads as label, convar, was, now", Guard.describe({ changes[1] }), "Specular reflections (mat_specular): 1, now 0")
local many = {}
for i = 1, 11 do many[i] = { name = "x" .. i, label = "X" .. i, was = "0", now = "1" } end
local text = Guard.describe(many, 8)
check("at most 8 lines, then how many more", select(2, text:gsub("\n", "\n")) + 1, 9)
check("the last line says how many more", text:match("[^\n]*$"), "and 3 more")

-- Servers always put back.
local list = Guard.parseList("1.2.3.4:27015 bad;address 5.6.7.8:27016")
check("addresses parsed, junk left out", table.concat(list, ","), "1.2.3.4:27015,5.6.7.8:27016")
check("has", Guard.listHas(list, "5.6.7.8:27016"), true)
list = Guard.listAdd(list, "1.2.3.4:27015", 20)
check("added again: moved to the end, once", table.concat(list, ","), "5.6.7.8:27016,1.2.3.4:27015")
for i = 1, 25 do list = Guard.listAdd(list, "10.0.0." .. i .. ":27015", 20) end
check("at most 20 kept", #list, 20)
check("the oldest dropped", Guard.listHas(list, "5.6.7.8:27016"), false)
list = Guard.listRemove(list, "10.0.0.25:27015")
check("removed", #list == 19 and not Guard.listHas(list, "10.0.0.25:27015"), true)
check("removing one that isn't there changes nothing", #Guard.listRemove(list, "9.9.9.9:1"), 19)

-- A server's part of a session, and what's done with it.
cvars = { mat_specular = "1", r_3dsky = "1", cl_interp = "0.1" }
local cur = { snap = Guard.snapshot(get), address = "1.2.3.4:27015", name = "Foo RP" }
cvars.r_3dsky = "0"
local part = Guard.closePart(cur, get)
check("part: its address and changes", part.address .. " " .. #part.changes .. " " .. part.changes[1].name, "1.2.3.4:27015 1 r_3dsky")
local function ours(a) return a == "138.201.62.52:27015" end
check("verdict: ask", Guard.verdict(part, ours, {}), "ask")
check("verdict: always", Guard.verdict(part, ours, { "1.2.3.4:27015" }), "putback")
check("verdict: ours is kept", Guard.verdict({ address = "138.201.62.52:27015", changes = part.changes }, ours, {}), "keep")
check("verdict: nothing changed", Guard.verdict({ address = "1.2.3.4:27015", changes = {} }, ours, {}), "none")
check("verdict: no address still asks", Guard.verdict({ changes = part.changes }, ours, { "1.2.3.4:27015" }), "ask")
check("still changed: in place", #Guard.stillChanged(part.changes, get), 1)
cvars.r_3dsky = "0.5"
check("still changed: a later server changed it again", #Guard.stillChanged(part.changes, get), 0)
check("only: kept by name", #Guard.only({ { name = "a" }, { name = "b" } }, function(n) return n == "b" end), 1)
cvars.r_3dsky = "0"

-- What's done with a part waiting: on what's still in place; a part from the
-- last run is asked about even for a server always put back.
check("decide: ask", (Guard.decide(part, get, ours, {})), "ask")
check("decide: always, put back", (Guard.decide(part, get, ours, { "1.2.3.4:27015" })), "putback")
local closed = { address = part.address, changes = part.changes, closed = true }
check("decide: from the last run, asked even when always", (Guard.decide(closed, get, ours, { "1.2.3.4:27015" })), "ask")
cvars.r_3dsky = "0.5"
local v, still = Guard.decide(part, get, ours, {})
check("decide: changed again since, nothing to do", v .. #still, "none0")
cvars.r_3dsky = "0"
check("fresh: today", Guard.fresh(1000, 1000 + 3600), true)
check("fresh: a week old", Guard.fresh(1000, 1000 + Guard.MAX_AGE + 1), false)
check("fresh: no time", Guard.fresh(nil, 1000), false)
check("fresh: from the future", Guard.fresh(1000 + 86400, 1000), false)

-- The session on disk: what's needed to ask at the next start, and nothing else.
local s = { parts = { { address = "5.6.7.8:27015", name = "Bar\nRP", at = 1700000000, changes = { { name = "mat_picmip", label = "x", was = "0", now = "2" } } } },
	cur = { address = "1.2.3.4:27015", name = "Foo RP", at = 1700000100, snap = { mat_specular = "1", r_3dsky = "1", not_watched = "1", cl_interp = "0 1" } } }
local text = Guard.encodeSession(s)
local back = Guard.decodeSession(text)
check("session: the part ended earlier", back.parts[1].address .. "|" .. back.parts[1].name, "5.6.7.8:27015|BarRP")
check("session: when each ended or started", back.parts[1].at .. " " .. back.cur.at, "1700000000 1700000100")
check("session: its change, labelled", back.parts[1].changes[1].name .. " " .. back.parts[1].changes[1].was .. ">" .. back.parts[1].changes[1].now .. " " .. back.parts[1].changes[1].label, "mat_picmip 0>2 Texture detail")
check("session: the server it was on", back.cur.address .. "|" .. back.cur.name, "1.2.3.4:27015|Foo RP")
check("session: its snapshot", back.cur.snap.mat_specular .. back.cur.snap.r_3dsky, "11")
check("session: only watched settings kept", back.cur.snap.not_watched, nil)
check("session: a value with a space isn't kept", back.cur.snap.cl_interp, nil)
check("session: none at all", Guard.decodeSession(""), nil)
check("session: not one", Guard.decodeSession("garbage"), nil)
local noAddress = Guard.decodeSession(Guard.encodeSession({ parts = {}, cur = { snap = { r_3dsky = "1" } } }))
check("session: no address yet", noAddress.cur.address == nil and noAddress.cur.snap.r_3dsky == "1", true)
local evil = Guard.decodeSession("v 1\ncur 1.2.3.4:27015;quit 5 x\nsnap r_3dsky 1\nsnap sv_cheats 1\nchange r_3dsky 1 0\n")
check("session: a damaged address isn't used", evil.cur.address, nil)
check("session: an unwatched setting isn't read", evil.cur.snap.sv_cheats, nil)
check("session: a change outside a part is ignored", #evil.parts, 0)
local broken = Guard.decodeSession("v 1\npart 1.2.3.4:27015 5 A\nchange mat_picmip 0 2\npart garbage\nchange r_3dsky 1 0\ncur nonsense\nsnap r_3dsky 1\n")
check("session: lines under a line that didn't read go nowhere", #broken.parts .. " " .. #broken.parts[1].changes .. " " .. tostring(broken.cur), "1 1 nil")

print(("%d checks, %d failed"):format(count, failures))
if failures > 0 then os.exit(1) end
