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
--   server had said it was restarting (meowy.ExpectRestart). Never from text.
--   Only after the player had got into the game there. The server is asked
--   whether it's up (A2S, serverlist.PingServer) before each join. A small
--   window shows each try and has Cancel.
-- * After a crash while on our server, the next start offers "Rejoin Meowy
--   Network?". It never joins without the click.
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

-- UI -------------------------------------------------------------------------
local BG, FG, DIM = Color(24, 24, 30, 245), Color(240, 240, 245), Color(180, 180, 190)

local function card(title, text, buttons, width)
	local w, h = width or 420, 170
	local f = vgui.Create("DFrame")
	f:SetTitle("")
	f:SetSize(w, h)
	f:Center()
	f:SetDraggable(true)
	f:ShowCloseButton(true)
	f.Paint = function(self, pw, ph) draw.RoundedBox(6, 0, 0, pw, ph, BG) end
	local t = vgui.Create("DLabel", f)
	t:SetFont("DermaLarge")
	t:SetTextColor(FG)
	t:SetText(title)
	t:SetPos(16, 12)
	t:SizeToContents()
	local body = vgui.Create("DLabel", f)
	body:SetFont("DermaDefault")
	body:SetTextColor(DIM)
	body:SetWrap(true)
	body:SetText(text)
	body:SetPos(16, 52)
	body:SetSize(w - 32, h - 52 - 50)
	body:SetContentAlignment(7)
	local x = w - 16
	for i = #buttons, 1, -1 do
		local b = vgui.Create("DButton", f)
		b:SetText(buttons[i][1])
		b:SetSize(110, 28)
		x = x - 110
		b:SetPos(x, h - 40)
		x = x - 8
		b.DoClick = function() f:Close() buttons[i][2]() end
	end
	f:MakePopup()
	return f
end

local function openSettings()
	local f = vgui.Create("DFrame")
	f:SetTitle("Meowy Client Pack settings")
	f:SetSize(440, 110)
	f:Center()
	local box = vgui.Create("DCheckBoxLabel", f)
	box:SetPos(16, 40)
	box:SetText("Stay connected through drops (rejoin Meowy Network automatically)")
	box:SetValue(settings.enabled)
	box:SizeToContents()
	box.OnChange = function(_, value) setEnabled(value) end
	local note = vgui.Create("DLabel", f)
	note:SetPos(16, 66)
	note:SetSize(408, 30)
	note:SetWrap(true)
	note:SetText("On: cl_timeout is 60 while you play. Off: your own cl_timeout comes back.")
	f:MakePopup()
end
if concommand then
	concommand.Add("meowy_settings", function() openSettings() end, nil, "Meowy Client Pack: settings")
end

-- A small "Meowy settings" button in the main menu's top-right corner (the
-- stock menu has nothing there). A panel of its own, not part of the menu's
-- page, so the stock menu is untouched; made again if a menu reload removes
-- it, shown only while the menu itself is (not while loading or playing).
local menuButton, buttonFailed = nil, false
local function menuVisible()
	if IsInLoading() then return false end
	if not IsInGame() then return true end
	return gui and gui.IsGameUIVisible and gui.IsGameUIVisible() or false
end
local function updateMenuButton()
	if not IsValid(menuButton) then
		menuButton = vgui.Create("DButton")
		menuButton:SetText("Meowy settings")
		menuButton:SetSize(120, 24)
		menuButton.DoClick = function() openSettings() end
		menuButton:MakePopup()
		menuButton:SetKeyboardInputEnabled(false)   -- clicks only, never the keyboard
	end
	menuButton:SetPos(ScrW() - 120 - 12, 12)
	menuButton:SetVisible(menuVisible())
end

local function askOptIn()
	card("Stay connected through drops?",
		"Keeps your connection alive through short drops and rejoins Meowy Network automatically "
		.. "after a disconnect (it sets cl_timeout to 60). You can change it later with the "
		.. "Meowy settings button at the top right of the main menu.",
		{ { "No thanks", function() setEnabled(false) end }, { "Yes", function() setEnabled(true) end } })
