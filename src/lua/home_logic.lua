-- The main menu's Home card: its rules, as pure functions (no GMod API), so
-- tests/home_logic_test.lua can run them with a plain Lua interpreter.
-- Lua 5.1 compatible: Garry's Mod runs LuaJIT.
--
-- Gets `Logic` (resilience_logic.lua) as its argument, for ago and jsString.
--
-- Everything a server says about itself (its name, map, counts) is the
-- server's own text: cleaned here, then only ever shown as text on the page.
-- A row is joined by its number; the address stays on the Lua side.
local Logic = ...

local M = {}

M.FAVORITES = 5     -- rows per section
M.RECENT = 5
M.KEEP = 60         -- servers read from one list at most
M.NAME_MAX = 80     -- bytes of a name or map kept

-- "a.b.c.d:port", each part in range.
function M.validAddress(a)
	if type(a) ~= "string" then return false end
	local o1, o2, o3, o4, port = string.match(a, "^(%d+)%.(%d+)%.(%d+)%.(%d+):(%d+)$")
	if not o1 then return false end
	for _, o in ipairs({ o1, o2, o3, o4 }) do
		if #o > 3 or tonumber(o) > 255 then return false end
	end
	port = tonumber(port)
	return port >= 1 and port <= 65535 and #a <= 21
end

-- A valid address on the public internet: not this-network, private, loopback,
-- link-local (Steam's relay addresses in the history list are 169.254.x.x),
-- carrier NAT, multicast or reserved.
function M.publicAddress(a)
	if not M.validAddress(a) then return false end
	local o1, o2 = string.match(a, "^(%d+)%.(%d+)%.")
	o1, o2 = tonumber(o1), tonumber(o2)
	if o1 == 0 or o1 == 10 or o1 == 127 or o1 >= 224 then return false end
	if o1 == 169 and o2 == 254 then return false end
	if o1 == 172 and o2 >= 16 and o2 <= 31 then return false end
	if o1 == 192 and o2 == 168 then return false end
	if o1 == 100 and o2 >= 64 and o2 <= 127 then return false end
	return true
end

-- A server's own text for showing: control characters dropped, spaces
-- trimmed, at most NAME_MAX bytes (not through a UTF-8 character).
function M.clean(s)
	if type(s) ~= "string" then return "" end
	s = string.gsub(s, "%c", "")
	s = string.match(s, "^%s*(.-)%s*$")
	if #s > M.NAME_MAX then
		local cut = M.NAME_MAX
		while cut > 0 do
			local b = string.byte(s, cut + 1)
			if not b or b < 128 or b >= 192 then break end   -- the next byte starts a character
			cut = cut - 1
		end
		s = string.sub(s, 1, cut)
	end
	return s
end

local function count(v)
	v = tonumber(v)
	if not v or v ~= v or v < 0 then return 0 end
	return math.floor(math.min(v, 65535))
end

-- One server from the game's query callback (ping, name, desc, map, players,
-- maxplayers, botplayers, pass, lastplayed, address), or nil when the address
-- isn't a public a.b.c.d:port. Players exclude bots, as the browser shows them.
function M.server(ping, name, desc, map, players, maxplayers, botplayers, pass, lastplayed, address)
	if not M.publicAddress(address) then return nil end
	local bots = count(botplayers)
	return {
		address = address,
		name = M.clean(name),
		map = M.clean(map),
		players = math.max(0, count(players) - bots),
		max = count(maxplayers),
		ping = count(ping),
		locked = pass == true,
		lastplayed = tonumber(lastplayed) and tonumber(lastplayed) > 0 and tonumber(lastplayed) or nil,
	}
end

-- A server that didn't answer.
function M.failed(address)
	if not M.publicAddress(address) then return nil end
	return { address = address, name = "", map = "", failed = true }
end

