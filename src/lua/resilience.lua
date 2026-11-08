-- Connection resilience, menu state. Runs with `Logic` (resilience_logic.lua)
-- in scope: module.cpp joins the two files into one chunk.
--
-- * Asks once: "Stay connected through drops?" Yes saves the player's cl_timeout,
--   sets it to 60 and turns on automatic rejoin; No changes nothing. Both are kept
--   in %LOCALAPPDATA%\MeowyClientPack\resilience.txt. meowy_settings opens a
--   window to change it; turning it off puts the player's cl_timeout back.
-- * Automatic rejoin, only to the Meowy Network server the game was really
--   connected to (the engine's own address, meowy.ResilienceWatch), and only
--   when the engine says the server had gone silent before the connection
--   ended (IsTimingOut and 10+ s since a packet, sampled every frame), or our
--   server had said it was restarting (meowy.ExpectRestart) and then went
--   down (stopped answering within 15 s; one that keeps answering wasn't
--   restarting: the player left, was kicked or joined elsewhere). Never from text.
--   Only after the player had got into the game there. The server is asked
--   whether it's up (A2S, serverlist.PingServer) before each join. A small
--   window shows each try and has Cancel.
-- * After a crash while on our server, the next start offers "Rejoin Meowy
--   Network?"; after a crash on any other public server, "Rejoin the server?"
--   with its name (the engine's address; the name only as text). Neither ever
--   joins without the click; automatic rejoin stays Meowy-only.
-- * The main menu's list gets "Continue" with the last server the player got
--   into the game on, any server (kept natively, ours or a public address
--   only); one click joins it. Off in Meowy settings: none is kept.
local meowy, Logic = meowy, Logic

local settings = Logic.parseSettings(meowy.ResilienceSettingsRead())
local function save() meowy.ResilienceSettingsWrite(Logic.formatSettings(settings)) end

local function clTimeout()
	local cv = GetConVar and GetConVar("cl_timeout")
	if cv then return cv:GetString() end
	return GetConVarString("cl_timeout")
end

local function setEnabled(on)
	if on and not settings.enabled then
		local current = clTimeout()
		if Logic.validTimeout(current) and tonumber(current) ~= tonumber(Logic.TIMEOUT_CL) then
			settings.saved_cl_timeout = current
		end
		RunConsoleCommand("cl_timeout", Logic.TIMEOUT_CL)
	elseif not on and settings.enabled then
		local back = Logic.restoreTimeout(clTimeout(), settings.saved_cl_timeout)
		if back then RunConsoleCommand("cl_timeout", back) end
		settings.saved_cl_timeout = nil
	end
	settings.enabled, settings.asked = on and true or false, true
	save()
end

-- The main menu's "Continue": the last server the player got into the game on
-- (any server; native keeps it in last_server.txt, and only for ours or a
-- public address). Off: none is kept, and the one kept is forgotten.
local continueSetting = meowy.Setting and meowy.Setting("meowy_menu_continue", "1",
	"Meowy Client Pack: a Continue entry in the main menu that joins the last server you played on (0 = none, and none kept)")
local function continueOn() return continueSetting == nil or continueSetting:GetBool() end
local function setContinue(on)
	-- Through the console: Garry's Mod refuses ConVar:SetBool on these menu convars.
	RunConsoleCommand("meowy_menu_continue", on and "1" or "0")
	if not on and meowy.LastServerForget then meowy.LastServerForget() end
end

-- The main menu's Home card (home.lua): our server's status, favourites, recent.
local homeSetting = meowy.Setting and meowy.Setting("meowy_menu_home", "1",
	"Meowy Client Pack: a card on the main menu with Meowy Network, your favourites and recent servers (0 = none)")
local function homeOn() return homeSetting == nil or homeSetting:GetBool() end
local function setHome(on)
	-- Through the console: Garry's Mod refuses ConVar:SetBool on these menu convars.
	RunConsoleCommand("meowy_menu_home", on and "1" or "0")
end

-- Our own home screen (menu_page.lua), on by default since it left preview.
local menuSetting = meowy.Setting and meowy.Setting("meowy_menu", "1",
	"Meowy Client Pack: the Meowy home screen instead of Garry's Mod's main screen (0 = Garry's Mod's)")
local function menuOn() return menuSetting ~= nil and menuSetting:GetBool() end
local function setMenu(on)
	-- Through the console: Garry's Mod refuses ConVar:SetBool on these menu convars.
	RunConsoleCommand("meowy_menu", on and "1" or "0")
end
-- Every setting is saved as soon as any one changes, so while the home screen
-- was a preview (off by default) most players got "meowy_menu=0" written
-- without choosing it. Once, when it left preview: on, whatever was saved.
local menuVersion = meowy.Setting and meowy.Setting("meowy_menu_version", "0",
	"Meowy Client Pack: internal (which home screen default has been applied)")
if menuVersion and menuVersion:GetString() ~= "2" then
	RunConsoleCommand("meowy_menu", "1")
	RunConsoleCommand("meowy_menu_version", "2")
end
-- The menu page's text size (menu_page.lua): "0" auto, or a percentage.
local menuScaleSetting = meowy.Setting and meowy.Setting("meowy_menu_scale", "0",
	"Meowy Client Pack: the main menu's size in percent (0 = auto: 100% at 1080p, 133% at 1440p, 200% at 4K)")
local function menuScale() return menuScaleSetting and menuScaleSetting:GetString() or "0" end
local function setMenuScale(v)
	-- Through the console: Garry's Mod refuses ConVar setters on these menu convars.
	RunConsoleCommand("meowy_menu_scale", tostring(v))
end

-- Join pre-warm (home.lua, prewarm.h): read a server's files into memory
-- before you join it.
local prewarmSetting = meowy.Setting and meowy.Setting("meowy_prewarm", "1",
	"Meowy Client Pack: read a server's files into memory when you're about to join it (0 = off)")
local function prewarmOn() return prewarmSetting == nil or prewarmSetting:GetBool() end
-- A map's compressed files decompressed once per join, not on every read (pak_cache.h).
local pakCacheSetting = meowy.Setting and meowy.Setting("meowy_pak_cache", "1",
	"Meowy Client Pack: decompress a map's compressed files once while a join loads, not on every read (0 = Garry's Mod's way)")
local function pakCacheOn() return pakCacheSetting == nil or pakCacheSetting:GetBool() end
-- Full speed while a join loads, even with the game in the background (home.lua).
local joinBoostSetting = meowy.Setting and meowy.Setting("meowy_join_boost", "1",
	"Meowy Client Pack: while a join loads, keep the game at full speed even when it's in the background (0 = Windows decides)")
local function joinBoostOn() return joinBoostSetting == nil or joinBoostSetting:GetBool() end
local function setPrewarm(on)
	RunConsoleCommand("meowy_prewarm", on and "1" or "0")
	if not on and meowy.PrewarmCancel then meowy.PrewarmCancel() end
end
-- ...and bring its Workshop items up to date first (home.lua): a download, so
-- its own switch.
local prewarmWorkshopSetting = meowy.Setting and meowy.Setting("meowy_prewarm_workshop", "1",
	"Meowy Client Pack: update a server's Workshop items when you're about to join it (0 = off)")
local function prewarmWorkshopOn() return prewarmWorkshopSetting == nil or prewarmWorkshopSetting:GetBool() end
local function setPrewarmWorkshop(on)
	RunConsoleCommand("meowy_prewarm_workshop", on and "1" or "0")
end

-- Set by menu_page.lua while our home shows on the stock page (its list, with
-- the settings entry, is hidden then).
local menuPage = { ours = false }

-- The Escape menu's card (home.lua): the server you're on, Reconnect, quick volume and FOV.
local escapeSetting = meowy.Setting and meowy.Setting("meowy_menu_escape", "1",
	"Meowy Client Pack: a card in the Escape menu with the server you're on and quick settings (0 = none)")
local function escapeOn() return escapeSetting == nil or escapeSetting:GetBool() end
local function setEscape(on)
	-- Through the console: Garry's Mod refuses ConVar:SetBool on these menu convars.
	RunConsoleCommand("meowy_menu_escape", on and "1" or "0")
end

-- Discord status on Meowy Network (our server's Lua asks for it; discord_ipc.h).
-- Off: nothing reaches Discord, and what it shows is cleared at once.
-- Protected (settings.lua): a server can't turn it back on.
local discordSetting = meowy.Setting and meowy.Setting("meowy_discord", "1",
	"Meowy Client Pack: on Meowy Network, show your job, the player count and how long you've played in your Discord status (0 = off)")
local function discordOn() return discordSetting == nil or discordSetting:GetBool() end
local function applyDiscord(on) if meowy.DiscordAllow then meowy.DiscordAllow(on) end end
local function setDiscord(on)
	-- Through the console: Garry's Mod refuses ConVar:SetBool on these menu convars.
	RunConsoleCommand("meowy_discord", on and "1" or "0")
	applyDiscord(on)   -- at once: the command may only take effect at the next frame
end
-- At start, and whenever the setting changes some other way (the Meowy
-- console, the policy): only a change of the setting itself is applied, so a
-- click isn't undone by the frame before its command has run.
local discordSeen
hook.Add("Think", "MeowyClientPack.Discord", function()
	local on = discordOn()
	if on ~= discordSeen then
		discordSeen = on
		applyDiscord(on)
	end
end)

-- UI (ui.lua's kit) ----------------------------------------------------------------
local C, S = UI.C, UI.S

-- More of the General tab, from the chunks after this one (the settings guard):
-- functions that return blocks, given a function that draws the tab again.
local settingsSections = {}

-- The settings window: a left tab list (General, PC doctor) and the tab's
-- content on the right, scrolled. Escape or the "x" closes it.
local settingsWindow
local function openSettings(startTab)
	if IsValid(settingsWindow) then settingsWindow:Remove() end
	local back, card = UI.Modal(720)
	settingsWindow = back
	local W = card.w
	local headerH, nav, pad = S(64), S(184), S(24)
	local version = "v" .. tostring(meowy.VERSION or "")

	-- Header: the name, a version chip, the close button, a divider.
	local header = vgui.Create("DPanel", card)
	header:SetPos(0, 0)
	header:SetSize(W, headerH)
	header:SetMouseInputEnabled(false)
	header.Paint = function(self, w, h)
		draw.SimpleText("Meowy Client Pack", "MeowyPack.Title", pad, h / 2, C.text, UI.LEFT, UI.CENTER)
		local tw = UI.textW("Meowy Client Pack", "MeowyPack.Title")
		surface.SetFont("MeowyPack.Label")
		local _, ch = surface.GetTextSize("1")
		UI.chip(version, pad + tw + S(12), math.floor(h / 2 - (ch + S(8)) / 2))
		draw.RoundedBox(0, 0, h - 1, w, 1, C.divider)
		return true
	end
	local x = UI.Close(card, function() back:Dismiss() end)
	x:SetPos(W - S(18) - x.w, math.floor((headerH - x.w) / 2))

	-- PC doctor's findings, worked out once per opening (it asks Windows).
	local tips, report
	if DoctorUI then
		local ok, a, b = pcall(DoctorUI.diagnose)
		if ok then tips, report = a, b else ErrorNoHalt("[Meowy Client Pack] PC doctor: " .. tostring(a) .. "\n") end
	end

	local pages, show = {}, nil
	pages.general = function()
		local blocks = { UI.Label(nil, "Connection") }
		local t = UI.Toggle(nil, "Stay connected through drops",
			"Waits through short drops instead of giving up, and rejoins Meowy Network by itself after a timeout "
			.. "or a server restart. Turning it off puts your own cl_timeout back.",
			settings.enabled, function(on) setEnabled(on) end)
		t.meowyToggle = "stay_connected"
		t.gapBefore = S(10)
		blocks[#blocks + 1] = t
		-- The start-up line in menu.lua (startup_hook.h). Off: the pack loads only
		-- when you join, and doesn't put the line back after a game update.
		if meowy.StartupHook then
			local present = meowy.StartupHook()
			local label = UI.Label(nil, "Start-up")
			label.gapBefore = S(22)
			blocks[#blocks + 1] = label
			local st = UI.Toggle(nil, "Start with the game",
				"Loads the pack when Garry's Mod starts, for our loading screen, faster joins and these settings "
				.. "in the main menu. Off: it loads only once you join Meowy Network. Takes effect next start.",
				present == true, function(on)
					if not meowy.SetStartupHook(on) then
						ErrorNoHalt("[Meowy Client Pack] couldn't change the start-up line in lua/includes/menu.lua\n")
					end
				end)
			st.meowyToggle = "start_with_game"
			st.gapBefore = S(10)
			blocks[#blocks + 1] = st
		end
		if meowy.LastServer or meowy.OurServerAddress or meowy.ServerNow then
			local label = UI.Label(nil, "Main menu")
			label.gapBefore = S(22)
			blocks[#blocks + 1] = label
		end
		if meowy.PrewarmStart then
			local pt = UI.Toggle(nil, "Get servers ready before you join",
				"When you're about to join a server you've played on (it's your Continue, or you point at it on the main "
				.. "menu), its big files (Workshop content, the map) are read into memory in the background, so a join "
				.. "after a restart doesn't wait on the disk. SSDs only, up to 4 GB, and it stops the moment a join starts.",
				prewarmOn(), function(on) setPrewarm(on) end)
			pt.meowyToggle = "prewarm"
			pt.gapBefore = S(10)
			blocks[#blocks + 1] = pt
		end
		if meowy.PrewarmWorkshop then
			local wt = UI.Toggle(nil, "Update a server's Workshop items before you join",
				"A server's Workshop items (what it says it uses, and what your last join there used) are checked with "
				.. "Steam on the main menu, and anything missing or out of date downloads there instead of on the loading "
				.. "screen. Meowy Network's own content and servers you've joined before (up to 4 GB) download by "
				.. "themselves, Meowy's a few seconds after the menu opens; any other server shows Get ready with the "
				.. "size, and nothing downloads until you click it. Pointing at a server asks it what it uses. Off stops "
				.. "what's queued.",
				prewarmWorkshopOn(), function(on) setPrewarmWorkshop(on) end)
			wt.meowyToggle = "prewarm_workshop"
			blocks[#blocks + 1] = wt
		end
		-- Find servers, faster (module.cpp's server list chunk owns the setting).
		local fastList = GetConVar and GetConVar("meowy_fast_serverlist")
		if fastList and meowy.ServerQueryStart then
			local ft = UI.Toggle(nil, "Garry's Mod's server list: only servers with players",
				"In Garry's Mod's own server list, Steam leaves out empty servers before anything is pinged, so the Internet "
				.. "list is complete in about 20 s instead of about 90 s. Off lists every server, the game's way. Favourites, "
				.. "History and LAN are always complete. The Meowy home screen's Find servers always lists every server, "
				.. "the empty ones after the rest.",
				fastList:GetBool(), function(on) RunConsoleCommand("meowy_fast_serverlist", on and "1" or "0") end)
			ft.meowyToggle = "fast_serverlist"
			blocks[#blocks + 1] = ft
		end
		-- The console key (console.lua owns the setting).
		local consoleKey = GetConVar and GetConVar("meowy_console_key")
		if consoleKey and meowy.ConsoleRun then
			local ck = UI.Toggle(nil, "The console key opens the Meowy console",
				"The Meowy console: search, warnings and errors only, repeats collapsed, copy, and commands run as "
				.. "typed. Off, the key opens Garry's Mod's console; meowy_console still opens this one.",
				consoleKey:GetBool(), function(on) RunConsoleCommand("meowy_console_key", on and "1" or "0") end)
			ck.meowyToggle = "console_key"
			blocks[#blocks + 1] = ck
		end
		if UI.Choice and menuScaleSetting then
			local options = {}
			for _, pct in ipairs({ 0, 100, 125, 150, 175, 200 }) do
				options[#options + 1] = { pct == 0 and "Auto" or (pct .. "%"), tostring(pct) }
			end
			local current = menuScale()
			local known = false
			for _, o in ipairs(options) do if o[2] == current then known = true end end
			local sc = UI.Choice(nil, "Menu text size",
				"Garry's Mod draws its menu at the same pixel size on any screen, so it's small at 1440p and tiny at 4K. "
				.. "Auto is 100% at 1080p, 133% at 1440p and 200% at 4K. Applies to the main and Escape menus, "
				.. "including Find servers and the other screens.",
				options, known and current or "0", function(v) setMenuScale(v) end)
			sc.meowyChoice = "menu_scale"
			sc.gapBefore = S(10)
			blocks[#blocks + 1] = sc
		end
		if meowy.LastServer then
			local ct = UI.Toggle(nil, "Continue where you left off",
				"Shows Continue in the main menu with the last server you played on, any server. One click joins it; "
				.. "nothing joins on its own. Off: the pack doesn't keep which server it was.",
				continueOn(), function(on) setContinue(on) end)
			ct.meowyToggle = "menu_continue"
			ct.gapBefore = S(10)
			blocks[#blocks + 1] = ct
		end
		if meowy.OurServerAddress then
			local ht = UI.Toggle(nil, "Your servers on the main menu",
				"A card next to the menu with Meowy Network's player count, your favourites and the servers you played on "
				.. "lately, live. One click joins; nothing joins on its own. Asks each server at most once a minute.",
				homeOn(), function(on) setHome(on) end)
			ht.meowyToggle = "menu_home"
			ht.gapBefore = S(10)
			blocks[#blocks + 1] = ht
		end
		if meowy.ServerNow then
			local et = UI.Toggle(nil, "Server card in the Escape menu",
				"In game, the Escape menu shows the server you're on (players, ping, loss, time here), Reconnect, "
				.. "Copy address, and sliders for master volume, voice volume and field of view.",
				escapeOn(), function(on) setEscape(on) end)
			et.meowyToggle = "menu_escape"
			et.gapBefore = S(10)
			blocks[#blocks + 1] = et
		end
		if meowy.MenuShellJs then
			local mt = UI.Toggle(nil, "Meowy home screen",
				"Our home in place of Garry's Mod's main screen: Continue, your servers, one search for servers, maps, "
				.. "addons and settings, Garry's Mod's news, and the usual links (servers, addons and the rest are Garry's "
				.. "Mod's own screens, as before). \"Use Garry's Mod's menu\" at the bottom, or turning this off, puts the "
				.. "usual main screen back.",
				menuOn(), function(on) setMenu(on) end)
			mt.meowyToggle = "menu_page"
			mt.gapBefore = S(10)
			blocks[#blocks + 1] = mt
		end
		if meowy.DiscordAllow then
			local label = UI.Label(nil, "Discord")
			label.gapBefore = S(22)
			blocks[#blocks + 1] = label
			local dt = UI.Toggle(nil, "Discord status on Meowy Network",
				"While you play on Meowy Network, Discord shows your job, the player count, how long you've played and a "
				.. "Join button. Everyone who can see your Discord activity sees it, not only friends. Off: nothing is sent "
				.. "to Discord, and what it shows now is cleared at once.",
				discordOn(), function(on) setDiscord(on) end)
			dt.meowyToggle = "discord"
			dt.gapBefore = S(10)
			blocks[#blocks + 1] = dt
		end
		for _, section in ipairs(settingsSections) do
			local ok, more = pcall(section, function() if show then show("general") end end)
			if ok then
				for _, b in ipairs(more or {}) do blocks[#blocks + 1] = b end
			else
				ErrorNoHalt("[Meowy Client Pack] settings: " .. tostring(more) .. "\n")
			end
		end
		return blocks
	end
	pages.doctor = function()
		if not DoctorUI or not tips then
			return { UI.Text(nil, "The PC doctor couldn't run.", "MeowyPack.Body", C.body) }
		end
		return DoctorUI.build(tips, report)
	end

	-- The page area: a quiet scrollbar, used only when a page is taller than
	-- the screen allows.
	local contentX = nav + 1
	local contentW = W - contentX
	local innerW = contentW - pad * 2
	local scroll = vgui.Create("DScrollPanel", card)
	scroll:SetPos(contentX, headerH)
	local vbar = scroll.GetVBar and scroll:GetVBar()
	if vbar then
		vbar:SetWide(S(6))
		if vbar.SetHideButtons then vbar:SetHideButtons(true) end
		vbar.Paint = function() end
		if vbar.btnGrip then vbar.btnGrip.Paint = function(_, w, h) draw.RoundedBox(w / 2, 0, 0, w, h, C.raised) end end
	end

	-- Left nav, with a divider on its right.
	local navPanel = vgui.Create("DPanel", card)
	navPanel:SetPos(0, headerH)
	navPanel.Paint = function(self, w, h)
		draw.RoundedBox(0, w - 1, 0, 1, h, C.divider)
		return true
	end

	local current
	local maxH = math.floor(ScrH() * 0.8)
	-- Shows a page and sizes the window to it (between 300 px and 80% of the screen).
	function show(id)
		current = id
		scroll:Clear()
		local holder = vgui.Create("DPanel")
		holder.Paint = function() end
		local blocks = pages[id]()
		for _, b in ipairs(blocks) do b:SetParent(holder) end
		local h = UI.stack(blocks, pad, pad, innerW, S(10)) + pad
		holder:SetSize(contentW, h)
		scroll:AddItem(holder)
		local H = math.min(math.max(headerH + h, S(300)), maxH)
		card:SetSize(W, H)
		card:SetPos(math.floor((ScrW() - W) / 2), math.floor((ScrH() - H) / 2))
		scroll:SetSize(contentW, H - headerH)
		navPanel:SetSize(nav, H - headerH)
		if scroll.InvalidateLayout then scroll:InvalidateLayout(true) end
	end

	local navY = S(14)
	local function tab(id, label, badge, tone)
		local b = vgui.Create("DButton", navPanel)
		b:SetText(label)
		b.text, b.badge, b.tabId = label, badge, id
		b:SetPos(S(12), navY)
		b:SetSize(nav - S(24), S(38))
		navY = navY + S(42)
		b.Paint = function(self, w, h)
			local t = UI.hover(self, self:IsHovered())
			local on = current == id
			if on then
				draw.RoundedBox(S(8), 0, 0, w, h, C.raised)
				draw.RoundedBox(S(2), 0, S(10), S(3), h - S(20), C.accent)
			elseif t > 0.01 then
				draw.RoundedBox(S(8), 0, 0, w, h, Color(255, 255, 255, 8 * t))
			end
			draw.SimpleText(label, "MeowyPack.Button", S(14), h / 2, on and C.text or UI.mix(C.body, C.text, t), UI.LEFT, UI.CENTER)
			if self.badge then
				surface.SetFont("MeowyPack.Label")
				local _, ch = surface.GetTextSize("1")
				UI.chip(self.chipText or self.badge, w - S(10), math.floor(h / 2 - (ch + S(8)) / 2), tone, true)
			end
			return true
		end
		b.DoClick = function() show(id) end
		return b
	end
	tab("general", "General")
	if DoctorUI then
		local n = tips and #tips or 0
		local t = tab("doctor", "PC doctor", tips and Doctor.summary(tips) or "?", n > 0 and "amber" or "green")
		t.chipText = tips and (n == 0 and "OK" or tostring(n)) or "?"
	end

	card.w = W
	back:Open(S(300))
	show(pages[startTab or ""] and startTab or "general")
	return back
end

-- The main menu's own list (Start New Game ... Options, Quit) gets a "Meowy
-- settings" entry after Options, through the menu page's JS: the stock
-- entry's look, plus a small accent dot. It's put back whenever the menu
-- page redraws (a MutationObserver, and the Lua side asks again every second),
-- and it reports back through lua.Run so we know it's there.
-- "Continue" goes before Start New Game, out of game only: window.MeowyContinue
-- is [title, server, when] or null, set by the Lua side every second, and only
-- ever shown as text. A long server name is cut short; the time always shows.
local ENTRY_JS = [==[
(function () {
	if (!window.MeowyEntry) {
		window.MeowyContinueEntry = function (ul) {
			var c = ul.querySelector('.meowy-continue-entry');
			var data = window.MeowyContinue;
			if (!data) { if (c) c.parentNode.removeChild(c); return; }
			if (!c) {
				var start = ul.querySelector('a[ng-href="#/newgame/"]') || ul.querySelector('a[href="#/newgame/"]');
				if (!start) return;
				c = document.createElement('li');
				c.className = 'meowy-continue-entry';
				var a = document.createElement('a');
				a.style.cursor = 'pointer';
				var title = document.createElement('span');
				title.className = 'meowy-continue-title';
				var sub = document.createElement('span');
				sub.style.cssText = 'display:block;font-size:14px;letter-spacing:0;font-weight:normal;opacity:0.85;'
					+ 'white-space:nowrap;margin:-3px 0 6px 0;';
				var server = document.createElement('span');
				server.className = 'meowy-continue-server';
				server.style.cssText = 'display:inline-block;max-width:260px;overflow:hidden;text-overflow:ellipsis;'
					+ 'white-space:nowrap;vertical-align:bottom;';
				var when = document.createElement('span');
				when.className = 'meowy-continue-when';
				sub.appendChild(server);
				sub.appendChild(when);
				a.appendChild(title);
				a.appendChild(sub);
				a.onclick = function () { lua.Run('MeowyClientPackContinue()'); };
				c.appendChild(a);
				start.parentNode.parentNode.insertBefore(c, start.parentNode);
			}
			var t = c.querySelector('.meowy-continue-title'), s = c.querySelector('.meowy-continue-server'),
				w = c.querySelector('.meowy-continue-when');
			if (t.textContent !== data[0]) t.textContent = data[0];
			if (s.textContent !== data[1]) { s.textContent = data[1]; s.title = data[1]; }
			if (w.textContent !== data[2]) w.textContent = data[2];
		};
		window.MeowyEntry = function () {
			var lists = document.querySelectorAll('.options ul');
			var added = false;
			for (var i = 0; i < lists.length; i++) {
				var ul = lists[i];
				window.MeowyContinueEntry(ul);
				if (ul.querySelector('.meowy-settings-entry')) continue;
				var links = ul.querySelectorAll('a'), after = null;
				for (var j = 0; j < links.length; j++) {
					if ((links[j].getAttribute('ng-click') || '').indexOf('OpenOptionsDialog') >= 0) { after = links[j].parentNode; break; }
				}
				if (!after) continue;
				var li = document.createElement('li');
				li.className = 'meowy-settings-entry';
				var a = document.createElement('a');
				a.style.cursor = 'pointer';
				a.textContent = 'Meowy settings';
				a.onclick = function () { lua.Run('MeowyClientPackOpenSettings()'); };
				li.appendChild(a);
				after.parentNode.insertBefore(li, after.nextSibling);
				added = true;
			}
			if (added) lua.Run('MeowyClientPackMenuEntry()');
		};
		var MO = window.MutationObserver || window.WebKitMutationObserver;
		if (MO && document.body) new MO(function () { window.MeowyEntry(); }).observe(document.body, { childList: true, subtree: true });
	}
})();
]==]

-- The whole call: the functions (once per page), then this second's Continue
-- data, then the entries put in.
local function entryJS(continueData)
	return ENTRY_JS .. "window.MeowyContinue = " .. continueData .. ";\nwindow.MeowyEntry();\n"
		.. "if (window.MeowyShellContinue) window.MeowyShellContinue();\n"   -- the Meowy home screen's Continue (shell.js)
end

-- What Continue shows now, as a JS value: [title, second line] or null.
local function continueData(hide)
	if hide or not continueOn() or IsInGame() or IsInLoading() or not meowy.LastServer then return "null" end
	local address, name, since = meowy.LastServer()
	local e = Logic.continueEntry(address, name, since, os.time())
	if not e then return "null" end
	return "[" .. Logic.jsString(e.title) .. "," .. Logic.jsString(e.name) .. "," .. Logic.jsString(e.ago) .. "]"
end

-- Called from the menu page (lua.Run): menu state only, no arguments.
function MeowyClientPackOpenSettings() openSettings() end

local entry = { menu = nil, ok = false, since = 0, nextCall = 0 }
function MeowyClientPackMenuEntry() entry.ok = true end

local function menuVisible()
	if IsInLoading() then return false end
	if not IsInGame() then return true end
	return gui and gui.IsGameUIVisible and gui.IsGameUIVisible() or false
end

-- Only if the entry can't be added (a different menu page): a pill at the
-- bottom left, above the menu's footer bar, with the accent dot.
local pill
local function updatePill(show)
	if not show then
		if IsValid(pill) then pill:SetVisible(false) end
		return
	end
	if not IsValid(pill) then
		UI.fonts()
		pill = vgui.Create("DButton")
		pill:SetText("Meowy settings")
		pill:SetFont("MeowyPack.Button")
		surface.SetFont("MeowyPack.Button")
		local tw = surface.GetTextSize("Meowy settings")
		pill.w, pill.h = tw + S(50), S(38)
		pill:SetSize(pill.w, pill.h)
		pill:SetContentAlignment(6)
		pill:SetTextInset(S(18), 0)
		pill.UpdateColours = function(self) return self:SetTextStyleColor(C.white) end
		pill.Paint = function(self, w, h)
			local t = UI.hover(self, self:IsHovered())
			UI.box(h / 2, 0, 0, w, h, Color(31 + 12 * t, 32 + 12 * t, 35 + 12 * t, 235), Color(42 + 40 * t, 43 + 20 * t, 47 + 30 * t))
			local d = S(8)
			draw.RoundedBox(d / 2, S(16), math.floor((h - d) / 2), d, d, C.accent)
		end
		pill.DoClick = function() openSettings() end
		pill:MakePopup()
		pill:SetKeyboardInputEnabled(false)   -- clicks only, never the keyboard
	end
	pill:SetPos(S(24), ScrH() - S(64) - pill.h)
	pill:SetVisible(true)
end

-- hideContinue: a rejoin is under way, so no Continue next to its card.
local function updateMenuEntry(now, hideContinue)
	local visible = menuVisible()
	if visible and IsValid(pnlMainMenu) and pnlMainMenu.Call then
		if entry.menu ~= pnlMainMenu then
			entry.menu, entry.ok, entry.since = pnlMainMenu, false, now   -- a new menu page
		end
		if now >= entry.nextCall then
			entry.nextCall = now + 1
			pnlMainMenu:Call(entryJS(continueData(hideContinue)))
		end
	end
	-- The pill only when the entry hasn't shown up within 6 s of the menu being
	-- there (our own page has its own Meowy settings link).
	updatePill(visible and not entry.ok and not menuPage.ours and (not IsValid(pnlMainMenu) or now - entry.since > 6))
end

if concommand then
	concommand.Add("meowy_settings", function() openSettings() end, nil, "Meowy Client Pack: settings")
end

-- Prompts -------------------------------------------------------------------------
local function askOptIn(after)
	UI.Prompt({
		icon = { "signal", "accent" },
		title = "Stay connected through drops?",
		body = "If your connection drops for a moment, the game keeps waiting instead of giving up, "
			.. "and if you're disconnected anyway, the pack brings you back to Meowy Network by itself. "
			.. "Back within 3 minutes, you usually keep your job, and your weapons, health and armor too unless you "
			.. "were just in a fight, restrained or downed. Not after a server restart, and at most 3 times an hour.",
		note = "Change it any time in Meowy settings, in the main menu.",
		buttons = {
			{ "Not now", "ghost", function() setEnabled(false) if after then after() end end },
			{ "Stay connected", "primary", function() setEnabled(true) if after then after() end end },
		},
		onDismiss = after,   -- Escape: asked again next start
	})
end

-- The reconnect card: top centre, non-modal, clicks only (doesn't take the keyboard).
local overlay
local function hideOverlay()
	if IsValid(overlay) then overlay:Remove() end
	overlay = nil
end
-- buttons: { { text, kind, fn }, ... }, right-aligned; busy shows a spinner;
-- progress (0..1 or nil) is a thin bar along the bottom: time used of the cap.
local function showOverlay(titleText, subText, buttons, busy, progress)
	if not IsValid(overlay) then
		UI.fonts()
		overlay = vgui.Create("DPanel")
		overlay.meowyOverlay = true
		overlay.w = S(460)
		overlay.Paint = function(self, w, h)
			UI.shadow(S(12), 0, 0, w, h)
			UI.box(S(12), 0, 0, w, h, C.window, C.border)
			local bar = self.progress and S(10) or 0
			if self.busy then UI.spinner(S(32), math.floor((h - bar) / 2), S(10), C.accentHi) end
			if self.progress then
				local bw, bh, by = w - S(24), S(3), h - S(9)
				draw.RoundedBox(S(2), S(12), by, bw, bh, C.raised)
				draw.RoundedBox(S(2), S(12), by, math.max(bh, bw * math.Clamp(self.progress, 0, 1)), bh, C.accent)
			end
			return true
		end
		overlay.title = UI.Text(overlay, "", "MeowyPack.Heading", C.text)
		overlay.sub = UI.Text(overlay, "", "MeowyPack.Small", C.body)
		overlay.buttons = {}
		overlay:MakePopup()
		overlay:SetKeyboardInputEnabled(false)
		UI.fadeIn(overlay)
	end
	overlay.progress = progress
	-- The layout is rebuilt only when the title, buttons or bar change; the
	-- status line (a countdown) is just re-wrapped in place.
	local key = titleText .. "|" .. tostring(busy) .. "|" .. tostring(progress ~= nil)
	for _, spec in ipairs(buttons) do key = key .. "|" .. spec[1] end
	if overlay.key == key and overlay.sub.text ~= subText then
		overlay.sub.text = subText
		overlay.sub:Fit(overlay.sub:GetWide())
	end
	if overlay.key ~= key then
		overlay.key, overlay.busy = key, busy
		overlay.title.text, overlay.sub.text = titleText, subText
		for _, b in ipairs(overlay.buttons) do b:Remove() end
		overlay.buttons = {}
		local buttonsW = 0
		for _, spec in ipairs(buttons) do
			local b = UI.Button(overlay, spec[1], spec[2], spec[3])
			overlay.buttons[#overlay.buttons + 1] = b
			buttonsW = buttonsW + b.w + S(8)
		end
		local pad = S(18)
		local bar = progress and S(10) or 0
		local left = busy and pad + S(36) or pad
		local textW = overlay.w - left - pad - buttonsW - S(8)
		local y = UI.stack({ overlay.title, overlay.sub }, left, pad, textW, S(3))
		local h = math.max(y + pad, S(38) + pad * 2) + bar
		overlay:SetSize(overlay.w, h)
		UI.row(overlay.buttons, 0, math.floor((h - bar - S(38)) / 2), overlay.w - pad)
	else
		-- Same buttons, maybe new handlers.
		for i, spec in ipairs(buttons) do
			local fn = spec[3]
			if overlay.buttons[i] then overlay.buttons[i].DoClick = function() if fn then fn() end end end
		end
	end
	overlay:SetPos(math.floor((ScrW() - overlay.w) / 2), S(28))
	-- Above GMod's own "Disconnected" dialog, which the engine opens after the drop.
	overlay:MoveToFront()
end

-- Watching the connection ------------------------------------------------------
-- Every decision goes to the console, so a live test shows why it did (or didn't) rejoin.
local function log(fmt, ...)
	MsgN("[Meowy Client Pack] " .. string.format(fmt, ...))
end
local function show(v) if v == nil then return "nil" end return tostring(v) end
local function fmtNum(v) return isnumber(v) and string.format("%.1f", v) or "nil" end

-- Joins a server the way the menu's own server browser does (the engine's
-- JoinServer). After a Wi-Fi drop the engine kept refusing
-- RunConsoleCommand("connect", address) with "Bad server address" while
-- the same address entered in the console got straight in. Never a password.
local function join(address)
	if JoinServer then return JoinServer(address) end
	RunConsoleCommand("connect", address)
end

-- Continue, clicked (the menu page's lua.Run, or meowy_continue): joins the
-- last server, read again here (native checks its address again). Never on its own.
function MeowyClientPackContinue()
	if IsInGame() or IsInLoading() or not continueOn() or not meowy.LastServer then return end
	local address = meowy.LastServer()
	if not isstring(address) or address == "" then return end
	log("continue: joining %s", address)
	join(address)
end
if concommand then
	concommand.Add("meowy_continue", function() MeowyClientPackContinue() end, nil,
		"Meowy Client Pack: join the last server you played on (the main menu's Continue)")
end

local onOurs, ourAddress = false, nil
local onOther, markedName = false, nil   -- on another (public) server, and the name it was marked with
local continueMarked                     -- the name this connection was kept for Continue with
local armed = false      -- got into the game on our server since the last rejoin try
local last               -- { timingOut, silence, at }: the engine's latest sample of our connection
local episode            -- { s, address, phase = "waiting" | "probing" | "joining", ... }
local stoppedUntil = 0   -- how long the "couldn't reconnect" note stays
local probeId = 0

local function closeNote()
	stoppedUntil = 0
	hideOverlay()
end

local function finish(why, show)
	local e = episode
	if e then log("reconnect: stopped (%s)", why or "no longer needed") end
	episode = nil
	if show and why then
		local text = why:sub(1, 1):upper() .. why:sub(2) .. "."
		if e and Logic.offerRejoin(e.s) then
			-- Gave up (not a kick, ban, password or the player cancelling): the
			-- card stays, with one click to try once more.
			local address = e.address
			showOverlay("Couldn't reconnect to Meowy Network", text, {
				{ "Close", "secondary", closeNote },
				{ "Rejoin now", "primary", function()
					closeNote()
					if isstring(address) and meowy.IsOurServer(address) then
						log("reconnect: Rejoin now -> joining %s", address)
						join(address)
					end
				end },
			}, false)
			stoppedUntil = math.huge   -- until Close, or the player is on a server again
		else
			showOverlay("Couldn't reconnect to Meowy Network", text, { { "Close", "secondary", closeNote } }, false)
			stoppedUntil = SysTime() + 10
		end
	else
		hideOverlay()
	end
end

local function cancel()
	if episode then Logic.stop(episode.s, "cancelled", "cancelled") end
	finish("cancelled", false)
end

local function steamOnline()
	if not meowy.ResilienceSteamOnline then return nil end
	return meowy.ResilienceSteamOnline()
end

local function steamOffline()
	if not meowy.ResilienceSteamOnline then return nil end
	local on = meowy.ResilienceSteamOnline()
	if on == nil then return nil end
	return not on
end

-- Asks the server whether it's up: the pack's own A2S query over plain UDP
-- (it works while Steam is offline), else GMod's serverlist.PingServer.
local function probe(now)
	probeId = probeId + 1
	local id = probeId
	episode.phase, episode.probeUntil = "probing", now + 9
	local function result(answered, locked)
		if not episode or episode.phase ~= "probing" or id ~= probeId then return end
		local t = SysTime()
		local offline = answered and steamOffline() or nil
		local act = Logic.onProbe(episode.s, answered, locked, t, math.random(), offline)
		if episode.s.waitingFor == "down" then
			log("reconnect: server still up %s s after the end (a restart joins only once it went down) -> asking again in %s s",
				fmtNum(t - episode.s.startedAt), fmtNum(episode.s.nextAt - t))
		else
			log("reconnect: attempt %d, server %s%s -> %s", episode.s.attempt,
				answered and "answered" or "didn't answer", offline and ", Steam offline" or "",
				act == "wait" and ("next try in " .. fmtNum(episode.s.nextAt - t) .. " s") or act)
		end
		-- No "is a join already under way?" check here: IsInLoading()/IsInGame()
		-- stay true through GMod's disconnect dialog and teardown, and fooled
		-- that check into waiting forever. A join of the player's own that
		-- already has a connection ends this rejoin through the engine state
		-- (tickEpisode); one that doesn't is replaced by this connect.
		if act == "join" then
			episode.phase, episode.joinAt, episode.sawLoading, episode.reachedOurs = "joining", t, false, false
			log("reconnect: joining %s", episode.address)
			join(episode.address)
		elseif act == "wait" then
			episode.phase = "waiting"
		elseif act == "steam" then
			-- The server is up; only Steam is still logging back on. Check it every
			-- second (no server query, not a try) and join as soon as it's back.
			episode.phase, episode.nextSteam = "steam", t + 1
			log("reconnect: waiting for Steam to reconnect (up to %d s from the drop)", Logic.capFor(episode.s))
		else
			finish(episode.s.stopped, Logic.announceStop(episode.s))
		end
	end
	episode.result = result
	episode.native = meowy.ResilienceProbe and meowy.ResilienceProbe(episode.address) or false
	if not episode.native then
		serverlist.PingServer(episode.address, function(ping, name, desc, map, players, maxplayers, bots, pass)
			result(isnumber(players), pass == true)
		end)
	end
end

local function tickEpisode(now, state)
	local e = episode
	if not settings.enabled then return cancel() end
	-- Only a real connection ends a rejoin early: the player is on our server
	-- again, or joined another one. Not IsInGame()/IsInLoading(): for a moment
	-- after the drop (GMod's disconnect dialog, the teardown) they still say
	-- "in game", which ended every rejoin the moment it started.
	if e.phase ~= "joining" then
		if state == "ours" then return finish("back on our server", false) end
		if state == "other" then return finish("joined another server", false) end
	end
	if e.phase == "waiting" then
		if e.s.stopped then return finish(e.s.stopped, Logic.announceStop(e.s)) end
		if Logic.confirmingDown(e.s) then
			-- A restart notice, but the server hasn't gone down yet: it may be the
			-- player's own Disconnect, a kick or a join elsewhere. Nothing shown.
			hideOverlay()
		else
			local wait = math.max(0, math.ceil(e.s.nextAt - now))
			local status = e.s.attempt == 0 and "Starting..."
				or (e.s.mode == "restart" and "The server is restarting. " or "The server didn't answer. ")
					.. (wait > 0 and ("Trying again in %d s."):format(wait) or "Checking now...")
			showOverlay("Reconnecting to Meowy Network", status, { { "Cancel", "secondary", cancel } }, true,
				(now - e.s.startedAt) / Logic.capFor(e.s))
		end
		if now >= e.s.nextAt then probe(now) end
	elseif e.phase == "steam" then
		showOverlay("Waiting for Steam to reconnect",
			"The server is up. Rejoining as soon as Steam is back.", { { "Cancel", "secondary", cancel } }, true,
			(now - e.s.startedAt) / Logic.capFor(e.s))
		if now >= e.nextSteam then
			e.nextSteam = now + 1
			local act = Logic.onSteamCheck(e.s, steamOnline(), now)
			if act == "probe" then
				log("reconnect: Steam is back; checking the server once more")
				probe(now)
			elseif act == "stop" then
				return finish(e.s.stopped, Logic.announceStop(e.s))
			end
		end
	elseif e.phase == "probing" then
		if e.native then
			local answered, locked = meowy.ResilienceProbeResult()
			if answered ~= nil then return e.result(answered, locked) end
		end
		if now > e.probeUntil then e.result(false, false) end
	elseif e.phase == "joining" then
		hideOverlay()   -- the loading screen shows the join
		if state == "other" then return finish(nil, false) end
		if state == "ours" and IsInGame() and not IsInLoading() then return finish("back in the game", false) end
		if IsInLoading() then e.sawLoading = true end
		-- A join whose connection came up and then ended is judged where it
		-- ends (joinEnded). Here: one that never got a connection.
		if e.reachedOurs then return end
		local waited = now - e.joinAt
		-- The engine said why the join ended ("Disconnected: <reason>",
		-- since this join started): judged at once, not after JOIN_WAIT.
		local reason, ago
		if meowy.ResilienceLastDisconnect then reason, ago = meowy.ResilienceLastDisconnect() end
		if isstring(reason) and isnumber(ago) and ago < waited then
			local kind = Logic.classifyRefusal(reason)
			log("reconnect: join refused after %s s (%s) -> %s", fmtNum(waited), reason, kind)
			Logic.onJoinFailed(e.s, kind, now, math.random())
			if e.s.stopped then return finish(e.s.stopped, Logic.announceStop(e.s)) end
			e.phase = "waiting"
			return
		end
		local turnedAway = e.sawLoading and not IsInLoading() and not IsInGame()
		if turnedAway or waited >= Logic.JOIN_WAIT then
			if not turnedAway then RunConsoleCommand("disconnect") end   -- stop the engine's own retries
			local kind = Logic.classifyJoinEnd({ reachedOurs = false, waited = waited })
			log("reconnect: join got no connection (%s after %s s) -> %s", turnedAway and "turned away" or "no answer", fmtNum(waited), kind)
			Logic.onJoinFailed(e.s, kind, now, math.random())
			if e.s.stopped then return finish(e.s.stopped, Logic.announceStop(e.s)) end
			e.phase = "waiting"
			e.s.nextAt = math.max(e.s.nextAt, now + 1)   -- let the disconnect settle
		end
	end
end

-- A join's connection to our server came up, then ended before the player was in.
local function joinEnded(now, facts)
	local e = episode
	local kind = Logic.classifyJoinEnd({ reachedOurs = true, timingOut = facts.timingOut, silence = facts.silence })
	log("reconnect: the join's connection ended (timingOut=%s, silence=%s) -> %s", show(facts.timingOut), fmtNum(facts.silence), kind)
	Logic.onJoinFailed(e.s, kind, now, math.random())
	if e.s.stopped then return finish(e.s.stopped, Logic.announceStop(e.s)) end
	e.phase = "waiting"
end

-- Shown once at start-up (after a crash on a server), then the opt-in question.
local crashed, crashedName = meowy.ResilienceCrashedSession()
local startAt, startupDone, timeoutChecked = SysTime(), false, false

-- Once, a few seconds after start (config.cfg has run by then): staying
-- connected is on but cl_timeout lost our 60 (a crash before the engine saved
-- it at exit, or a config file): set it again, every start, so config.cfg is
-- never rewritten by the pack. A value the player chose since stays.
local function keepTimeout()
	if not settings.enabled then return end
	local current = clTimeout()
	if not Logic.reapplyTimeout(current, settings.saved_cl_timeout) then return end
	log("cl_timeout is %s again while staying connected is on; setting %s", tostring(current), Logic.TIMEOUT_CL)
	RunConsoleCommand("cl_timeout", Logic.TIMEOUT_CL)
end

-- One prompt at a time: the crash card first, then the opt-in question.
local function startup()
	local function optIn() if not settings.asked then askOptIn() end end
	if crashed and meowy.IsOurServer(crashed) then
		local address = crashed
		UI.Prompt({
			icon = { "signal", "accent" },
			title = "Rejoin Meowy Network?",
			body = "Garry's Mod closed unexpectedly while you were playing. Jump back in?",
			closable = true,
			buttons = {
				{ "Not now", "ghost", optIn },
				{ "Rejoin", "primary", function()
					if meowy.IsOurServer(address) then join(address) end
				end },
			},
			onDismiss = optIn,
		})
		return
	end
	if crashed then
		-- Another server (native checked it's a public a.b.c.d:port): one click
		-- connects; nothing automatic.
		local address = crashed
		local name = isstring(crashedName) and crashedName ~= "" and crashedName or address
		UI.Prompt({
			icon = { "signal", "accent" },
			title = "Rejoin the server?",
			body = "Garry's Mod closed unexpectedly while you were on " .. name .. " (" .. address .. "). Jump back in?",
			closable = true,
			buttons = {
				{ "Not now", "ghost", optIn },
				-- JoinServer, as every other join: the engine refuses RunConsoleCommand("connect", address).
				{ "Rejoin", "primary", function() join(address) end },
			},
			onDismiss = optIn,
		})
		return
	end
	optIn()
end

local nextLook, entryFailed = 0, false
hook.Add("Think", "MeowyClientPack.Resilience", function()
	local now = SysTime()

	-- Every frame: the engine's own view of our connection, and the moment it goes.
	local state, address, timingOut, quiet = meowy.ResilienceWatch()
	local ended
	if state == "ours" then
		if not onOurs then
			meowy.ResilienceSession(true, meowy.JoiningName or "")
			log("connected to our server %s; connection reading: timingOut=%s, silence=%s%s", show(address), show(timingOut),
				fmtNum(quiet), timingOut == nil and " (couldn't read it: a drop won't be recognised)" or "")
		end
		onOurs, ourAddress = true, address
		last = { timingOut = timingOut, silence = quiet, at = now }
		if episode and episode.phase == "joining" then episode.reachedOurs = true end
		if IsInGame() and not IsInLoading() then armed = true end
	elseif (state == "none" or state == "other") and onOurs then
		onOurs = false
		meowy.ResilienceSession(false)
		-- Only a sample from just before counts (the menu runs every frame;
		-- a few "unknown" frames while the engine tears the connection down are fine).
		local fresh = last and now - last.at < 3 or false
		ended = { connectionEnded = state == "none", restartExpected = meowy.ResilienceRestartExpected() }
		if fresh then ended.timingOut, ended.silence = last.timingOut, last.silence end   -- false stays false
		meowy.ResilienceClearRestart()   -- used (or not): either way it's spent
		ended.fresh, ended.state, ended.sampleAge = fresh, state, last and (now - last.at) or nil
		last = nil
	end

	-- Any other server: remembered too (the address is the engine's, and only a
	-- public one is kept), so a crash there can offer a rejoin. Marked again
	-- when its name arrives during the join.
	if state == "other" then
		local name = meowy.JoiningName or ""
		if not onOther or name ~= markedName then
			onOther, markedName = true, name
			meowy.ResilienceSession(true, name)
		end
	elseif onOther then
		onOther, markedName = false, nil
		if state == "none" then meowy.ResilienceSession(false) end
	end

	-- Continue: kept once the player is in the game on a server (again when its
	-- name arrives or changes); native keeps only ours or a public address.
	if state == "ours" or state == "other" then
		local name = meowy.JoiningName or ""
		if IsInGame() and not IsInLoading() and continueMarked ~= name and continueOn() and meowy.LastServerMark then
			continueMarked = name
			meowy.LastServerMark(name)
		end
	else
		continueMarked = nil
	end

	if ended then
		local mode, needDown = Logic.classifyEnd(ended)
		local decision
		if episode and episode.phase == "joining" then
			decision = "part of a rejoin attempt"
		elseif episode then
			decision = "no rejoin: one is already running"
		elseif not settings.enabled then
			decision = "no rejoin: staying connected is off"
		elseif not armed then
			decision = "no rejoin: not in the game since the last try"
		elseif not (isstring(ourAddress) and meowy.IsOurServer(ourAddress)) then
			decision = "no rejoin: no address of ours"
		elseif not mode then
			decision = ended.connectionEnded and (ended.timingOut == nil and "no rejoin: no engine sample" or "no rejoin: packets were still arriving")
				or "no rejoin: joined another server"
		else
			decision = "rejoin (" .. mode .. ")"
		end
		log("connection ended: state=%s, fresh sample=%s (%s s old), timingOut=%s, silence=%s, restartExpected=%s, armed=%s, enabled=%s -> %s",
			show(ended.state), show(ended.fresh), fmtNum(ended.sampleAge), show(ended.timingOut), fmtNum(ended.silence),
			show(ended.restartExpected), show(armed), show(settings.enabled), decision)
		if episode and episode.phase == "joining" then
			local ok, err = pcall(joinEnded, now, ended)
			if not ok then ErrorNoHalt("[Meowy Client Pack] reconnect: " .. tostring(err) .. "\n") finish(nil, false) end
		elseif decision:sub(1, 6) == "rejoin" then
			armed = false   -- the next try needs the player in the game again
			episode = { s = Logic.start(mode, now, math.random(), needDown), address = ourAddress, phase = "waiting" }
			log("reconnect: %s, first try in %s s, giving up after %d s%s", mode, fmtNum(episode.s.nextAt - now), Logic.CAP[mode] or 0,
				episode.s.needDown and (", joining only once the server has gone down (still up after " .. Logic.UP_WAIT .. " s: not a restart)") or "")
		end
	end

	if now < nextLook then return end
	nextLook = now + 0.2

	local okEntry, entryErr = pcall(updateMenuEntry, now, episode ~= nil)
	if not okEntry and not entryFailed then
		entryFailed = true   -- report once, not five times a second
		ErrorNoHalt("[Meowy Client Pack] settings entry: " .. tostring(entryErr) .. "\n")
	end

	if not timeoutChecked and now - startAt > 3 then
		timeoutChecked = true
		local ok, err = pcall(keepTimeout)
		if not ok then ErrorNoHalt("[Meowy Client Pack] cl_timeout: " .. tostring(err) .. "\n") end
	end

	-- Started straight into a join (a Steam join link): no point offering one after.
	if not startupDone and (IsInGame() or IsInLoading()) then crashed = nil end
	if not startupDone and now - startAt > 3 and not IsInGame() and not IsInLoading() then
		startupDone = true
		local ok, err = pcall(startup)
		if not ok then ErrorNoHalt("[Meowy Client Pack] " .. tostring(err) .. "\n") end
	end

	if episode then
		local ok, err = pcall(tickEpisode, now, state)
		if not ok then
			ErrorNoHalt("[Meowy Client Pack] reconnect: " .. tostring(err) .. "\n")
			finish(nil, false)
		end
	elseif IsValid(overlay) and stoppedUntil > 0 and (now > stoppedUntil or state == "ours" or state == "other") then
		closeNote()   -- the note's time is up, or the player is on a server again
	end
end)

-- meowy_resilience_debug: the engine's live reading and what the pack would do,
-- to check the net-channel reads in game. Prints only; changes nothing.
if concommand then
	concommand.Add("meowy_resilience_debug", function()
		local state, address, timingOut, quiet = meowy.ResilienceWatch()
		log("debug: state=%s, address=%s, timingOut=%s, silence=%s (nil = couldn't read)",
			show(state), show(address), show(timingOut), fmtNum(quiet))
		log("debug: enabled=%s, armed=%s, on our server=%s, last sample %s, restartExpected=%s, Steam online=%s",
			show(settings.enabled), show(armed), show(onOurs),
			last and (fmtNum(SysTime() - last.at) .. " s ago") or "none",
			show(meowy.ResilienceRestartExpected()),
			show(meowy.ResilienceSteamOnline and meowy.ResilienceSteamOnline()))
		if episode then
			local s = episode.s
			log("debug: rejoin %s to %s: phase=%s, attempt=%d, failed joins=%d, next try in %s s, waiting for %s, %s s of %d",
				s.mode, show(episode.address), episode.phase, s.attempt, s.failedJoins, fmtNum(s.nextAt - SysTime()),
				show(s.waitingFor), fmtNum(SysTime() - s.startedAt), Logic.capFor(s))
		else
			log("debug: no rejoin running")
		end
	end, nil, "Meowy Client Pack: print the connection reading and the rejoin state")
end
