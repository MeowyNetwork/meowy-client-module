-- Meowy home screen (U5 shell, preview; meowy_menu 1). Runs in its own function
-- at the end of resilience.lua's chunk (module.cpp), with its locals in scope:
-- log, menuOn, menuVisible, menuPage.
--
-- Our home lives inside Garry's Mod's own menu page (src/menu/shell.js): the
-- stock main screen's list and news are hidden by a style rule while it's on,
-- and its links are the page's own routes. Nothing is reloaded, so going to
-- Find servers and back is instant, and the stock page keeps everything its Lua
-- sent it (version, gamemodes, maps, the in-game state). In game the Escape menu
-- is the stock one, with the Escape card.
--
-- The page asks for actions through meowy.post(kind, value): a fixed table of
-- message types, each checking its value, and only while our home is on.
--
-- Also, on or off: the menu page's zoom (meowy_menu_scale, U1). Garry's Mod
-- draws the page at the same pixel size on any screen; a CSS zoom on the root
-- makes it readable at 1440p and 4K (auto: screen height / 1080).
local REFRESH = 1   -- seconds between sends (the script is safe to run again)

local ZOOM_RESEND = 3         -- seconds
local zoomSent, zoomAt = nil, 0
local hatch = false           -- "Use Garry's Mod's menu": stock for the rest of this start
local bridged                 -- the menu's browser panel the bridge is on
local nextLook, sent, sentAt, wasOn, failedTick = 0, nil, 0, false, false
local loaded                  -- the script is in this page (shell.js; it stays until the page reloads)

local js
local function shellJs()
	if js == nil then
		js = meowy.MenuShellJs and meowy.MenuShellJs() or ""
		if not isstring(js) then js = "" end
	end
	return js
end

-- Whether our home should show now.
local function wanted()
	return menuOn() and not hatch and menuVisible() and not IsInGame() and not IsInLoading()
		and IsValid(pnlMainMenu) and pnlMainMenu.Call ~= nil and shellJs() ~= ""
end

-- Find servers on the home screen (U6, src/menu/browser.js): every list Garry's
-- Mod's browser has (Internet, Favourites, History, Local network) and Friends,
-- sent to the page in batches as the servers answer. Internet and Friends come
-- straight from Steam (server_query.h; Internet with players first, then the
-- empty ones), the others from the game's own query. Servers on Garry's Mod's
-- blacklist (mainmenu.lua's IsServerBlacklisted) are left out, as its browser
-- leaves them out, and the honest server list's tags come along ([FAKE - 12
-- real] shows the real count). The page asks for a server by its number; the
-- addresses it's sent are only shown. Every server string goes to the page as
-- an escaped string and is shown as text.
local BROWSE_MAX = 30000
local BROWSE_TABS = { internet = true, favorite = true, history = true, lan = true, friends = true }
local BROWSE_CATS = { rp = true, pvp = true, pve = true, other = true, none = true }
local BROWSE_PREFS = "meowy_browser_prefs"
-- Rows by number, across lists: { address, tab }. A new list of a tab drops that tab's old rows.
-- byAddr: this list's row numbers by address; fromIndex: rows the index gave
-- that Steam hasn't answered for yet; known: what each index row said last;
-- ping: { ms, hint kind } a row was last sent with.
local browse = { gen = 0, rows = {}, nextI = 0, seen = {}, ws = {}, pinged = {}, byAddr = {}, fromIndex = {}, known = {}, ping = {} }
local function tagValue(tags, key) return (" " .. (tags or "") .. " "):match(" " .. key .. ":(%S+)") end
local function ipv4(address) return isstring(address) and address:match("^%d+%.%d+%.%d+%.%d+:%d+$") ~= nil end

-- Meowy's server index (MR3: server_index.h, browse_logic.lua). The Internet
-- list fills from it at once; Steam's own list then updates those rows in
-- place (pings, counts) and adds any the index missed. Nothing is removed.
-- Switched off, unreachable or too old, the list is Steam's alone, as before.
-- Pings Steam measured are remembered per server network (/24) in
-- %LOCALAPPDATA%\MeowyClientPack\pings.json and shown as hints next time.
local indexSetting = meowy.Setting and meowy.Setting("meowy_server_index", "1",
	"Meowy Client Pack: Find servers starts from Meowy's server index, then asks Steam (0 = Steam alone)")
