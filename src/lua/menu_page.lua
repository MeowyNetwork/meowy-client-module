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

-- Find servers on the home screen (U6, src/menu/browser.js): every server
-- straight from Steam (server_query.h), the ones with players first, then the
-- empty ones, sent to the page in batches as they answer. Servers on Garry's
-- Mod's own blacklist (mainmenu.lua's IsServerBlacklisted) are left out, as its
-- browser leaves them out. The addresses stay here; the page asks for a server
-- by its number. Every server string goes to the page as an escaped string and
-- is shown as text.
local BROWSE_MAX = 30000
local BROWSE_CATS = { rp = true, pvp = true, pve = true, other = true, none = true }
local browse = { gen = 0, rows = {}, seen = {}, ws = {} }
local function tagValue(tags, key) return (" " .. (tags or "") .. " "):match(" " .. key .. ":(%S+)") end

local function browseStop()
	if browse.id and meowy.ServerQueryCancel then meowy.ServerQueryCancel(browse.id) end
	browse.id = nil
end

-- Stage 1: servers with players; 2: the empty ones.
local function browseQuery(stage)
	browse.stage = stage
	browse.id = meowy.ServerQueryStart
		and meowy.ServerQueryStart("internet", "gamedir", "garrysmod", stage == 1 and "hasplayers" or "noplayers", "1") or nil
	return browse.id ~= nil
end

local function browseStart()
	browseStop()
	browse.gen = browse.gen + 1
	browse.rows, browse.seen, browse.ws, browse.blocked, browse.full, browse.started = {}, {}, {}, 0, false, SysTime()
	local ok = browseQuery(1)
	if IsValid(pnlMainMenu) and pnlMainMenu.Call then
		pnlMainMenu:Call("if (window.MeowyBrowse) window.MeowyBrowse.reset(" .. browse.gen .. ", " .. tostring(ok) .. ");")
	end
	log("browse: %s", ok and "listing servers" or "Steam's server list isn't available")
end

local function blacklisted(s, gm)
	if not isfunction(IsServerBlacklisted) then return false end
	local ok, hit = pcall(IsServerBlacklisted, s.address, s.name or "", s.desc or "", gm, s.map or "")
	return ok and hit ~= nil
end

-- A server as the page reads it (browser.js's server()).
local function browseRow(i, s, gm)
	local bots = math.max(0, tonumber(s.bots) or 0)
	local ws = tagValue(s.tags, "gmws")
	ws = ws and #ws <= 20 and ws:match("^%d+$") and ws ~= "0" and ws or ""
	if ws ~= "" then browse.ws[ws] = true end
	local cat = tagValue(s.tags, "gmc") or ""
	local isFav = serverlist and serverlist.IsServerFavorite
	return {
		i = i, a = s.address, n = Home.clean(s.name), d = Home.clean(s.desc), m = Home.clean(s.map), g = Home.clean(gm),
		c = BROWSE_CATS[cat] and cat or "", ws = ws, f = (tagValue(s.tags, "loc") or ""):match("^%a+$") or "",
		v = math.floor(tonumber(s.version) or 0),   -- the network version, as the game's list compares it
		p = math.max(0, (tonumber(s.players) or 0) - bots), x = math.max(0, (tonumber(s.max) or 0) - bots), b = bots,
		ms = tonumber(s.ping) or 0, pw = s.pass == true, an = s.anon == true, lp = tonumber(s.lastplayed) or 0,
		fv = isfunction(isFav) and isFav(s.address) == true or false,
	}
end

local function browsePump(now)
	if not browse.id then return end
	local list, finished = meowy.ServerQueryTake(browse.id)
	local batch = {}
	for _, s in ipairs(istable(list) and list or {}) do
		if #browse.rows >= BROWSE_MAX then finished, browse.full = true, true break end
		local address = s.address
		if Home.publicAddress(address) and not browse.seen[address] then
			browse.seen[address] = true
			local gm = tagValue(s.tags, "gm") or ""
			if blacklisted(s, gm) then
				browse.blocked = browse.blocked + 1
			else
				local i = #browse.rows + 1
				browse.rows[i] = address
				batch[#batch + 1] = browseRow(i, s, gm)
			end
		end
	end
	local done = finished and (browse.stage == 2 or browse.full)
	if finished and not done then
		-- The servers with players are in; now the empty ones.
		browseStop()
		if not browseQuery(2) then done = true end
	end
	if #batch > 0 or finished then
		pnlMainMenu:Call("if (window.MeowyBrowse) window.MeowyBrowse.add(" .. browse.gen .. ", "
			.. (#batch > 0 and Home.encode(batch) or "[]") .. ", " .. tostring(done == true) .. ", "
			.. string.format("%.1f", now - browse.started) .. ", " .. browse.stage .. ");")
	end
	if done then
		log("browse: %d servers in %.1f s (%d on Garry's Mod's blacklist)", #browse.rows, now - browse.started, browse.blocked)
		browseStop()
	end
end

-- The page's messages: kind -> handler(value). Anything else is ignored.
local handlers = {
	browse = function() browseStart() end,
	browse_stop = function() browseStop() end,
	browse_join = function(v)
		local address = browse.rows[math.floor(tonumber(v) or 0)]
		if not Home.publicAddress(address) then return end
		log("browse: joining %s", address)
		browseStop()
		join(address)
	end,
	browse_intent = function(v)
		local address = browse.rows[math.floor(tonumber(v) or 0)]
		if Home.publicAddress(address) and MeowyClientPackWarm then MeowyClientPackWarm(address, "pointer on its row in Find servers") end
	end,
	-- The star: a Steam favourite (Garry's Mod's Favourites list), on or off.
	browse_fav = function(v)
		local address = browse.rows[math.floor(tonumber(v) or 0)]
		if not Home.publicAddress(address) or not serverlist or not isfunction(serverlist.IsServerFavorite) then return end
		if serverlist.IsServerFavorite(address) then serverlist.RemoveServerFromFavorites(address) else serverlist.AddServerToFavorites(address) end
	end,
	-- Right-click: the address, copied.
	browse_copy = function(v)
		local address = browse.rows[math.floor(tonumber(v) or 0)]
		if Home.publicAddress(address) and SetClipboardText then SetClipboardText(address) end
	end,
	-- Install: subscribe to a gamemode's Workshop item, only one a listed server named.
	browse_install = function(v)
		local ws = tostring(v or "")
		if not browse.ws[ws] or not steamworks or not isfunction(steamworks.Subscribe) then return end
		log("browse: subscribing to gamemode %s", ws)
		steamworks.Subscribe(ws)
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

local function post(kind, value)
	if not menuPage.ours then return end   -- only while our home shows
	if not isstring(kind) or #kind > 16 then return end
	local h = handlers[kind]
	if not h then return end
	if value ~= nil and not isnumber(value) and not (isstring(value) and #value <= 16) then return end
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
	if browse.id then
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
end
