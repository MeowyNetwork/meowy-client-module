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

print(("%d checks, %d failed"):format(count, failures))
if failures > 0 then os.exit(1) end