local INDEX_FRESH = 60   -- seconds: an index fetched this recently is used as it is
local idx = { rows = nil, at = 0, got = -math.huge, lat = nil, lon = nil, fetching = false, last = "not fetched yet" }
local HINT_SEEN, HINT_DISTANCE, HINT_NONE = 1, 2, 3   -- row.ph: where a ping that Steam hasn't measured came from
local pingMem, pingDirty
local function today() return math.floor(os.time() / 86400) end
local function indexWanted() return meowy.ServerIndexFetch ~= nil and (indexSetting == nil or indexSetting:GetBool()) end
local function indexAge() return math.max(0, os.time() - idx.at) end
local function indexFresh(now) return idx.rows ~= nil and now - idx.got < INDEX_FRESH and indexAge() <= BrowseLogic.MAX_AGE end

local function pings()
	if not pingMem then
		local json = meowy.PingMemoryLoad and meowy.PingMemoryLoad() or ""
		pingMem = BrowseLogic.loadPings(json ~= "" and util.JSONToTable(json) or nil, today())
	end
	return pingMem
end
local function savePings()
	if not pingDirty or not pingMem or not meowy.PingMemorySave then return end
	pingDirty = false
	meowy.PingMemorySave(util.TableToJSON(BrowseLogic.prunePings(pingMem)))
end

local function indexFetch()
	if indexWanted() and not idx.fetching and meowy.ServerIndexFetch() then idx.fetching = true end
end

local function browseStop()
	if browse.id and meowy.ServerQueryCancel then meowy.ServerQueryCancel(browse.id) end
	browse.id, browse.stock = nil, nil
	savePings()
end

-- Internet stage 1: servers with players; 2: the empty ones. Friends: one list.
local function browseQuery(stage)
	browse.stage = stage
	if not meowy.ServerQueryStart then return false end
	if browse.tab == "friends" then
		browse.id = meowy.ServerQueryStart("friends", "gamedir", "garrysmod")
	else
		browse.id = meowy.ServerQueryStart("internet", "gamedir", "garrysmod", stage == 1 and "hasplayers" or "noplayers", "1")
	end
	return browse.id ~= nil
end

-- Favourites, History and Local network: the game's own query (without the
-- honest list's changes: the tags are added here). Its answers wait in a queue
-- the pump empties.
local function browseStockQuery(tab)
	local query = meowy.StockServerQuery or (serverlist and serverlist.Query)
	if not isfunction(query) then return false end
	local run = { queue = {}, done = false }
	browse.stock, browse.stage = run, 1
	query({
		Type = tab, GameDir = "garrysmod", AppID = 4000,
		Callback = function(ping, name, desc, map, players, maxplayers, botplayers, pass, lastplayed, address, gm, workshopid, isAnon, netVersion, luaVersion, loc, gmcat)
			if browse.stock ~= run then return false end
			run.queue[#run.queue + 1] = { address = address, name = name, desc = desc, map = map, players = players, max = maxplayers,
				bots = botplayers, ping = ping, pass = pass == true, lastplayed = lastplayed, anon = isAnon == true, version = netVersion,
				gm = gm, ws = workshopid, loc = loc, cat = gmcat }
			return true
		end,
		CallbackFailed = function() return browse.stock == run end,
		Finished = function() run.done = true end,
	})
	return true
end

local function browsePrefs()
	return cookie and isfunction(cookie.GetString) and cookie.GetString(BROWSE_PREFS, "") or ""
end

