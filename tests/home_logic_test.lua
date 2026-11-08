-- The main menu's Home card rules (src/lua/home_logic.lua), no game:
--   lua tests/home_logic_test.lua        (from the repository root; Lua 5.1+)
local here = (arg and arg[0] or ""):match("^(.*)[/\\]") or "."
local Logic = dofile(here .. "/../src/lua/resilience_logic.lua")
local H = assert(loadfile(here .. "/../src/lua/home_logic.lua"))(Logic)

local failures, count = 0, 0
local function check(name, got, want)
	count = count + 1
	if got ~= want then
		failures = failures + 1
		print(("FAIL %s: got %s, want %s"):format(name, tostring(got), tostring(want)))
	end
end
local DOT = " \194\183 "

-- Addresses
check("address", H.validAddress("138.201.62.52:27015"), true)
check("address: octet over 255", H.validAddress("256.1.1.1:27015"), false)
check("address: port 0", H.validAddress("1.2.3.4:0"), false)
check("address: port over 65535", H.validAddress("1.2.3.4:65536"), false)
check("address: no port", H.validAddress("1.2.3.4"), false)
check("address: a name", H.validAddress("example.com:27015"), false)
check("address: trailing junk", H.validAddress("1.2.3.4:27015;quit"), false)
check("address: leading zeros, too long", H.validAddress("0001.2.3.4:27015"), false)
check("address: not a string", H.validAddress(nil), false)
check("public: ours", H.publicAddress("138.201.62.52:27015"), true)
check("public: Steam relay / link-local", H.publicAddress("169.254.100.14:27259"), false)
check("public: LAN 192.168", H.publicAddress("192.168.1.5:27015"), false)
check("public: LAN 10", H.publicAddress("10.0.0.2:27015"), false)
check("public: LAN 172.16-31", H.publicAddress("172.20.1.1:27015") or H.publicAddress("172.31.0.1:1"), false)
check("public: 172.32 is public", H.publicAddress("172.32.0.1:27015"), true)
check("public: loopback", H.publicAddress("127.0.0.1:27015"), false)
check("public: carrier NAT", H.publicAddress("100.64.0.1:27015"), false)
check("public: multicast and reserved", H.publicAddress("224.0.0.1:1") or H.publicAddress("255.255.255.255:1"), false)
check("public: this network", H.publicAddress("0.0.0.0:27015"), false)
check("server: a LAN address isn't kept", H.server(1, "x", "", "m", 1, 2, 0, false, 0, "192.168.1.5:27015"), nil)
check("failed: a relay address isn't kept", H.failed("169.254.1.17:27042"), nil)

