-- Our own main menu page (U5 shell, preview; meowy_menu 1). Runs in its own
-- function at the end of resilience.lua's chunk (module.cpp), with its locals in
-- scope: log, join, menuOn, setMenu, menuPage.
--
-- * Out of game, the menu's browser shows our page (src/menu/home.html, written
--   by the DLL to garrysmod/html/meowy/home.html and opened as asset://, like
--   the stock page: a page set from a string gets no bindings) instead of
--   asset://garrysmod/html/menu.html.
--   Garry's Mod's own Lua keeps calling its page functions; ours defines them
--   all as no-ops. The background slideshow is drawn behind the page by Lua,
--   as before.
-- * The page reaches Lua only through meowy.post(kind, value): a fixed table of
--   message types, each checking its value. While our page is up, the stock
--   lua.Run (RunString of whatever the page sends) is switched off.
-- * Watchdog: no "ready" within 3 s, and the stock page comes back; twice, and
--   this start stays on the stock page.
-- * Servers, New game, Addons, Dupes, Saves and Demos open the stock page at
--   that screen; back on the stock page's main screen, our page returns. In game
--   (and the Escape menu) it's the stock page, with the Escape card.
local STOCK = "asset://garrysmod/html/menu.html"
local OURS = "asset://garrysmod/html/meowy/home.html"   -- written there by the DLL (meowy.MenuPageUrl)
local SCREENS = { servers = "servers", newgame = "newgame", addons = "addons", dupes = "dupes", saves = "saves", demos = "demos" }
local READY_WAIT = 3     -- seconds for our page to say it's ready
local MAX_FAILS = 2      -- failed loads before this start stays on the stock page

local page = {
	html = nil,          -- the menu's browser panel the bridge is on
	state = "stock",     -- "stock" | "loading" | "ours"
	why = nil,           -- on the stock page by choice: "screen", "hatch", "game", "failed"
	deadline = 0,
	fails = 0,
	savedRun = nil,      -- the stock lua.Run, while ours is up
	blocked = 0,         -- lua.Run calls dropped while ours was up
}

local function browser()
	local html = IsValid(pnlMainMenu) and pnlMainMenu.HTML or nil
	if html and IsValid(html) and html.Callbacks and html.AddFunction then return html end
end

-- lua.Run off while our page is up; back on for the stock page.
local function guardRun(on)
	local html = page.html
	if not (html and html.Callbacks) then return end
	if on and not page.savedRun then
		page.savedRun = html.Callbacks["lua.Run"]
		html.Callbacks["lua.Run"] = function()
			page.blocked = page.blocked + 1
			if page.blocked == 1 then log("menu page: a lua.Run call on our page was dropped") end
		end
	elseif not on and page.savedRun then
		html.Callbacks["lua.Run"] = page.savedRun
		page.savedRun = nil
	end
end

-- A reopened stock page knows only what it asks for itself (news, server
-- lists). What Garry's Mod's Lua pushed to it at start (mainmenu.lua's
-- timer.Simple(0): version, language, then GameContentChanged for gamemodes,
-- maps and games) is pushed again, and the panel's remembered in-game and
-- favourite state is forgotten so its Paint sends SetInGame and the favourite
-- button again. Calls made now wait in the browser's queue until the page loads.
local function replayStock()
	local p = pnlMainMenu
	if not (IsValid(p) and p.Call) then return end
	if isstring(VERSIONSTR) and isstring(NETVERSIONSTR) and isstring(BRANCH) then
		p:Call("UpdateVersion( '" .. VERSIONSTR:JavascriptSafe() .. "', '" .. NETVERSIONSTR:JavascriptSafe() .. "', '" .. BRANCH:JavascriptSafe() .. "' )")
	end
	if isfunction(LanguageChanged) then LanguageChanged(GetConVarString("gmod_language")) end
	if hook.Run then hook.Run("GameContentChanged") end
	p.IsInGame, p.CanAddServerToFavorites, p.IsCurrentServerFavorite = nil, nil, nil
end