-- Where this list's rows come from, for the page's status line: the index
-- ("waiting", "shown", "failed", "off") and its age.
-- `first`: script to run before it, in the same call (the list's reset).
local function browseSource(first)
	if not (IsValid(pnlMainMenu) and pnlMainMenu.Call) then return end
	local info = { idx = browse.index or "off", age = browse.index == "shown" and indexAge() or -1,
		geo = idx.lat ~= nil, n = browse.indexCount or 0 }
	pnlMainMenu:Call((first or "") .. "if (window.MeowyBrowse && window.MeowyBrowse.source) window.MeowyBrowse.source("
		.. browse.gen .. ", " .. Home.encode(info) .. ");")
end

local function browseStart(tab)
	tab = BROWSE_TABS[tab] and tab or "internet"
	browseStop()
	browse.gen = browse.gen + 1
	browse.tab = tab
	for i, r in pairs(browse.rows) do if r.tab == tab then browse.rows[i] = nil end end
	browse.count, browse.seen, browse.blocked, browse.full, browse.started = 0, {}, 0, false, SysTime()
	browse.byAddr, browse.fromIndex, browse.known, browse.ping, browse.indexCount = {}, {}, {}, {}, 0
	if meowy.FetchServerRep then meowy.FetchServerRep() end
	-- The index first (the Internet list only): shown as soon as it's here.
	browse.index = nil
	if tab == "internet" then
		if indexWanted() then
			browse.index = "waiting"
			if not indexFresh(browse.started) then indexFetch() end
		else
			browse.index = "off"
		end
	end
	local ok
	if tab == "internet" or tab == "friends" then ok = browseQuery(1) else ok = browseStockQuery(tab) end
	browseSource("if (window.MeowyBrowse) window.MeowyBrowse.reset(" .. browse.gen .. ", " .. tostring(ok or browse.index == "waiting") .. ", "
		.. Home.encode(tab) .. ", " .. Home.encode(browsePrefs()) .. ");\n")
	log("browse: %s%s", ok and ("listing " .. tab) or "Steam's server list isn't available",
		browse.index == "waiting" and (idx.fetching and ", asking Meowy's index" or ", from Meowy's index") or "")
end

local function blacklisted(s)
	if not isfunction(IsServerBlacklisted) then return false end
	local ok, hit = pcall(IsServerBlacklisted, s.address, s.name or "", s.desc or "", s.gm or "", s.map or "")
	return ok and hit ~= nil
end

-- The native list's server, in the stock query's terms.
local function fromNative(s)
	local tags = s.tags
	return { address = s.address, name = s.name, desc = s.desc, map = s.map, players = s.players, max = s.max, bots = s.bots,
		ping = s.ping, pass = s.pass == true, lastplayed = s.lastplayed, anon = s.anon == true, version = s.version,
		gm = tagValue(tags, "gm") or "", ws = tagValue(tags, "gmws") or "", loc = tagValue(tags, "loc") or "", cat = tagValue(tags, "gmc") or "" }
end

-- A server as the page reads it (browser.js's server()), numbered: a new
-- number, or `i` again for a row the page already has (an update).
local function browseRow(s, tab, i, keep)
	if not i then
		browse.nextI = browse.nextI + 1
		i = browse.nextI
	end
	browse.rows[i] = { address = s.address, tab = tab, name = s.name }
	local bots = math.max(0, tonumber(s.bots) or 0)
	local players = math.max(0, (tonumber(s.players) or 0) - bots)
	local ws = tostring(s.ws or "")
	ws = #ws <= 20 and ws:match("^%d+$") and ws ~= "0" and ws or ""
	if ws ~= "" then browse.ws[ws] = true end
	local cat = tostring(s.cat or "")
	local isFav = serverlist and serverlist.IsServerFavorite
	local row = {
		i = i, a = s.address, n = Home.clean(s.name), d = Home.clean(s.desc), m = Home.clean(s.map), g = Home.clean(s.gm),
		c = BROWSE_CATS[cat] and cat or "", ws = ws, f = tostring(s.loc or ""):match("^%a+$") or "",
		v = math.floor(tonumber(s.version) or 0),   -- the network version, as the game's list compares it
		p = players, x = math.max(0, (tonumber(s.max) or 0) - bots), b = bots,
		ms = tonumber(s.ping) or 0, pw = s.pass == true, an = s.anon == true, lp = tonumber(s.lastplayed) or 0,
		fv = isfunction(isFav) and isFav(s.address) == true or false,
	}
	-- The honest list's verdict: its tag, the real count of a fake, unverified a little lower.
	local tag, v = nil, nil
	if meowy.ServerTag then tag, v = meowy.ServerTag(s.address) end
	-- Until the pack's own verdicts have arrived (a moment after the list
	-- opens), the index's: the same backend's, without this PC's checks.
	if not isstring(tag) and s.h then
		tag, v = BrowseLogic.indexTag(s), { verdict = s.h, real = s.real }
	end
	if isstring(tag) then
		row.h = Home.clean(tag:gsub("[%[%]]", ""))
		if istable(v) and v.verdict == "fake" and tonumber(v.real) then row.p = math.min(row.p, math.max(0, math.floor(tonumber(v.real)))) row.hr = row.p end
		if istable(v) and v.verdict == "unverified" then row.hp = 40 end
	end
	-- The ping: Steam's measurement (s.ping), else the one the row had (keep:
	-- { ms, kind }), else a hint (an index row).
	if keep then
		row.ms, row.ph = keep[1], keep[2]
	elseif s.ping == nil then
		local ms, from = BrowseLogic.pingHint(pings(), s, today(), idx.lat, idx.lon)
		row.ms = ms or BrowseLogic.UNKNOWN_PING
		row.ph = from == "seen" and HINT_SEEN or from == "distance" and HINT_DISTANCE or HINT_NONE
	end
	if tab == browse.tab then browse.ping[s.address] = { row.ms, row.ph } end
	return row