-- Names
check("clean: controls dropped, trimmed", H.clean("  Foo\nRP\t\0 "), "FooRP")
check("clean: capped", #H.clean(string.rep("x", 200)), H.NAME_MAX)
local u = string.rep("x", H.NAME_MAX - 1) .. "\195\169yy"
check("clean: not through a UTF-8 character", H.clean(u), string.rep("x", H.NAME_MAX - 1))
check("clean: not a string", H.clean(nil), "")

-- One server from the query callback
local s = H.server(38, "Foo RP", "desc", "rp_city", 40, 64, 4, false, 1000, "1.2.3.4:27015")
check("server: players exclude bots", s and s.players, 36)
check("server: max", s and s.max, 64)
check("server: ping", s and s.ping, 38)
check("server: last played", s and s.lastplayed, 1000)
check("server: not locked", s and s.locked, false)
check("server: bad address refused", H.server(38, "x", "", "m", 1, 2, 0, false, 0, "nope"), nil)
s = H.server(-5, "x", "", "m", "lots", 1e9, 0 / 0, true, 0, "1.2.3.4:1")
check("server: junk counts made safe", s and s.ping == 0 and s.players == 0 and s.max == 65535, true)
check("server: locked", s and s.locked, true)
check("server: no last played", s and s.lastplayed, nil)
check("failed: kept with its address", H.failed("1.2.3.4:27015").failed, true)
check("failed: bad address refused", H.failed("x"), nil)

-- The model
local OURS = "138.201.62.52:27015"
local function srv(addr, name, players, max, ping, last, map)
	return H.server(ping or 30, name, "", map or "gm_x", players or 0, max or 32, 0, false, last or 0, addr)
end
local now = 1000000000
local m = H.model({ oursAddress = OURS }, now)
check("model: ours first, checking", m.sections[1].title, "Meowy Network")
check("model: ours note", m.sections[1].note, "Checking...")
check("model: favourites checking", m.sections[2].title == "Favourites" and m.sections[2].note, "Checking...")
check("model: recent checking", m.sections[3].title == "Recent" and m.sections[3].note, "Checking...")
check("model: nothing to join yet", #m.addresses, 0)

local fav = {
	srv("1.1.1.1:27015", "Small", 2, 16, 40),
	srv("2.2.2.2:27015", "Big", 50, 64, 60),
	srv(OURS, "Meowy dup", 10, 32, 20),
	H.failed("3.3.3.3:27015"),
}
local hist = {
	srv("2.2.2.2:27015", "Big", 50, 64, 60, now - 50),          -- also a favourite: not again
	srv("4.4.4.4:27015", "Old", 1, 10, 90, now - 86400 * 3),
	srv("5.5.5.5:27015", "New", 5, 10, 90, now - 7200),
	srv(OURS, "Meowy", 10, 32, 20, now - 10),                    -- ours: not again
}
for i = 1, 8 do hist[#hist + 1] = srv("6.6.6." .. i .. ":27015", "H" .. i, 0, 10, 50, now - 86400 * 10 - i) end
m = H.model({ oursAddress = OURS, ours = srv(OURS, "Meowy Network | DarkRP", 12, 32, 18, 0, "rp_downtown_meowy"), favorite = fav, history = hist }, now)
local secs = m.sections
check("model: three sections", #secs, 3)
local o = secs[1].rows[1]
check("model: ours row", o and o.name, "Meowy Network | DarkRP")
check("model: ours stats", o and o.stats, "12/32" .. DOT .. "18 ms")
check("model: ours detail is the map", o and o.detail, "rp_downtown_meowy")
check("model: ours joins ours", m.addresses[o.id], OURS)
local f = secs[2].rows
check("model: favourites without ours or silent ones", #f, 2)
check("model: busiest favourite first", f[1].name, "Big")
check("model: then by players", f[2].name, "Small")
check("model: silent favourites counted, not listed", secs[2].note, "1 more isn't answering")
local r = secs[3].rows
check("model: recent shows a few", secs[3].show, H.RECENT)
check("model: but every answering one is a row, for search", #r, 10)
check("model: recent newest first, no repeats", r[1].name, "New")
check("model: recent shows when", r[1].detail, "gm_x" .. DOT .. "2 h ago")
check("model: older after", r[2].name, "Old")
check("model: days ago", r[2].detail, "gm_x" .. DOT .. "3 days ago")
local ids = {}
for _, sec in ipairs(secs) do for _, row in ipairs(sec.rows) do ids[#ids + 1] = row.id end end
local distinct = true
for i, id in ipairs(ids) do if id ~= i then distinct = false end end
check("model: rows numbered 1..n in order", distinct and #ids == #m.addresses, true)
check("model: each number maps to its server", m.addresses[f[1].id], "2.2.2.2:27015")

m = H.model({ oursAddress = OURS, ours = H.failed(OURS), favorite = {}, history = {} }, now)
check("model: ours not answering", m.sections[1].rows[1].name == "Meowy Network" and m.sections[1].rows[1].detail == "no answer", true)
check("model: ours not answering, dimmed, no counts", m.sections[1].rows[1].dim == true and m.sections[1].rows[1].stats == "", true)
check("model: empty lists hidden", #m.sections, 1)
m = H.model({ favorite = { H.failed("1.1.1.1:1"), H.failed("2.2.2.2:2") }, history = { H.failed("3.3.3.3:3") } }, now)
check("model: only silent favourites: the count alone", #m.sections == 1 and #m.sections[1].rows == 0 and m.sections[1].note, "2 aren't answering")
check("model: silent recent ones left out", #m.sections, 1)
m = H.model({ oursAddress = nil, favorite = {}, history = { srv("5.5.5.5:27015", "New", 5, 10, 90, now - 30) } }, now)
check("model: no address of ours: no ours section", m.sections[1].title, "Recent")
local locked = H.server(30, "Priv", "", "m", 1, 2, 0, true, 0, "7.7.7.7:27015")
m = H.model({ favorite = { locked }, history = {} }, now)
check("model: password shown", m.sections[1].rows[1].stats, "password" .. DOT .. "1/2" .. DOT .. "30 ms")
local unnamed = srv("8.8.8.8:27015", "", 1, 2, 3)
m = H.model({ favorite = { unnamed }, history = {} }, now)
check("model: no name: the address", m.sections[1].rows[1].name, "8.8.8.8:27015")

-- Friends' servers: first after ours, listed once, hidden when there are none.
local friends = { srv("2.2.2.2:27015", "Big", 50, 64, 60), srv("9.9.9.9:27015", "Pals", 3, 10, 30) }
m = H.model({ oursAddress = OURS, ours = srv(OURS, "Meowy", 1, 2, 3), friends = friends, favorite = fav, history = {} }, now)
check("friends: a section after ours", m.sections[2].title, "Friends are playing on")
check("friends: shows a few", m.sections[2].show, H.FRIENDS)
check("friends: busiest first", m.sections[2].rows[1].name, "Big")
local inFav = false
for _, row in ipairs(m.sections[3].rows) do if row.name == "Big" then inFav = true end end
check("friends: not listed again under favourites", m.sections[3].title == "Favourites" and not inFav, true)
m = H.model({ oursAddress = OURS, friends = nil, favorite = {}, history = {} }, now)
check("friends: no 'Checking...' section while asking", m.sections[2] == nil or m.sections[2].title ~= "Friends are playing on", true)
m = H.model({ friends = {}, favorite = {}, history = {} }, now)
check("friends: none, no section", #m.sections, 0)

-- The menu page's zoom
check("zoom: auto at 1080p", H.menuZoom("0", 1080), 1)
check("zoom: auto at 1440p", H.menuZoom("0", 1440), 1.33)
check("zoom: auto at 4K", H.menuZoom("0", 2160), 2)
check("zoom: auto never below 1", H.menuZoom("0", 768), 1)
check("zoom: auto, capped", H.menuZoom("0", 4320), 2.5)
check("zoom: a chosen size", H.menuZoom("150", 1440), 1.5)
check("zoom: a chosen size, clamped low", H.menuZoom("10", 1440), 0.75)
check("zoom: a chosen size, clamped high", H.menuZoom("900", 1440), 2.5)
check("zoom: junk is auto", H.menuZoom("big", 1440), 1.33)
check("zoom: no height", H.menuZoom("0", nil), 1)

-- Encoding for the page
check("encode: string", H.encode("a\"b"), [["a\x22b"]])
check("encode: number", H.encode(12), "12")
check("encode: NaN made safe", H.encode(0 / 0), "0")
check("encode: bool", H.encode(true), "true")
check("encode: nil", H.encode(nil), "null")
check("encode: empty table is an array", H.encode({}), "[]")
check("encode: array", H.encode({ 1, "x" }), '[1,"x"]')
check("encode: object, keys sorted", H.encode({ b = 1, a = "y" }), [[{"a":"y","b":1}]])
local evil = H.server(1, "</script><img src=x onerror=lua.Run('x')>", "", "m", 1, 2, 0, false, 0, "9.9.9.9:27015")
local text = H.encode(H.model({ favorite = { evil }, history = {} }, now).sections)
check("encode: a hostile name can't break out", text:find("</", 1, true) == nil and text:find("'", 1, true) == nil and text:find("<", 1, true) == nil, true)
check("encode: same model, same text", text, H.encode(H.model({ favorite = { evil }, history = {} }, now).sections))

-- The Escape menu's card
check("setting: volume clamped and snapped", select(2, H.settingValue(1, 0.73)), "0.75")
check("setting: its convar", (H.settingValue(1, 0.5)), "volume")
check("setting: above the range", select(2, H.settingValue(2, 7)), "1.00")
check("setting: below the range", select(2, H.settingValue(1, -3)), "0.00")
check("setting: fov whole degrees", select(2, H.settingValue(3, 90.4)), "90")
check("setting: fov range", select(2, H.settingValue(3, 200)), "100")
check("setting: fov low", select(2, H.settingValue(3, 10)), "75")
check("setting: unknown row", H.settingValue(9, 1), nil)
check("setting: junk row", H.settingValue("1; quit", 1), nil)
check("setting: not a number", H.settingValue(1, "x"), nil)
check("setting: NaN", H.settingValue(1, 0 / 0), nil)
check("duration: seconds", H.duration(45.7), "45 s")
check("duration: minutes", H.duration(12 * 60 + 5), "12 min")
check("duration: hours", H.duration(3600 + 12 * 60), "1 h 12 min")
check("duration: junk", H.duration(-1), nil)
local e = H.escapeModel({ address = OURS, name = "Meowy | DarkRP", map = "rp_downtown_meowy", players = 12, max = 32,
	latency = 45.6, loss = 0.013, connected = 4320, values = { volume = 0.7, voice_scale = 2, fov_desired = 90 } })
check("escape: server name", e.server and e.server.name, "Meowy | DarkRP")
check("escape: map and players", e.server and e.server.line1, "rp_downtown_meowy" .. DOT .. "12/32 players")
check("escape: ping, loss, time here", e.server and e.server.line2, "ping 46 ms" .. DOT .. "loss 1%" .. DOT .. "1 h 12 min here")
check("escape: address", e.server and e.server.address, OURS)
local e2 = H.escapeModel({ address = OURS, latency = 63.86, updaterate = 32, values = {} })
check("escape: ping as net_graph shows it (less half an update)", e2.server.line2, "ping 48 ms")
e2 = H.escapeModel({ address = OURS, latency = 5, updaterate = 10, values = {} })
check("escape: ping never below 0", e2.server.line2, "ping 0 ms")
check("escape: three settings", #e.settings, 3)
check("escape: a setting's value", e.settings[1].value, 0.7)
check("escape: out-of-range value shown clamped", e.settings[2].value, 1)
check("escape: settings numbered by row", e.settings[3].id, 3)
e = H.escapeModel({ address = nil, name = "LAN game", values = { volume = 0.5 } })
check("escape: no public address: no server section", e.server, nil)
check("escape: settings still there", #e.settings, 1)
e = H.escapeModel({ address = "1.2.3.4:27015", name = "", values = {} })
check("escape: no name: the address", e.server.name, "1.2.3.4:27015")
check("escape: nothing measured yet: empty lines", e.server.line1 == "" and e.server.line2 == "", true)
check("escape: no party", e.party, nil)
e = H.escapeModel({ address = "1.2.3.4:27015", values = {}, party = { text = "Following Ann to X in 5 s", stay = true } })
check("escape: party line cleaned", e.party.text, "Following Ann to X in 5 s")
check("escape: party stay", e.party.stay, true)
e = H.escapeModel({ address = "1.2.3.4:27015", values = {}, party = { text = 5 } })
check("escape: a party line that isn't text", e.party, nil)

print(("%d checks, %d failed"):format(count, failures))
os.exit(failures == 0 and 0 or 1)
