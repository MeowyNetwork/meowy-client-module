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

-- The page's messages: kind -> handler(value). Anything else is ignored.
local handlers = {
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

local function tick()
	local now = SysTime()
	if now < nextLook then return end
	nextLook = now + 0.25
	local html = IsValid(pnlMainMenu) and pnlMainMenu.HTML or nil
	if html and html ~= bridged and IsValid(html) and html.AddFunction then
		bridged = html   -- a new menu panel: the bridge goes on it once
		html:AddFunction("meowy", "post", post)
	end
	-- Turned on again: try again, even after "Use Garry's Mod's menu".
	local on = menuOn()
	if on and not wasOn then hatch = false end
	wasOn = on

	local want = wanted()
	menuPage.ours = want
	if not (IsValid(pnlMainMenu) and pnlMainMenu.Call) then return end

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
	if want ~= sent or (want and now - sentAt >= REFRESH) then
		-- When off, the script may never have been sent: then there's nothing to take out.
		if want or sent then
			pnlMainMenu:Call(shellJs() .. "\nwindow.MeowyShell(" .. tostring(want) .. ");\n")
		end
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
