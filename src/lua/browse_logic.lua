-- Find servers' rules for Meowy's server index (roadmap MR3,
-- docs/server-index-and-parties.md), no game: tests/browse_logic_test.lua.
-- menu_page.lua has it as `BrowseLogic`.
--
-- The index (GET /api/pack/index) is { v = 1, at, cols = { "a", "n", ... },
-- s = { { ... }, ... } }: one array per server in the order `cols` gives. Every
-- value is checked here before a row is built from it, as for Steam's own
-- answers: the backend is ours, but the text in it is each server's own.
--
-- Ping hints, until Steam has measured a server: the ping this PC measured on
-- that server's network (/24) before, else an estimate from the distance
-- between this PC's coarse location and the server's (both from the index,
-- IP Geolocation by DB-IP), else none.
local M = {}

M.MAX_AGE = 600            -- seconds: an older index isn't shown
M.REMEMBER_DAYS = 30       -- a remembered ping counts for this long
M.REMEMBER_MAX = 4000      -- networks kept
M.UNKNOWN_PING = 150       -- what an unmeasured server is ranked by (Garry's Mod's rank is ping-based)

local function str(v, max)
	if type(v) ~= "string" then return "" end
	v = v:gsub("[%c]", "")
	return #v > max and v:sub(1, max) or v
end

local function int(v, lo, hi)
	v = tonumber(v)
	if not v or v ~= v then return nil end
	v = math.floor(v)
	if v < lo or v > hi then return nil end
	return v
end