-- The row a server shows as. id: its number on the page.
local function row(id, s, now, withAgo)
	local name = s.name ~= "" and s.name or s.address
	local detail = {}
	if s.failed then
		detail[#detail + 1] = "no answer"
	elseif s.map ~= "" then
		detail[#detail + 1] = s.map
	end
	if withAgo and s.lastplayed and now then
		local ago = Logic.ago(now - s.lastplayed)
		if ago then detail[#detail + 1] = ago end
	end
	local stats = ""
	if not s.failed and s.max then
		stats = (s.locked and "password \194\183 " or "") .. s.players .. "/" .. s.max .. " \194\183 " .. s.ping .. " ms"
	end
	return {
		id = id,
		name = name,
		detail = table.concat(detail, " \194\183 "),
		stats = stats,
		dim = s.failed == true,
	}
end

local function byPlayers(a, b)
	if (a.failed and 1 or 0) ~= (b.failed and 1 or 0) then return not a.failed end
	if (a.players or 0) ~= (b.players or 0) then return (a.players or 0) > (b.players or 0) end
	return (a.name or "") < (b.name or "")
end

local function byLastPlayed(a, b)
	local ta, tb = a.lastplayed or 0, b.lastplayed or 0
	if ta ~= tb then return ta > tb end
	return a.address < b.address
end

-- What the card shows. st = { oursAddress, ours (a server, failed, or nil while
-- asking), favorite = {servers} or nil, history = {servers} or nil }.
-- Returns { sections = { { title, rows = {...}, note } }, addresses = { [id] = address } }.
function M.model(st, now)
	local sections, addresses, seen = {}, {}, {}
	local function add(s, withAgo)
		local id = #addresses + 1
		addresses[id] = s.address
		seen[s.address] = true
		return row(id, s, now, withAgo)
	end

	if M.validAddress(st.oursAddress) then
		local ours = st.ours
		local sec = { title = "Meowy Network", rows = {} }
		if ours then
			local r = add(ours, false)
			if ours.failed or ours.name == "" then r.name = "Meowy Network" end
			sec.rows[1] = r
		else
			seen[st.oursAddress] = true
			sec.note = "Checking..."
		end
		sections[#sections + 1] = sec
	end

	-- Servers that answered, as rows; those that didn't, only counted (a
	-- favourite that's down is worth a line, not a row each).
	local function list(title, servers, limit, sort, withAgo, countSilent)
		if not servers then
			sections[#sections + 1] = { title = title, rows = {}, note = "Checking..." }
			return
		end
		local pick, silent = {}, 0
		for _, s in ipairs(servers) do
			if not seen[s.address] then
				seen[s.address] = true
				if s.failed then silent = silent + 1 else pick[#pick + 1] = s end
			end
		end
		table.sort(pick, sort)
		local sec = { title = title, rows = {} }
		for i = 1, math.min(limit, #pick) do sec.rows[i] = add(pick[i], withAgo) end
		if countSilent and silent > 0 then
			local more = #sec.rows > 0 and " more" or ""
			sec.note = silent .. more .. (silent == 1 and " isn't answering" or " aren't answering")
		end
		if #sec.rows > 0 or sec.note then sections[#sections + 1] = sec end
	end
	list("Favourites", st.favorite, M.FAVORITES, byPlayers, false, true)
	list("Recent", st.history, M.RECENT, byLastPlayed, true, false)

	return { sections = sections, addresses = addresses }
end

-- The Escape menu's card -----------------------------------------------------------

-- Quick settings: only these convars, only in these ranges. The page sends a
-- row number and a number; nothing else crosses back.
M.SETTINGS = {
	{ key = "volume", label = "Master volume", min = 0, max = 1, step = 0.05, percent = true },
	{ key = "voice_scale", label = "Voice volume", min = 0, max = 1, step = 0.05, percent = true },
	{ key = "fov_desired", label = "Field of view", min = 75, max = 100, step = 1 },
}

-- Setting `i` with `value` clamped and snapped to its step, as the string to
-- set; nil for an unknown row or a value that isn't a number.
function M.settingValue(i, value)
	local s = M.SETTINGS[tonumber(i) or 0]
	value = tonumber(value)
	if not s or not value or value ~= value then return nil end
	value = math.max(s.min, math.min(s.max, value))
	value = s.min + math.floor((value - s.min) / s.step + 0.5) * s.step
	value = math.max(s.min, math.min(s.max, value))
	return s.key, s.step < 1 and string.format("%.2f", value) or string.format("%d", value)
end

-- "45 s", "12 min", "1 h 12 min".
function M.duration(seconds)
	seconds = tonumber(seconds)
	if not seconds or seconds ~= seconds or seconds < 0 then return nil end
	if seconds < 60 then return math.floor(seconds) .. " s" end
	local minutes = math.floor(seconds / 60)
	if minutes < 60 then return minutes .. " min" end
	return math.floor(minutes / 60) .. " h " .. (minutes % 60) .. " min"
end

-- What the card shows. info = { address (public or ours, else nil), name, map,
-- players, max (from the server's answer, may be nil), latency (ms), loss
-- (0..1), connected (s), values = { [key] = number } }.
function M.escapeModel(info)
	local out = { settings = {} }
	if M.validAddress(info.address) then
		local name = M.clean(info.name)
		local line1 = {}
		local map = M.clean(info.map)
		if map ~= "" then line1[#line1 + 1] = map end
		if tonumber(info.players) and tonumber(info.max) then
			line1[#line1 + 1] = count(info.players) .. "/" .. count(info.max) .. " players"
		end
		local line2 = {}
		local latency = tonumber(info.latency)
		if latency then
			-- As net_graph shows it: less half an update interval (cl_updaterate).
			local rate = tonumber(info.updaterate)
			if rate and rate > 0 then latency = math.max(0, latency - 500 / rate) end
			line2[#line2 + 1] = "ping " .. math.floor(latency + 0.5) .. " ms"
		end
		if tonumber(info.loss) then line2[#line2 + 1] = "loss " .. math.floor(tonumber(info.loss) * 100 + 0.5) .. "%" end
		local here = M.duration(info.connected)
		if here then line2[#line2 + 1] = here .. " here" end
		out.server = {
			name = name ~= "" and name or info.address,
			line1 = table.concat(line1, " \194\183 "),
			line2 = table.concat(line2, " \194\183 "),
			address = info.address,
		}
	end
	for i, s in ipairs(M.SETTINGS) do
		local v = tonumber(info.values and info.values[s.key])
		if v and v == v then
			v = math.max(s.min, math.min(s.max, v))
			out.settings[#out.settings + 1] = {
				id = i, label = s.label, min = s.min, max = s.max, step = s.step, value = v,
				percent = s.percent == true,
			}
		end
	end
	return out
end

-- A Lua value as a JavaScript literal: strings through Logic.jsString, numbers,
-- booleans, nil as null; a table with [1] as an array, otherwise an object
-- (keys sorted, so the same model always reads the same).
function M.encode(v)
	local t = type(v)
	if t == "string" then return Logic.jsString(v) end
	if t == "number" then
		if v ~= v or v == math.huge or v == -math.huge then return "0" end
		return string.format("%.14g", v)
	end
	if t == "boolean" then return v and "true" or "false" end
	if t ~= "table" then return "null" end
	local out = {}
	if v[1] ~= nil or next(v) == nil then
		for i = 1, #v do out[i] = M.encode(v[i]) end
		return "[" .. table.concat(out, ",") .. "]"
	end
	local keys = {}
	for k in pairs(v) do if type(k) == "string" then keys[#keys + 1] = k end end
	table.sort(keys)
	for _, k in ipairs(keys) do out[#out + 1] = Logic.jsString(k) .. ":" .. M.encode(v[k]) end
	return "{" .. table.concat(out, ",") .. "}"
end

return M
