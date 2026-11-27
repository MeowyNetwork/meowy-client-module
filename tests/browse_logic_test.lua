-- Find servers' index rules (src/lua/browse_logic.lua), no game:
--   lua tests/browse_logic_test.lua        (from the repository root; Lua 5.1+)
local here = (arg and arg[0] or ""):match("^(.*)[/\\]") or "."
local B = dofile(here .. "/../src/lua/browse_logic.lua")

local failures, count = 0, 0
local function check(name, got, want)
	count = count + 1
	if got ~= want then
		failures = failures + 1
		print(("FAIL %s: got %s, want %s"):format(name, tostring(got), tostring(want)))
	end
end

local cols = { "a", "n", "d", "m", "g", "w", "c", "l", "p", "x", "b", "v", "pw", "q", "h", "r", "la", "lo" }
local function idx(rows) return { v = 1, at = 1800000000, cols = cols, s = rows } end
local good = { "45.10.1.7:27015", "My \1Server", "DarkRP", "rp_downtown", "darkrp", "123", "rp", "de", 40, 64, 2, 20260429, 0, 1, "fake", 12, 525, 134 }

-- Rows
local rows, at = B.indexRows(idx({ good }))
check("one row", #rows, 1)
check("time", at, 1800000000)
local s = rows[1]
check("address", s.address, "45.10.1.7:27015")
check("name: control characters gone", s.name, "My Server")
check("title", s.desc, "DarkRP")
check("players", s.players, 40)
check("bots", s.bots, 2)
check("version", s.version, 20260429)
check("password", s.pass, false)
check("answers", s.quiet, false)
check("lat", s.lat, 52.5)
check("lon", s.lon, 13.4)
check("verdict", s.h, "fake")
check("real", s.real, 12)
check("tag", B.indexTag(s), "FAKE - 12 real")

local bad = {
	{ "nonsense", "x" }, { "300.1.1.1:27015", "x" }, { "1.2.3.4:0", "x" }, "not a row",
	{ "1.2.3.4:27015", { "table name" }, nil, nil, nil, "12ab", nil, nil, -5, 99999, nil, nil, 7, 0, "evil", nil, 5000, 5 },
}
rows = B.indexRows(idx(bad))
check("bad rows skipped", #rows, 1)
check("bad name", rows[1].name, "")
check("bad workshop id", rows[1].ws, "")
check("negative players", rows[1].players, 0)
check("too many max", rows[1].max, 0)
check("unknown password", rows[1].pass, false)
check("silent", rows[1].quiet, true)
check("unknown verdict", rows[1].h, nil)
check("location out of range", rows[1].lat, nil)

check("not an index", (B.indexRows({ v = 2, cols = cols, s = {} })), nil)
check("no columns", (B.indexRows({ v = 1, at = 1, cols = { "x" }, s = {} })), nil)
check("columns in another order", B.indexRows({ v = 1, at = 1, cols = { "p", "n", "a" }, s = { { 5, "N", "45.1.1.1:27015" } } })[1].players, 5)
check("row cap", #B.indexRows(idx({ good, good, good }), 2), 2)

-- In steps (the menu spreads a big index over frames): the same rows as at once.
local function serialize(list)
	local keys = { "address", "name", "desc", "map", "gm", "ws", "cat", "loc", "players", "max", "bots", "version", "pass", "quiet", "lat", "lon", "h", "real" }
	local out = {}
	for _, s in ipairs(list) do
		for _, k in ipairs(keys) do out[#out + 1] = tostring(s[k]) end
		out[#out + 1] = "\n"
	end
	return table.concat(out, "|")
end
local many = {}
for i = 1, 2500 do
	local r = {}
	for k, v in pairs(good) do r[k] = v end
	r[1] = "45.10." .. math.floor(i / 250) .. "." .. (i % 250) .. ":27015"
	r[9] = i % 70
	if i % 7 == 0 then r = bad[(i / 7) % #bad + 1] end
	many[i] = r
end
local whole, wholeAt = B.indexRows(idx(many))
check("in steps: the test's index has rows", #whole > 2000, true)
for _, step in ipairs({ 1, 7, 100, 2499, 5000 }) do
	local job = B.indexRowsBegin(idx(many))
	local steps = 0
	repeat steps = steps + 1 until B.indexRowsStep(job, step) or steps > 10000
	check("in steps of " .. step .. ": the same rows as at once", serialize(job.out) == serialize(whole) and job.at == wholeAt, true)
end
local capped = B.indexRowsBegin(idx(many), 10)
repeat until B.indexRowsStep(capped, 3)
check("in steps: the row cap", #capped.out, 10)
check("in steps: not an index", select(2, B.indexRowsBegin({ v = 2 })), "not an index")
local holes = idx({ good, good })
holes.s[4] = good   -- after a hole: not read, as ipairs wouldn't
check("in steps: a hole ends the rows", #B.indexRows(holes), 2)

-- Geo and distance
local lat, lon = B.parseGeo("52.5,13.4")
check("geo lat", lat, 52.5)
check("geo lon", lon, 13.4)
check("geo bad", B.parseGeo("52.5;13.4"), nil)
check("geo out of range", B.parseGeo("95,13"), nil)
check("Berlin-Frankfurt ~420 km", math.floor(B.distanceKm(52.5, 13.4, 50.1, 8.7) / 10), 42)
check("same place", B.estimatePing(0), 10)
check("transatlantic ~100 ms", B.estimatePing(6200) >= 90 and B.estimatePing(6200) <= 110, true)

-- Ping memory
local mem = {}
B.remember(mem, "45.10.1.7:27015", 40, 100)
check("remembered", B.recall(mem, "45.10.1.200:27016", 100), 40)
B.remember(mem, "45.10.1.7:27015", 90, 101)
check("smoothed", B.recall(mem, "45.10.1.7:27015", 101), 60)
check("too old", B.recall(mem, "45.10.1.7:27015", 140), nil)
B.remember(mem, "45.10.2.7:27015", 0, 100)
check("no zero pings", B.recall(mem, "45.10.2.7:27015", 100), nil)
B.remember(mem, "nonsense", 50, 100)
check("other network", B.recall(mem, "45.10.9.7:27015", 101), nil)

local loaded = B.loadPings({ ["1.2.3"] = { 30, 100 }, ["bad key"] = { 30, 100 }, ["4.5.6"] = { "x", 100 }, ["7.8.9"] = { 30, 10 } }, 101)
check("load: good", loaded["1.2.3"] and loaded["1.2.3"][1], 30)
check("load: bad key", loaded["bad key"], nil)
check("load: bad value", loaded["4.5.6"], nil)
check("load: expired", loaded["7.8.9"], nil)

local big = {}
for i = 1, B.REMEMBER_MAX + 10 do big["10." .. math.floor(i / 256) .. "." .. (i % 256)] = { 50, i } end
local pruned, n = B.prunePings(big), 0
for _ in pairs(pruned) do n = n + 1 end
check("pruned", n, B.REMEMBER_MAX)
check("pruned keeps newest", pruned["10." .. math.floor((B.REMEMBER_MAX + 10) / 256) .. "." .. ((B.REMEMBER_MAX + 10) % 256)] ~= nil, true)

-- Hints
local srv = { address = "45.10.1.7:27015", lat = 50.1, lon = 8.7 }
local ms, from = B.pingHint({}, srv, 100, 52.5, 13.4)
check("hint from distance", from, "distance")
check("hint value", ms, B.estimatePing(B.distanceKm(52.5, 13.4, 50.1, 8.7)))
ms, from = B.pingHint({ ["45.10.1"] = { 33, 100 } }, srv, 100, 52.5, 13.4)
check("hint from memory first", from, "seen")
check("hint memory value", ms, 33)
check("no hint", (B.pingHint({}, { address = "45.10.1.7:27015" }, 100, nil, nil)), nil)

-- Changes
local a = { players = 1, max = 10, bots = 0, map = "m", name = "n", pass = false, desc = "D" }
local b = { players = 1, max = 10, bots = 0, map = "m", name = "n", pass = false, desc = "" }
check("unchanged (no title in the new one)", B.changed(a, b), false)
b.players = 2
check("players changed", B.changed(a, b), true)

print(("%d checks, %d failed"):format(count, failures))
if failures > 0 then os.exit(1) end
