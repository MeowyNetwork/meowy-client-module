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

-- UI (ui.lua's kit) ----------------------------------------------------------------
local C, S = UI.C, UI.S

-- The settings window: a left tab list (General, PC doctor) and the tab's
-- content on the right, scrolled. Escape or the "x" closes it.
local settingsWindow
local function openSettings(startTab)
	if IsValid(settingsWindow) then settingsWindow:Remove() end
	local back, card = UI.Modal(780)
	settingsWindow = back
	local W, H = card.w, S(540)
	local pad, side = S(24), S(196)

	local title = UI.Text(card, "Meowy Client Pack", "MeowyPack.Title", C.white)
	local sub = UI.Text(card, "Settings  ·  " .. tostring(meowy.VERSION or ""), "MeowyPack.Small", C.dim)
	sub.gapBefore = S(2)
	local headerBottom = UI.stack({ title, sub }, pad + S(4), pad, side + S(200))
	local x = UI.Close(card, function() back:Dismiss() end)
	x:SetPos(W - S(16) - x.w, S(16))

	-- PC doctor's findings, worked out once per opening (it asks Windows).
	local tips, report
	if DoctorUI then
		local ok, a, b = pcall(DoctorUI.diagnose)
		if ok then tips, report = a, b else ErrorNoHalt("[Meowy Client Pack] PC doctor: " .. tostring(a) .. "\n") end
	end

	local top = headerBottom + S(20)
	local contentX = pad + S(4) + side + pad
	local contentW = W - contentX - pad
	local scroll = vgui.Create("DScrollPanel", card)
	scroll:SetPos(contentX, top)
	scroll:SetSize(contentW, H - top - pad)
	local bar = scroll.GetVBar and scroll:GetVBar()
	if bar then
		bar:SetWide(S(6))
		if bar.SetHideButtons then bar:SetHideButtons(true) end
		bar.Paint = function() end
		if bar.btnGrip then bar.btnGrip.Paint = function(_, w, h) draw.RoundedBox(w / 2, 0, 0, w, h, C.border) end end
	end
	local innerW = contentW - S(14)

	local function heading(text, gap)
		local t = UI.Text(nil, text, "MeowyPack.Heading", C.white)
		t.gapBefore = gap
		return t
	end

	local pages = {}
	pages.general = function()
		local blocks = { heading("Connection") }
		local card1 = UI.Card(nil)
		card1:Add(UI.Toggle(card1, "Stay connected through drops",
			"Keeps your connection through short drops (cl_timeout 60) and rejoins Meowy Network by itself after "
			.. "a timeout or a server restart. Turning it off puts your own cl_timeout back.",
			settings.enabled, function(on) setEnabled(on) end))
		card1.blocks[1].meowyToggle = "stay_connected"
		blocks[#blocks + 1] = card1
		card1.gapBefore = S(12)
		return blocks
	end
	pages.doctor = function()
		if not DoctorUI or not tips then
			return { UI.Text(nil, "The PC doctor couldn't run.", "MeowyPack.Body", C.text) }
		end
		return DoctorUI.build(tips, report)
	end

	local tabs, current = {}, nil
	local function show(id)
		current = id
		scroll:Clear()
		local blocks = pages[id]()
		for _, b in ipairs(blocks) do scroll:Add(b) end
		UI.stack(blocks, 0, 0, innerW, S(12))
		if scroll.InvalidateLayout then scroll:InvalidateLayout(true) end
	end

	local tabY = top
	local function tab(id, label, badge, badgeColor)
		local b = vgui.Create("DButton", card)
		b:SetText(label)
		b:SetFont("MeowyPack.Button")
		b.badge, b.tabId = badge, id
		b:SetSize(side, S(42))
		b:SetPos(pad + S(4), tabY)
		tabY = tabY + S(46)
		b:SetContentAlignment(4)
		b:SetTextInset(S(16), 0)
		b.UpdateColours = function(self) return self:SetTextStyleColor(current == id and C.white or C.text) end
		b.Paint = function(self, w, h)
			local t = UI.hover(self, self:IsHovered())
			if current == id then
				draw.RoundedBox(S(8), 0, 0, w, h, C.cardHi)
				draw.RoundedBox(S(2), 0, S(10), S(3), h - S(20), C.accent)
			elseif t > 0.01 then
				draw.RoundedBox(S(8), 0, 0, w, h, Color(255, 255, 255, 10 * t))
			end
			if self.badge then
				surface.SetFont("MeowyPack.Small")
				local tw, th = surface.GetTextSize(self.badge)
				local bw, bh = tw + S(14), th + S(4)
				local bx, by = w - bw - S(10), math.floor((h - bh) / 2)
				draw.RoundedBox(bh / 2, bx, by, bw, bh, badgeColor)
				draw.SimpleText(self.badge, "MeowyPack.Small", bx + bw / 2, by + bh / 2, C.white, TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER)
			end
		end
		b.DoClick = function() show(id) end
		tabs[#tabs + 1] = b
		return b
	end
	tab("general", "General")
	if DoctorUI then
		local n = tips and #tips or 0
		tab("doctor", "PC doctor", tips and Doctor.summary(tips) or "?", n > 0 and C.accent or C.green)
	end

	show(pages[startTab or ""] and startTab or "general")
	back:Open(H)
	return back
end

-- The main menu's own list (Start New Game ... Options, Quit) gets a "Meowy
-- settings" entry after Options, through the menu page's JS: the stock
-- entry's look, plus a small accent dot. It's put back whenever the menu
-- page redraws (a MutationObserver, and the Lua side asks again every second),
-- and it reports back through lua.Run so we know it's there.
local ENTRY_JS = [==[
(function () {
	if (!window.MeowyEntry) {
		window.MeowyEntry = function () {
			var lists = document.querySelectorAll('.options ul');
			var added = false;
			for (var i = 0; i < lists.length; i++) {
				var ul = lists[i];
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
				a.innerHTML = '<span style="display:inline-block;width:0.42em;height:0.42em;border-radius:50%;background:#c2407a;margin-right:0.45em;vertical-align:middle;position:relative;top:-0.08em"></span>Meowy settings';
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
	window.MeowyEntry();
})();
]==]

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

local function updateMenuEntry(now)
	local visible = menuVisible()
	if visible and IsValid(pnlMainMenu) and pnlMainMenu.Call then
		if entry.menu ~= pnlMainMenu then
			entry.menu, entry.ok, entry.since = pnlMainMenu, false, now   -- a new menu page
		end
		if now >= entry.nextCall then
			entry.nextCall = now + 1
			pnlMainMenu:Call(ENTRY_JS)
		end
	end
	-- The pill only when the entry hasn't shown up within 6 s of the menu being there.
	updatePill(visible and not entry.ok and (not IsValid(pnlMainMenu) or now - entry.since > 6))
end

if concommand then
	concommand.Add("meowy_settings", function() openSettings() end, nil, "Meowy Client Pack: settings")
end

-- Prompts -------------------------------------------------------------------------
local function askOptIn(after)
	UI.Prompt({
		title = "Stay connected through drops?",
		body = "If your connection drops for a moment, Garry's Mod keeps waiting for it (cl_timeout 60), "
			.. "and if you're disconnected anyway, the pack rejoins Meowy Network for you. Your job and "
			.. "items come back if you're in within 3 minutes.",
		note = "You can change this any time in Meowy settings, in the main menu.",
		buttons = {
			{ "No thanks", "secondary", function() setEnabled(false) if after then after() end end },
			{ "Yes, stay connected", "primary", function() setEnabled(true) if after then after() end end },
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
-- buttons: { { text, kind, fn }, ... }, right-aligned; busy shows the dots.
local function showOverlay(titleText, subText, buttons, busy)
	if not IsValid(overlay) then
		UI.fonts()
		overlay = vgui.Create("DPanel")
		overlay.meowyOverlay = true
		overlay.w = S(540)
		overlay.Paint = function(self, w, h)
			UI.box(S(12), 0, 0, w, h, C.bg, C.border)
			draw.RoundedBoxEx(S(12), 0, 0, S(5), h, C.accent, true, false, true, false)
			if self.busy then   -- three dots, one after another
				for i = 0, 2 do
					local a = 0.35 + 0.65 * math.max(0, math.sin(SysTime() * 5 - i * 0.9))
					local d = S(7)
					draw.RoundedBox(d / 2, S(26) + i * S(12), math.floor(h / 2) - d / 2, d, d, Color(194, 64, 122, 255 * a))
				end
			end
			return true
		end
		overlay.title = UI.Text(overlay, "", "MeowyPack.Heading", C.white)
		overlay.sub = UI.Text(overlay, "", "MeowyPack.Small", C.dim)
		overlay.buttons = {}
		overlay:MakePopup()
		overlay:SetKeyboardInputEnabled(false)
		UI.fadeIn(overlay)
	end
	local key = titleText .. "|" .. subText .. "|" .. tostring(busy)
	for _, spec in ipairs(buttons) do key = key .. "|" .. spec[1] end
	if overlay.key ~= key then
		overlay.key, overlay.busy = key, busy
		overlay.title.text, overlay.sub.text = titleText, subText
		for _, b in ipairs(overlay.buttons) do b:Remove() end
		overlay.buttons = {}
		local buttonsW = 0
		for _, spec in ipairs(buttons) do
			local b = UI.Button(overlay, spec[1], spec[2], spec[3])
			overlay.buttons[#overlay.buttons + 1] = b
			buttonsW = buttonsW + b.w + S(10)
		end
		local left = busy and S(70) or S(26)
		local textW = overlay.w - left - buttonsW - S(30)
		local y = UI.stack({ overlay.title, overlay.sub }, left, S(16), textW, S(2))
		local h = math.max(y + S(16), S(40) + S(28))
		overlay:SetSize(overlay.w, h)
		UI.row(overlay.buttons, 0, math.floor((h - S(40)) / 2), overlay.w - S(18))
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

local onOurs, ourAddress = false, nil
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
						RunConsoleCommand("connect", address)   -- never a password
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
		log("reconnect: attempt %d, server %s%s -> %s", episode.s.attempt,
			answered and "answered" or "didn't answer", offline and ", Steam offline" or "",
			act == "wait" and ("next try in " .. fmtNum(episode.s.nextAt - t) .. " s") or act)
		-- No "is a join already under way?" check here: IsInLoading()/IsInGame()
		-- stay true through GMod's disconnect dialog and teardown, and fooled
		-- that check into waiting forever. A join of the player's own that
		-- already has a connection ends this rejoin through the engine state
		-- (tickEpisode); one that doesn't is replaced by this connect.
		if act == "join" then
			episode.phase, episode.joinAt, episode.sawLoading, episode.reachedOurs = "joining", t, false, false
			log("reconnect: joining %s", episode.address)
			RunConsoleCommand("connect", episode.address)   -- never a password
		elseif act == "wait" then
			episode.phase = "waiting"
		elseif act == "steam" then
			-- The server is up; only Steam is still logging back on. Check it every
			-- second (no server query, not a try) and join as soon as it's back.
			episode.phase, episode.nextSteam = "steam", t + 1
			log("reconnect: waiting for Steam to reconnect (up to %d s from the drop)", Logic.capFor(episode.s))
		else
			finish(episode.s.stopped, true)
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
		if e.s.stopped then return finish(e.s.stopped, true) end
		showOverlay("Reconnecting to Meowy Network...",
			("Attempt %d  ·  %s"):format(e.s.attempt, e.s.mode == "restart" and "waiting for the server to restart" or "waiting for the server"),
			{ { "Cancel", "secondary", cancel } }, true)
		if now >= e.s.nextAt then probe(now) end
	elseif e.phase == "steam" then
		showOverlay("Waiting for Steam to reconnect...",
			"The server is up. Rejoining as soon as Steam is back.", { { "Cancel", "secondary", cancel } }, true)
		if now >= e.nextSteam then
			e.nextSteam = now + 1
			local act = Logic.onSteamCheck(e.s, steamOnline(), now)
			if act == "probe" then
				log("reconnect: Steam is back; checking the server once more")
				probe(now)
			elseif act == "stop" then
				return finish(e.s.stopped, true)
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
		local turnedAway = e.sawLoading and not IsInLoading() and not IsInGame()
		if turnedAway or waited >= Logic.JOIN_WAIT then
			if not turnedAway then RunConsoleCommand("disconnect") end   -- stop the engine's own retries
			local kind = Logic.classifyJoinEnd({ reachedOurs = false, waited = waited })
			log("reconnect: join got no connection (%s after %s s) -> %s", turnedAway and "turned away" or "no answer", fmtNum(waited), kind)
			Logic.onJoinFailed(e.s, kind, now, math.random())
			if e.s.stopped then return finish(e.s.stopped, true) end
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
	if e.s.stopped then return finish(e.s.stopped, true) end
	e.phase = "waiting"
end

-- Shown once at start-up (after a crash on our server), then the opt-in question.
local crashed = meowy.ResilienceCrashedSession()
local startAt, startupDone = SysTime(), false

-- One prompt at a time: the crash card first, then the opt-in question.
local function startup()
	local function optIn() if not settings.asked then askOptIn() end end
	if crashed and meowy.IsOurServer(crashed) then
		local address = crashed
		UI.Prompt({
			title = "Rejoin Meowy Network?",
			body = "Garry's Mod closed unexpectedly while you were playing on Meowy Network.",
			closable = true,
			buttons = {
				{ "Not now", "secondary", optIn },
				{ "Rejoin", "primary", function()
					if meowy.IsOurServer(address) then RunConsoleCommand("connect", address) end
				end },
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
			meowy.ResilienceSession(true)
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

	if ended then
		local mode = Logic.classifyEnd(ended)
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
			episode = { s = Logic.start(mode, now, math.random()), address = ourAddress, phase = "waiting" }
			log("reconnect: %s, first try in %s s, giving up after %d s", mode, fmtNum(episode.s.nextAt - now), Logic.CAP[mode] or 0)
		end
	end

	if now < nextLook then return end
	nextLook = now + 0.2

	local okEntry, entryErr = pcall(updateMenuEntry, now)
	if not okEntry and not entryFailed then
		entryFailed = true   -- report once, not five times a second
		ErrorNoHalt("[Meowy Client Pack] settings entry: " .. tostring(entryErr) .. "\n")
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