end

local function browseSend(batch, done, now)
	pnlMainMenu:Call("if (window.MeowyBrowse) window.MeowyBrowse.add(" .. browse.gen .. ", "
		.. (#batch > 0 and Home.encode(batch) or "[]") .. ", " .. tostring(done == true) .. ", "
		.. string.format("%.1f", now - browse.started) .. ", " .. browse.stage .. ");")
end

-- Rows the page already has, sent again with new numbers (same row numbers).
local function browseUpdate(rows)
	pnlMainMenu:Call("if (window.MeowyBrowse && window.MeowyBrowse.update) window.MeowyBrowse.update(" .. browse.gen .. ", " .. Home.encode(rows) .. ");")
end

-- The index's rows, at once, in one batch. Servers that didn't answer the
-- index's own queries lately are left for Steam to list if they answer it.
local function browseFromIndex(now)
	browse.index = "shown"
	local batch = {}
	for _, s in ipairs(idx.rows) do
		if browse.count >= BROWSE_MAX then browse.full = true break end
		local address = s.address
		if not s.quiet and Home.publicAddress(address) and not browse.seen[address] then
			browse.seen[address] = true
			if blacklisted(s) then
				browse.blocked = browse.blocked + 1
			else
				browse.count = browse.count + 1
				local row = browseRow(s, "internet")
				batch[#batch + 1] = row
				browse.byAddr[address], browse.fromIndex[address], browse.known[address] = row.i, true, s
			end
		end
	end
	browse.indexCount = #batch
	-- The list is complete if Steam has already finished (it's the slower one).
	browseSend(batch, not browse.id and not browse.stock, now)
	browseSource()
	log("browse: %d servers from Meowy's index (%d s old) in %.2f s", #batch, indexAge(), now - browse.started)
end

-- A newer index while the list is open: rows whose numbers changed are sent
-- again with the ping they have (Steam's or a hint), new servers are added.
local function browseIndexRefresh(now)
	local updates, added = {}, {}
	for _, s in ipairs(idx.rows) do
		local address = s.address
		local i, old = browse.byAddr[address], browse.known[address]
		if i and old then
			if BrowseLogic.changed(old, s) then
				updates[#updates + 1] = browseRow(s, "internet", i, browse.ping[address])
				browse.known[address] = s
			end
		elseif not i and not s.quiet and not browse.seen[address] and Home.publicAddress(address) and browse.count < BROWSE_MAX then
			browse.seen[address] = true
			if not blacklisted(s) then
				browse.count = browse.count + 1
				local row = browseRow(s, "internet")
				added[#added + 1] = row
				browse.byAddr[address], browse.fromIndex[address], browse.known[address] = row.i, true, s
			end
		end
	end
	if #updates > 0 then browseUpdate(updates) end
	if #added > 0 then browseSend(added, false, now) end
	browseSource()
	log("browse: index refreshed, %d changed, %d new", #updates, #added)
end

local function browsePump(now)
	if not browse.id and not browse.stock then return end
	local list, finished
	if browse.id then
		local got
		got, finished = meowy.ServerQueryTake(browse.id)
		list = {}
		for _, s in ipairs(istable(got) and got or {}) do list[#list + 1] = fromNative(s) end
	else
		list, finished = browse.stock.queue, browse.stock.done
		browse.stock.queue = {}
	end
	local batch, updates, lan, day = {}, {}, browse.tab == "lan", today()
	for _, s in ipairs(list) do
		local address = s.address
		local fits = lan and ipv4(address) or (not lan and Home.publicAddress(address))
		local ms = tonumber(s.ping)
		if fits and not lan and ms and ms > 0 then
			BrowseLogic.remember(pings(), address, ms, day)
			pingDirty = true
		end
		if fits and browse.fromIndex[address] then
			-- Steam's answer for a row the index gave: the same row, Steam's numbers.
			browse.fromIndex[address] = nil
			updates[#updates + 1] = browseRow(s, browse.tab, browse.byAddr[address])
		elseif browse.count >= BROWSE_MAX then
			finished, browse.full = true, true
			break
		elseif fits and not browse.seen[address] then
			browse.seen[address] = true
			if blacklisted(s) then
				browse.blocked = browse.blocked + 1
			else
				browse.count = browse.count + 1
				local row = browseRow(s, browse.tab)
				batch[#batch + 1] = row
				browse.byAddr[address] = row.i
			end
		end
	end
	-- Internet: the servers with players, then the empty ones; with the index
	-- shown, it already listed the empty ones, so Steam's first stage is enough.
	local twoStages = browse.tab == "internet" and browse.index ~= "shown"
	local done = finished and (not twoStages or browse.stage == 2 or browse.full)
	if finished and not done then
		browseStop()
		if not browseQuery(2) then done = true end
	end
	if #updates > 0 then browseUpdate(updates) end
	if #batch > 0 or finished then browseSend(batch, done, now) end
	if done then
		local answered = browse.indexCount
		for _ in pairs(browse.fromIndex) do answered = answered - 1 end
		log("browse: %d %s servers in %.1f s (%d on Garry's Mod's blacklist%s)", browse.count, browse.tab, now - browse.started, browse.blocked,
			browse.index == "shown" and string.format("; %d from the index, %d of them answered Steam", browse.indexCount, answered) or "")
		browseStop()
	end
end

-- The index's fetch, when it finishes: the list waiting for it gets its rows
-- (or goes on with Steam alone); an open list gets the changes.
local function indexTake(now)
	if not idx.fetching then return end
	local status, body, geo, secs = meowy.ServerIndexTake()
	if not status then return end
	idx.fetching = false
	if status == "ok" then
		local rows, at = BrowseLogic.indexRows(util.JSONToTable(body))
		if rows then
			idx.rows, idx.at, idx.got = rows, at, now
		else
			status = "error: " .. tostring(at)
		end
	elseif status == "same" and idx.rows then
		idx.got = now
	end
	local lat, lon = BrowseLogic.parseGeo(geo)
	if lat then idx.lat, idx.lon = lat, lon end
	idx.last = string.format("%s in %.2f s%s", status, tonumber(secs) or 0, idx.rows and string.format(", %d servers, %d s old", #idx.rows, indexAge()) or "")
	log("browse: Meowy's index: %s", idx.last)
	local usable = (status == "ok" or status == "same") and indexFresh(now)
	if browse.tab ~= "internet" or not (IsValid(pnlMainMenu) and pnlMainMenu.Call) then return end
	if browse.index == "waiting" then
		if usable then
			browseFromIndex(now)
		else
			browse.index = "failed"
			browseSource()
			-- Steam's list wasn't available either: nothing more is coming.
			if not browse.id and not browse.stock then browseSend({}, true, now) end
		end
	elseif browse.index == "shown" and usable and status == "ok" then
		browseIndexRefresh(now)
	end
end

local function browseAddress(v)
	local r = browse.rows[math.floor(tonumber(v) or 0)]
	if not r then return nil end
	if r.tab == "lan" or r.tab == "found" then return ipv4(r.address) and r.address or nil end
	return Home.publicAddress(r.address) and r.address or nil
end

-- Find by address (Favourites): "host" (its usual ports, as Garry's Mod's own
-- find does) or "host:port", each asked once; what answers goes to the page.
local function browseFind(text)
	text = tostring(text or ""):match("^%s*(.-)%s*$")
	local host, port = text:match("^([%w%.%-]+):(%d+)$")
	local candidates = {}
	if host then
		candidates[1] = host .. ":" .. port
	elseif text:match("^[%w%.%-]+$") then
		for k = 0, 5 do candidates[#candidates + 1] = text .. ":" .. (27015 + k) candidates[#candidates + 1] = text .. ":" .. (26900 + k) end
	end
	browse.findGen = (browse.findGen or 0) + 1
	local gen, left, found = browse.findGen, #candidates, {}
	local function send()
		if gen ~= browse.findGen or not IsValid(pnlMainMenu) then return end
		pnlMainMenu:Call("if (window.MeowyBrowse) window.MeowyBrowse.found(" .. gen .. ", " .. (#found > 0 and Home.encode(found) or "[]")
			.. ", " .. tostring(left <= 0) .. ");")
	end
	if #candidates == 0 or not (serverlist and isfunction(serverlist.PingServer)) then left = 0 send() return end
	for _, address in ipairs(candidates) do
		serverlist.PingServer(address, function(ping, name, desc, map, players, maxplayers, botplayers, pass, lastplayed, addr, gm, workshopid, isAnon, netVersion, luaVersion, loc, gmcat)
			left = left - 1
			if name and ipv4(addr) and gen == browse.findGen then
				found[#found + 1] = browseRow({ address = addr, name = name, desc = desc, map = map, players = players, max = maxplayers,
					bots = botplayers, ping = ping, pass = pass == true, lastplayed = lastplayed, anon = isAnon == true, version = netVersion,
					gm = gm, ws = workshopid, loc = loc, cat = gmcat }, "found")
			end
			send()
		end)
	end
end

local browsePassword   -- { i, password } for the next join of that row (never logged)

-- The page's messages: kind -> handler(value). Anything else is ignored.
local handlers = {
	browse = function(v) browseStart(v) end,
	browse_stop = function() browseStop() end,
	-- Parties (party.lua): "create", "leave", "invite" (Steam's dialog), "stay".
	party = function(v)
		if MeowyClientPackParty and (v == "create" or v == "leave" or v == "invite" or v == "stay") then MeowyClientPackParty.action(v) end
	end,
	-- A friend from the party card's list, by number: invited.
	party_friend = function(v) if MeowyClientPackParty then MeowyClientPackParty.inviteFriend(v) end end,
	-- A member's server, by number: joined (the player's click).
	party_member = function(v) if MeowyClientPackParty then MeowyClientPackParty.joinMember(v) end end,
	-- The leader's "Take the party here" on a server's details: the row's server.
	party_go = function(v)
		local address = browseAddress(v)
		local r = browse.rows[math.floor(tonumber(v) or 0)]
		if address and Home.publicAddress(address) and MeowyClientPackParty then
			browseStop()
			MeowyClientPackParty.go(address, r and r.name or "")
		end
	end,
	-- The page, once a minute while the Internet list from the index is open: a newer index.
	browse_refresh = function()
		if browse.tab == "internet" and browse.index == "shown" then indexFetch() end
	end,
	browse_join = function(v)
		local address = browseAddress(v)
		if not address then return end
		log("browse: joining %s", address)
		browseStop()
		if browsePassword and browsePassword.i == math.floor(tonumber(v) or 0) then
			RunConsoleCommand("password", browsePassword.password)
		end
		browsePassword = nil
		join(address)
	end,
	-- "12:secret": the password for row 12's next join.
	browse_pass = function(v)
		local i, password = tostring(v or ""):match("^(%d+):(.*)$")
		if not i or #password > 64 or password:find("[%c\"]") then return end
		browsePassword = { i = tonumber(i), password = password }
	end,
	browse_intent = function(v)
		local address = browseAddress(v)
		if address and Home.publicAddress(address) and MeowyClientPackWarm then MeowyClientPackWarm(address, "pointer on its row in Find servers") end
	end,
	-- A server picked: its players (the honest list's own query, which reads
	-- lists some servers hide from Garry's Mod's) and its numbers again. The
	-- page asks every 10 seconds while it stays picked; at most every 2 here.
	browse_select = function(v)
		local address = browseAddress(v)
		if not address then return end
		local now = SysTime()
		if browse.pinged[address] and now - browse.pinged[address] < 2 then return end
		browse.pinged[address] = now
		if isfunction(GetPlayerList) then GetPlayerList(address) end
		if isfunction(PingServer) then PingServer(address) end
		if Home.publicAddress(address) and MeowyClientPackWarm then MeowyClientPackWarm(address, "picked in Find servers") end
	end,
	-- The star: a Steam favourite (Garry's Mod's Favourites list), on or off.
	browse_fav = function(v)
		local address = browseAddress(v)
		if not address or not serverlist or not isfunction(serverlist.IsServerFavorite) then return end
		if serverlist.IsServerFavorite(address) then serverlist.RemoveServerFromFavorites(address) else serverlist.AddServerToFavorites(address) end
	end,
	-- Right-click or Copy: the address.
	browse_copy = function(v)
		local address = browseAddress(v)
		if address and SetClipboardText then SetClipboardText(address) end
	end,
	-- Install: subscribe to a gamemode's Workshop item, only one a listed server named.
	browse_install = function(v)
		local ws = tostring(v or "")
		if not browse.ws[ws] or not steamworks or not isfunction(steamworks.Subscribe) then return end
		log("browse: subscribing to gamemode %s", ws)
		steamworks.Subscribe(ws)
	end,
	-- A gamemode's Workshop page, only one a listed server named.
	browse_workshop = function(v)
		local ws = tostring(v or "")
		if browse.ws[ws] and gui and isfunction(gui.OpenURL) then gui.OpenURL("https://steamcommunity.com/sharedfiles/filedetails/?id=" .. ws) end
	end,
	browse_find = function(v) browseFind(v) end,
	-- The browser's sort, categories and filters, kept between starts (JSON the page reads back).
	browse_prefs = function(v)
		if isstring(v) and v:sub(1, 1) == "{" and not v:find("[%c]") and cookie and isfunction(cookie.Set) then cookie.Set(BROWSE_PREFS, v) end
	end,
	join = function(v) MeowyClientPackHomeJoin(tonumber(v)) end,
	intent = function(v) MeowyClientPackHomeIntent(tonumber(v)) end,
	-- A quoted key: Garry's Mod's Lua has `continue` as a keyword.
	["continue"] = function() MeowyClientPackContinue() end,
	options = function() if RunGameUICommand then RunGameUICommand("OpenOptionsDialog") end end,
	settings = function() MeowyClientPackOpenSettings() end,
	quit = function() if RunGameUICommand then RunGameUICommand("QuitNoConfirm") end end,
	stock = function()
		hatch = true
		log("menu page: Garry's Mod's menu for the rest of this start")
	end,
}

-- Values are short, but for these: a password, an address to find, the browser's settings.
local LONG_VALUES = { browse_pass = 90, browse_find = 64, browse_prefs = 600 }

local function post(kind, value)
	if not menuPage.ours then return end   -- only while our home shows
	if not isstring(kind) or #kind > 16 then return end
	local h = handlers[kind]
	if not h then return end
	if value ~= nil and not isnumber(value) and not (isstring(value) and #value <= (LONG_VALUES[kind] or 16)) then return end
	h(value)
end

-- The subscribed addons, for the home's search (shell.js): titles and IDs as
-- JSON, sent when the list changes or the page has reloaded, not every second.
local ADDONS_MAX, ADDONS_LOOK = 2000, 10   -- addons, seconds between looks
local addonsSent, addonsAt, addonsText = nil, -math.huge, nil
local function sendAddons(now)
	if now - addonsAt >= ADDONS_LOOK or not addonsText then
		addonsAt = now
		local list = {}
		local ok, addons = pcall(engine and engine.GetAddons or function() return {} end)
		for _, a in ipairs(ok and istable(addons) and addons or {}) do
			if #list >= ADDONS_MAX then break end
			if isstring(a.title) and a.title ~= "" then
				list[#list + 1] = { t = a.title:sub(1, 120), w = tostring(a.wsid or ""), m = a.mounted == true }
			end
		end
		addonsText = #list > 0 and Home.encode(list) or "[]"   -- a JavaScript literal, strings escaped
	end
	if addonsText == addonsSent then return end
	pnlMainMenu:Call("window.MeowyAddons = " .. addonsText .. ";\nif (window.MeowyShellLists) window.MeowyShellLists();\n")
	addonsSent = addonsText
end

local function tick()
	local now = SysTime()
	if now < nextLook then return end
	nextLook = now + 0.25
	local html = IsValid(pnlMainMenu) and pnlMainMenu.HTML or nil
	if html and html ~= bridged and IsValid(html) and html.AddFunction then
		bridged = html   -- a new menu panel: the bridge goes on it once
		html:AddFunction("meowy", "post", post)
		-- A page that's just (re)loaded (the game starting, a language change) is
		-- Garry's Mod's own until the script arrives: send it on the next frame,
		-- not up to a second later, so the stock list and news don't flash.
		local ready = html.OnDocumentReady
		html.OnDocumentReady = function(self, url, ...)
			sent, zoomSent, nextLook, addonsSent, loaded = nil, nil, 0, nil, nil
			if ready then return ready(self, url, ...) end
		end
		loaded = nil
	end
	-- Turned on again: try again, even after "Use Garry's Mod's menu".
	local on = menuOn()
	if on and not wasOn then hatch = false end
	wasOn = on

	local want = wanted()
	menuPage.ours = want
	if not (IsValid(pnlMainMenu) and pnlMainMenu.Call) then return end
	indexTake(now)
	if browse.index == "waiting" and not idx.fetching and want then
		-- Fetched recently enough to use as it is (nothing was asked), or the ask didn't start.
		if indexFresh(now) then browseFromIndex(now) else browse.index = "failed" browseSource() end
	end
	if browse.id or browse.stock then
		if want then browsePump(now) else browseStop() end   -- in game or switched off: stop asking Steam
	end

	-- The page's zoom (meowy_menu_scale), on whatever it shows. Sent again every
	-- few seconds: the page may have been reloaded (a language change does).
	local zoom = Home.menuZoom(menuScale(), ScrH())
	if zoom ~= zoomSent or (zoom ~= 1 and now - zoomAt >= ZOOM_RESEND) then
		if zoom ~= 1 or zoomSent then
			pnlMainMenu:Call("document.documentElement.style.zoom = '" .. string.format("%.2f", zoom) .. "';")
		end
		if zoom ~= zoomSent then log("menu page: zoom %s", string.format("%.2f", zoom)) end
		zoomSent, zoomAt = zoom, now
	end
	if want ~= sent or (want and now - sentAt >= REFRESH) or (not loaded and shellJs() ~= "") then
		-- Sent to every page once, the home off too: the script also adds Find
		-- servers' Friends tab. After that, while off, there's nothing to send.
		if want or sent or (not loaded and shellJs() ~= "") then
			pnlMainMenu:Call(shellJs() .. "\nwindow.MeowyShell(" .. tostring(want) .. ");\n")
			loaded = shellJs() ~= ""
		end
		if want then sendAddons(now) end
		if want ~= sent then log("menu page: %s", want and "our home is on" or "Garry's Mod's main screen") end
		sent, sentAt = want, now
	end
end

hook.Add("Think", "MeowyClientPack.MenuPage", function()
	local ok, err = pcall(tick)
	if not ok and not failedTick then
		failedTick = true
		ErrorNoHalt("[Meowy Client Pack] menu page: " .. tostring(err) .. "\n")
	end
end)

if concommand then
	concommand.Add("meowy_menu_status", function()
		log("menu page: %s, switch %s%s", menuPage.ours and "our home" or "Garry's Mod's main screen",
			menuOn() and "on" or "off", hatch and " (Garry's Mod's menu chosen for this start)" or "")
	end, nil, "Meowy Client Pack: which main menu is showing, and why")
	concommand.Add("meowy_server_index_status", function()
		local n = 0
		for _ in pairs(pings()) do n = n + 1 end
		log("server index: %s; last: %s; %s; %d networks with a remembered ping%s",
			indexWanted() and "on" or "off", idx.last, meowy.ServerIndexState and meowy.ServerIndexState() or "unavailable", n,
			idx.lat and string.format("; located near %.1f, %.1f (IP Geolocation by DB-IP)", idx.lat, idx.lon) or "")
	end, nil, "Meowy Client Pack: Meowy's server index for Find servers, and the ping hints")
end