-- "a.b.c.d:port" with every part in range, or nil (no private-range check
-- here: menu_page.lua's Home.publicAddress does that for every row).
local function address(a)
	if type(a) ~= "string" or #a > 21 then return nil end
	local p1, p2, p3, p4, port = a:match("^(%d+)%.(%d+)%.(%d+)%.(%d+):(%d+)$")
	if not p1 then return nil end
	for _, p in ipairs({ p1, p2, p3, p4 }) do if tonumber(p) > 255 then return nil end end
	port = tonumber(port)
	if port < 1 or port > 65535 then return nil end
	return a
end
M.address = address

-- The index's servers as the stock query's terms (menu_page.lua's browseRow
-- reads these), with `quiet` (didn't answer the index's own queries lately),
-- `lat`/`lon`, and the backend's verdict (`h`, `real`). Bad rows are skipped.
-- Returns the list and the snapshot's time (unix seconds), or nil and why.
function M.indexRows(data, maxRows)
	if type(data) ~= "table" or data.v ~= 1 or type(data.cols) ~= "table" or type(data.s) ~= "table" then
		return nil, "not an index"
	end
	local at = int(data.at, 0, 2 ^ 40)
	if not at then return nil, "no time" end
	local col = {}
	for i, name in ipairs(data.cols) do if type(name) == "string" then col[name] = i end end
	if not col.a or not col.n or not col.p then return nil, "missing columns" end
	local out, max = {}, maxRows or 30000
	for _, r in ipairs(data.s) do
		if #out >= max then break end
		local a = type(r) == "table" and address(r[col.a]) or nil
		if a then
			local function get(name) return col[name] and r[col[name]] or nil end
			local pw, q = int(get("pw"), -1, 1), int(get("q"), -1, 1)
			local la, lo = int(get("la"), -900, 900), int(get("lo"), -1800, 1800)
			local h = get("h")
			out[#out + 1] = {
				address = a,
				name = str(get("n"), 128),
				desc = str(get("d"), 64),
				map = str(get("m"), 64),
				gm = str(get("g"), 64),
				ws = str(get("w"), 20):match("^%d+$") or "",
				cat = str(get("c"), 10),
				loc = str(get("l"), 20),
				players = int(get("p"), 0, 4096) or 0,
				max = int(get("x"), 0, 4096) or 0,
				bots = int(get("b"), 0, 4096) or 0,
				version = int(get("v"), 0, 999999999) or 0,
				pass = pw == 1,
				quiet = q == 0,
				lat = la and lo and la / 10 or nil,
				lon = la and lo and lo / 10 or nil,
				h = (h == "fake" or h == "verified" or h == "unverified") and h or nil,
				real = int(get("r"), 0, 4096),
			}
		end
	end
	return out, at
end

-- The tag the honest list would show for the backend's verdict alone (used
-- until the pack's own verdicts have arrived), or nil.
function M.indexTag(s)
	if s.h == "fake" and s.real then return "FAKE - " .. s.real .. " real" end
	if s.h == "verified" then return "VERIFIED" end
	if s.h == "unverified" then return "UNVERIFIED" end
	return nil
end

-- X-Meowy-Geo ("52.5,13.4") as lat, lon, or nil.
function M.parseGeo(text)
	if type(text) ~= "string" then return nil end
	local lat, lon = text:match("^(%-?%d+%.?%d*),(%-?%d+%.?%d*)$")
	lat, lon = tonumber(lat), tonumber(lon)
	if not lat or not lon or math.abs(lat) > 90 or math.abs(lon) > 180 then return nil end
	return lat, lon
end

-- Great-circle distance in km.
function M.distanceKm(lat1, lon1, lat2, lon2)
	local r = math.pi / 180
	local dlat, dlon = (lat2 - lat1) * r, (lon2 - lon1) * r
	local a = math.sin(dlat / 2) ^ 2 + math.cos(lat1 * r) * math.cos(lat2 * r) * math.sin(dlon / 2) ^ 2
	return 2 * 6371 * math.asin(math.min(1, math.sqrt(a)))
end

-- A round trip over that distance as the internet makes it: light in fibre
-- (~200 km/ms, both ways), routes ~1.4x the straight line, plus the access
-- networks at both ends. Close to measured pings within a continent; long
-- routes vary more.
function M.estimatePing(km)
	return math.floor(10 + km * 0.014 + 0.5)
end

-- The network a server's ping stands for: its /24.
function M.netKey(a)
	return type(a) == "string" and a:match("^(%d+%.%d+%.%d+)%.%d+:%d+$") or nil
end

-- The ping memory: { ["a.b.c"] = { ms, day } }, day = days since 1970.
function M.remember(mem, a, ms, day)
	local key = M.netKey(a)
	ms = tonumber(ms)
	if not key or not ms or ms <= 0 or ms >= 2000 then return end
	local old = mem[key]
	-- Smoothed: one slow answer doesn't replace a network's usual ping.
	local value = old and old[2] and day - old[2] <= M.REMEMBER_DAYS and math.floor(old[1] * 0.6 + ms * 0.4 + 0.5) or math.floor(ms)
	mem[key] = { value, day }
end

function M.recall(mem, a, day)
	local key = M.netKey(a)
	local e = key and mem[key]
	if type(e) ~= "table" or not tonumber(e[1]) or not tonumber(e[2]) or day - e[2] > M.REMEMBER_DAYS then return nil end
	return e[1]
end

-- From the saved JSON's table: only well-formed, recent entries.
function M.loadPings(tbl, day)
	local mem = {}
	if type(tbl) ~= "table" then return mem end
	for key, e in pairs(tbl) do
		if type(key) == "string" and key:match("^%d+%.%d+%.%d+$") and type(e) == "table" then
			local ms, d = int(e[1], 1, 1999), int(e[2], 0, 10 ^ 6)
			if ms and d and day - d <= M.REMEMBER_DAYS then mem[key] = { ms, d } end
		end
	end
	return mem
end

-- To save: the REMEMBER_MAX most recent networks.
function M.prunePings(mem)
	local list = {}
	for key, e in pairs(mem) do list[#list + 1] = { key, e } end
	if #list <= M.REMEMBER_MAX then return mem end
	table.sort(list, function(x, y) return x[2][2] > y[2][2] end)
	local out = {}
	for i = 1, M.REMEMBER_MAX do out[list[i][1]] = list[i][2] end
	return out
end

-- The hint for a server: ms and where it came from ("seen", "distance"), or nil.
function M.pingHint(mem, s, day, hereLat, hereLon)
	local seen = M.recall(mem, s.address, day)
	if seen then return seen, "seen" end
	if hereLat and s.lat then return M.estimatePing(M.distanceKm(hereLat, hereLon, s.lat, s.lon)), "distance" end
	return nil
end

-- Whether an index row says something new about a listed server.
function M.changed(old, new)
	return old.players ~= new.players or old.max ~= new.max or old.bots ~= new.bots or old.map ~= new.map
		or old.name ~= new.name or old.pass ~= new.pass or (new.desc ~= "" and old.desc ~= new.desc)
end

return M