local function openStock(why, screen)
	local html = browser()
	if not html then return end
	guardRun(false)
	page.state, page.why = "stock", why
	menuPage.ours = false
	html:OpenURL(STOCK .. (screen and ("#/" .. screen .. "/") or ""))
	local ok, err = pcall(replayStock)
	if not ok then log("menu page: couldn't refill Garry's Mod's page: %s", tostring(err)) end
end

local function openOurs(now)
	local html = browser()
	local url = meowy.MenuPageUrl and meowy.MenuPageUrl()
	if not html or url ~= OURS then return end
	page.state, page.why, page.deadline = "loading", nil, now + READY_WAIT
	guardRun(true)
	html:OpenURL(url)
end

-- The page's messages: kind -> handler(value). Anything else is ignored.
local handlers = {
	ready = function()
		if page.state ~= "loading" then return end
		page.state = "ours"
		menuPage.ours = true
		log("menu page: ours is up")
	end,
	join = function(v) MeowyClientPackHomeJoin(tonumber(v)) end,
	-- A quoted key: Garry's Mod's Lua has `continue` as a keyword.
	["continue"] = function() MeowyClientPackContinue() end,
	open = function(v)
		local screen = isstring(v) and SCREENS[v]
		if screen then openStock("screen", screen) end
	end,
	options = function() if RunGameUICommand then RunGameUICommand("OpenOptionsDialog") end end,
	settings = function() MeowyClientPackOpenSettings() end,
	quit = function() if RunGameUICommand then RunGameUICommand("QuitNoConfirm") end end,
	stock = function() openStock("hatch") end,
}

local function post(kind, value)
	if page.state == "stock" then return end   -- only our page may post
	if not isstring(kind) or #kind > 16 then return end
	local h = handlers[kind]
	if not h then return end
	if value ~= nil and not isnumber(value) and not (isstring(value) and #value <= 16) then return end
	h(value)
end

-- The stock page tells us when it's back on its main screen (via its own lua.Run).
function MeowyClientPackStockHome()
	if page.state == "stock" and page.why == "screen" then page.why = nil end
end
local STOCK_HOME_JS = [==[
(function () {
	var h = location.hash || '';
	if ((h === '' || h === '#/' || h === '#') && document.querySelector('.page[ng-controller="ControllerMain"]')) lua.Run('MeowyClientPackStockHome()');
})();
]==]

local nextLook, failedTick, wasOn = 0, false, false
local function tick()
	local now = SysTime()
	if now < nextLook then return end
	nextLook = now + 0.25
	local html = browser()
	if not html then return end
	-- Turned on again: try again, even after "Use Garry's Mod's menu" or failed loads.
	local on = menuOn()
	if on and not wasOn and page.state == "stock" then page.why, page.fails = nil, 0 end
	wasOn = on
	if html ~= page.html then
		-- A new menu panel: the bridge goes on it once.
		page.html, page.state, page.savedRun = html, "stock", nil
		html:AddFunction("meowy", "post", post)
	end
	local inGame = IsInGame() or IsInLoading()
	if page.state ~= "stock" and (inGame or not menuOn()) then
		openStock(inGame and "game" or nil)
		return
	end
	if page.state == "loading" and now > page.deadline then
		page.fails = page.fails + 1
		log("menu page: ours didn't say it was ready within %d s (%d of %d); Garry's Mod's page is back", READY_WAIT, page.fails, MAX_FAILS)
		openStock("failed")
		return
	end
	if page.state ~= "stock" or inGame or not menuOn() or page.fails >= MAX_FAILS then return end
	if page.why == "hatch" then return end   -- the player asked for the stock menu this start
	if page.why == "screen" then
		if pnlMainMenu.Call then pnlMainMenu:Call(STOCK_HOME_JS) end
		return
	end
	if page.why == "failed" and page.fails >= MAX_FAILS then return end
	openOurs(now)
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
		log("menu page: %s%s, failed loads %d, lua.Run calls dropped %d, switch %s", page.state,
			page.why and (" (" .. page.why .. ")") or "", page.fails, page.blocked, menuOn() and "on" or "off")
	end, nil, "Meowy Client Pack: which main menu page is showing, and why")
end