end

-- The reconnect window: top centre, clicks only (doesn't take the keyboard).
local overlay
local function hideOverlay()
	if IsValid(overlay) then overlay:Remove() end
	overlay = nil
end
local function showOverlay(text, buttonText, onButton)
	if not IsValid(overlay) then
		overlay = vgui.Create("DPanel")
		overlay:SetSize(420, 48)
		overlay.Paint = function(self, pw, ph) draw.RoundedBox(6, 0, 0, pw, ph, BG) end
		overlay.label = vgui.Create("DLabel", overlay)
		overlay.label:SetFont("DermaDefaultBold")
		overlay.label:SetTextColor(FG)
		overlay.label:SetPos(14, 0)
		overlay.label:SetSize(300, 48)
		overlay.button = vgui.Create("DButton", overlay)
		overlay.button:SetSize(80, 28)
		overlay.button:SetPos(420 - 94, 10)
		overlay:MakePopup()
		overlay:SetKeyboardInputEnabled(false)
	end
	overlay:SetPos(ScrW() / 2 - 210, 24)
	overlay.label:SetText(text)
	overlay.button:SetText(buttonText)
	overlay.button.DoClick = onButton
end

-- Watching the connection ------------------------------------------------------
local onOurs, ourAddress = false, nil
local armed = false      -- got into the game on our server since the last rejoin try
local last               -- { timingOut, silence, at }: the engine's latest sample of our connection
local episode            -- { s, address, phase = "waiting" | "probing" | "joining", ... }
local stoppedUntil = 0   -- how long the "couldn't reconnect" note stays
local probeId = 0

local function finish(why, show)
	episode = nil
	if show and why then
		showOverlay("Couldn't reconnect: " .. why, "Close", hideOverlay)
		stoppedUntil = SysTime() + 10
	else
		hideOverlay()
	end
end

local function cancel()
	if episode then Logic.stop(episode.s, "cancelled") end
	finish(nil, false)
end

local function probe(now)
	probeId = probeId + 1
	local id = probeId
	episode.phase, episode.probeUntil = "probing", now + 6
	local function result(answered, locked)
		if not episode or episode.phase ~= "probing" or id ~= probeId then return end
		local t = SysTime()
		local act = Logic.onProbe(episode.s, answered, locked, t, math.random())
		if act == "join" then
			episode.phase, episode.joinAt, episode.sawLoading, episode.reachedOurs = "joining", t, false, false
			RunConsoleCommand("connect", episode.address)   -- never a password
		elseif act == "wait" then
			episode.phase = "waiting"
		else
			finish(episode.s.stopped, true)
		end
	end
	episode.result = result
	serverlist.PingServer(episode.address, function(ping, name, desc, map, players, maxplayers, bots, pass)
		result(isnumber(players), pass == true)
	end)
end

local function tickEpisode(now, state)
	local e = episode
	if not settings.enabled then return cancel() end
	if e.phase == "waiting" then
		-- The player started a join of their own: theirs wins.
		if IsInLoading() or IsInGame() then return finish(nil, false) end
		if e.s.stopped then return finish(e.s.stopped, true) end
		showOverlay(("Reconnecting to Meowy Network... (attempt %d)"):format(e.s.attempt), "Cancel", cancel)
		if now >= e.s.nextAt then probe(now) end
	elseif e.phase == "probing" then
		if IsInLoading() or IsInGame() then return finish(nil, false) end
		if now > e.probeUntil then e.result(false, false) end
	elseif e.phase == "joining" then
		hideOverlay()   -- the loading screen shows the join
		if state == "other" then return finish(nil, false) end
		if state == "ours" and IsInGame() and not IsInLoading() then return finish(nil, false) end
		if IsInLoading() then e.sawLoading = true end
		-- A join whose connection came up and then ended is judged where it
		-- ends (joinEnded). Here: one that never got a connection.
		if e.reachedOurs then return end
		local waited = now - e.joinAt
		local turnedAway = e.sawLoading and not IsInLoading() and not IsInGame()
		if turnedAway or waited >= Logic.JOIN_WAIT then
			if not turnedAway then RunConsoleCommand("disconnect") end   -- stop the engine's own retries
			Logic.onJoinFailed(e.s, Logic.classifyJoinEnd({ reachedOurs = false, waited = waited }), now, math.random())
			if e.s.stopped then return finish(e.s.stopped, true) end
			e.phase = "waiting"
			e.s.nextAt = math.max(e.s.nextAt, now + 1)   -- let the disconnect settle
		end
	end
end

-- A join's connection to our server came up, then ended before the player was in.
local function joinEnded(now, facts)
	local e = episode
	Logic.onJoinFailed(e.s, Logic.classifyJoinEnd({ reachedOurs = true, timingOut = facts.timingOut, silence = facts.silence }), now, math.random())
	if e.s.stopped then return finish(e.s.stopped, true) end
	e.phase = "waiting"
end

-- Shown once at start-up (after a crash on our server), then the opt-in question.
local crashed = meowy.ResilienceCrashedSession()
local startAt, startupDone = SysTime(), false

local function startup()
	if crashed and meowy.IsOurServer(crashed) then
		local address = crashed
		card("Rejoin Meowy Network?",
			"Garry's Mod closed unexpectedly while you were playing on Meowy Network.",
			{ { "Not now", function() end }, { "Rejoin", function()
				if meowy.IsOurServer(address) then RunConsoleCommand("connect", address) end
			end } })
	end
	if not settings.asked then askOptIn() end
end

local nextLook = 0
hook.Add("Think", "MeowyClientPack.Resilience", function()
	local now = SysTime()

	-- Every frame: the engine's own view of our connection, and the moment it goes.
	local state, address, timingOut, quiet = meowy.ResilienceWatch()
	local ended
	if state == "ours" then
		if not onOurs then meowy.ResilienceSession(true) end
		onOurs, ourAddress = true, address
		last = { timingOut = timingOut, silence = quiet, at = now }
		if episode and episode.phase == "joining" then episode.reachedOurs = true end
		if IsInGame() and not IsInLoading() then armed = true end
	elseif (state == "none" or state == "other") and onOurs then
		onOurs = false
		meowy.ResilienceSession(false)
		-- Only a sample from just before counts (the menu runs every frame).
		local fresh = last and now - last.at < 1
		ended = {
			connectionEnded = state == "none",
			timingOut = fresh and last.timingOut or nil,
			silence = fresh and last.silence or nil,
			restartExpected = meowy.ResilienceRestartExpected(),
		}
		meowy.ResilienceClearRestart()   -- used (or not): either way it's spent
		last = nil
	end

	if ended then
		if episode and episode.phase == "joining" then
			local ok, err = pcall(joinEnded, now, ended)
			if not ok then ErrorNoHalt("[Meowy Client Pack] reconnect: " .. tostring(err) .. "\n") finish(nil, false) end
		elseif not episode and armed and settings.enabled and isstring(ourAddress) and meowy.IsOurServer(ourAddress) then
			local mode = Logic.classifyEnd(ended)
			if mode then
				armed = false   -- the next try needs the player in the game again
				episode = { s = Logic.start(mode, now, math.random()), address = ourAddress, phase = "waiting" }
			end
		end
	end

	if now < nextLook then return end
	nextLook = now + 0.2

	local okButton, buttonErr = pcall(updateMenuButton)
	if not okButton and not buttonFailed then
		buttonFailed = true   -- report once, not five times a second
		ErrorNoHalt("[Meowy Client Pack] settings button: " .. tostring(buttonErr) .. "\n")
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
	elseif IsValid(overlay) and stoppedUntil > 0 and now > stoppedUntil then
		stoppedUntil = 0
		hideOverlay()
	end
end)
