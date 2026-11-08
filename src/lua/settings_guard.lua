-- The settings guard: what a server changed in your settings, put back in one
-- click. A session starts when a join starts (the snapshot is taken before any
-- server Lua runs) and ends a few seconds after you're back at the main menu
-- and not reconnecting (resilience.lua's card is down). Each server you were
-- on in it is answered for on its own (settings_guard_logic.lua's parts): a
-- party follow or the in-game browser can take you from one server to the
-- next without the menu in between. If a watched setting changed, a prompt
-- lists them: Keep, Put back, or always put back for that server. Changes you
-- made yourself in that time show too; that's what Keep is for. Meowy
-- Network's own servers aren't second-guessed: what changes there is the
-- player's own choice in its FPS Booster, applied again each join (measured
-- on dev: 6 of its settings on every leave).
--
-- Parts waiting to be answered are taken newest first, each once the one
-- after it is answered (Guard.decide): servers joined back to back may have
-- changed the same setting, and each answers for its own step.
--
-- The session and the parts waiting are kept on disk (settings_guard_session.txt):
-- when the game closes on a server (a quit, a crash), the game has saved the
-- server's changes to config.cfg, and the next start asks about them (always,
-- even for a server always put back: the player may have changed things
-- since); a prompt not answered before a quit comes back too. Anything over a
-- week old is dropped.
--
-- The servers whose changes are always put back are in the pack's own file
-- (settings_guard_always.txt), not in a convar: any server's Lua can read a
-- convar, and the list names other servers you play on. Meowy settings lists
-- them, each with Remove.
--
-- And config.cfg, the game's saved settings and binds, is copied once per start
-- (config_backup.h): meowy_settings_backups lists the copies,
-- meowy_settings_restore <n> puts one back after a prompt at the main menu.
--
-- Runs in its own function at the end of resilience.lua's chunk (module.cpp),
-- with `SettingsGuard` and resilience.lua's locals in scope: UI, log,
-- menuVisible, overlay, settingsSections.
local Guard = SettingsGuard

local guardSetting = meowy.Setting and meowy.Setting("meowy_settings_guard", "1",
	"Meowy Client Pack: when you leave a server that changed your settings, show what changed and offer to put it back (0 = off)")
local function guardOn() return guardSetting == nil or guardSetting:GetBool() end
local function setGuard(on)
	-- Through the console: Garry's Mod refuses ConVar:SetBool on these menu convars.
	RunConsoleCommand("meowy_settings_guard", on and "1" or "0")
end

-- Values a put-back has sent that the game hasn't applied yet (ConsoleRun's
-- lines run at its next frame): read as already put back, so a snapshot or a
-- check right after one sees what the setting will be. Dropped once the game
-- has it (or after 5 s).
local inFlight = {}   -- name -> { value, untilAt }
local function cvar(name)
	local f = inFlight[name]
	if f then
		local actual = GetConVarString(name)
		if Guard.same(actual, f.value) or SysTime() > f.untilAt then
			inFlight[name] = nil
			return actual
		end
		return f.value
	end
	return GetConVarString(name)
end
local function sweepInFlight()
	for name in pairs(inFlight) do cvar(name) end
end

-- settings_guard_session.txt and settings_guard_always.txt (module.cpp's MenuFileRead/Write).
local function fileRead(kind) return meowy.MenuFileRead and meowy.MenuFileRead("settings_guard_" .. kind) or "" end
local function fileWrite(kind, text) if meowy.MenuFileWrite then meowy.MenuFileWrite("settings_guard_" .. kind, text) end end

-- Always put back: up to 20 servers, newest last.
local ALWAYS_MAX = 20
local always
local function alwaysList()
	if not always then always = Guard.parseList(fileRead("always")) end
	return always
end
local function saveAlways(list)
	always = list
	fileWrite("always", table.concat(list, "\n"))
end
-- 1.7.0 kept the list in the menu convar meowy_settings_guard_always (saved in
-- menu_settings.txt): moved into the file once, and the line taken out, so no
-- server can read it and a server removed here doesn't come back.
do
	local text = meowy.MenuSettingsRead and meowy.MenuSettingsRead() or ""
	local old = text:match("meowy_settings_guard_always=([^\r\n]*)")
	if old then
		local list = alwaysList()
		for _, a in ipairs(Guard.parseList(old)) do
			if not Guard.listHas(list, a) then list = Guard.listAdd(list, a, ALWAYS_MAX) end
		end
		saveAlways(list)
		local rest = text:gsub("[^\r\n]*meowy_settings_guard_always=[^\r\n]*\r?\n?", "")
		if meowy.MenuSettingsWrite then meowy.MenuSettingsWrite(rest) end
		log("settings guard: the always-put-back list moved to the pack's own file (%d servers)", #list)
	end
end

local function isOurs(address) return meowy.IsOurServer ~= nil and meowy.IsOurServer(address) == true end

-- Only settings the game saves to config.cfg stay changed after a restart.
local ARCHIVE = FCVAR_ARCHIVE or 128
local function archived(name)
	local cv = GetConVar and GetConVar(name)
	if not (cv and cv.IsFlagSet) then return true end
	return cv:IsFlagSet(ARCHIVE)
end

-- Only plain numbers are run as commands: every watched setting is one, and a
-- value a server left behind can't carry a second command. writeConfig: the
-- values were saved already (a part from a game that closed), so config.cfg
-- is written again at once, and a crash later this session can't bring the
-- server's values back.
local function plainNumber(v) return isstring(v) and v:match("^%-?%d*%.?%d+$") ~= nil end
local function putBack(changes, writeConfig)
	local n = 0
	for _, c in ipairs(changes) do
		if plainNumber(c.was) then
			if meowy.ConsoleRun then meowy.ConsoleRun(c.name .. " " .. c.was) else RunConsoleCommand(c.name, c.was) end
			inFlight[c.name] = { value = c.was, untilAt = SysTime() + 5 }
			n = n + 1
		end
	end
	if writeConfig and n > 0 and meowy.ConsoleRun then meowy.ConsoleRun("host_writeconfig") end
	return n
end

-- The parts waiting to be answered, oldest first ({ address, name, changes, at,
-- closed }; closed: from the last run), and the session running now
-- ({ cur = { snap, address, name, at }, menuSince }). Both are on disk.
local pending, session = {}, nil
local function save()
	local any = #pending > 0 or session ~= nil
	fileWrite("session", any and Guard.encodeSession({ parts = pending, cur = session and session.cur }) or "")
end
local function answered(part)
	for i = #pending, 1, -1 do
		if pending[i] == part then table.remove(pending, i) end
	end
	save()
end

-- The last run's: its parts waiting (never answered), and the server the game
-- closed on (`leftover`, settled once config.cfg has run, a few seconds after
-- start, or as the next join starts). Over a week old: dropped.
local leftover
do
	local s = Guard.decodeSession(fileRead("session"))
	local now, dropped = os.time(), 0
	for _, p in ipairs(s and s.parts or {}) do
		if Guard.fresh(p.at, now) and #p.changes > 0 then
			p.closed = true
			pending[#pending + 1] = p
		else
			dropped = dropped + 1
		end
	end
	if s and s.cur then
		if Guard.fresh(s.cur.at, now) then leftover = s.cur else dropped = dropped + 1 end
	end
	if dropped > 0 then log("settings guard: %d left from an earlier run dropped (over a week old)", dropped) end
end
local START, SETTLE = SysTime(), 8

-- The prompt for the newest part waiting: `still`, its changes still in place.
local function whoOf(part)
	local who = isstring(part.name) and part.name ~= "" and part.name or part.address or "the last server"
	if #who > 48 then who = who:sub(1, 45) .. "..." end
	return who
end
local function ask(part, still)
	local buttons = { { "Keep", "ghost", function()
		answered(part)
		log("settings guard: kept %d changes from %s", #still, tostring(part.address))
	end } }
	if part.address then
		buttons[#buttons + 1] = { "Always put back here", "secondary", function()
			answered(part)
			saveAlways(Guard.listAdd(alwaysList(), part.address, ALWAYS_MAX))
			log("settings guard: put back %d settings; %s's changes are put back from now on", putBack(still, part.closed), part.address)
		end }
	end
	buttons[#buttons + 1] = { "Put back", "primary", function()
		answered(part)
		log("settings guard: put back %d settings changed on %s", putBack(still, part.closed), tostring(part.address))
	end }
	UI.Prompt({
		width = 600,
		icon = { "alert", "amber" },
		title = string.format("%d %s changed while you were on %s", #still, #still == 1 and "setting" or "settings", whoOf(part)),
		body = Guard.describe(still, 8),
		note = (part.gameClosed and "The game closed while you were there, so they were saved. " or "")
			.. "Saved settings follow you to every server until they're changed back. Put back sets each to what it "
			.. "was when you joined. Changed one yourself, in Options? Keep leaves them all as they are.",
		closable = true,
		buttons = buttons,
		onDismiss = function() answered(part) end,   -- Escape or the "x": kept
	})
end

-- A server's part ended (another server joined, or back at the menu): kept
-- (ours), put back at once (always; returns those changes), or waiting to be asked.
local function ended(part)
	part.at = os.time()
	local v = Guard.verdict(part, isOurs, alwaysList())
	if v == "putback" then
		log("settings guard: put back %d settings %s changed (always for this server)", putBack(part.changes), part.address)
		return part.changes
	elseif v == "ask" then
		log("settings guard: %s changed %d settings", tostring(part.address), #part.changes)
		pending[#pending + 1] = part
	elseif v == "keep" then
		log("settings guard: %d changes on %s kept (Meowy Network's own server)", #part.changes, part.address)
	end
end

-- The server the game closed on last run: what's still saved of its changes
-- (only settings config.cfg keeps), waiting to be asked about.
local function settleLeftover()
	local part = Guard.closePart(leftover, cvar)
	leftover = nil
	part.changes = Guard.only(part.changes, archived)
	part.at, part.closed, part.gameClosed = os.time(), true, true
	if #part.changes > 0 then
		log("settings guard: the game closed on %s with %d of its changes saved", tostring(part.address), #part.changes)
		pending[#pending + 1] = part
	end
	save()
end

-- The newest part waiting, once nothing is in the way: asked, put back, or done with.
local function nextPending()
	local part = pending[#pending]
	local v, still = Guard.decide(part, cvar, isOurs, alwaysList())
	if v == "ask" then return ask(part, still) end
	answered(part)
	if v == "putback" then
		log("settings guard: put back %d settings %s changed (always for this server)", putBack(still), part.address)
	elseif v == "keep" then
		log("settings guard: %d changes on %s kept (Meowy Network's own server)", #still, part.address)
	end
end

local nextCheck, menuShownSince = 0, nil
hook.Add("Think", "MeowyClientPack.SettingsGuard", function()
	local now = SysTime()
	local loading, inGame = IsInLoading(), IsInGame()
	if loading or inGame then menuShownSince = nil else menuShownSince = menuShownSince or now end
	if now < nextCheck then return end
	nextCheck = now + 0.5
	sweepInFlight()
	if not guardOn() then
		if session or leftover or #pending > 0 then
			session, leftover, pending = nil, nil, {}
			save()
		end
		return
	end
	if leftover and (loading or inGame or now - START >= SETTLE) then settleLeftover() end
	if loading or inGame then
		local changed = false
		if not session then
			session = { cur = { snap = Guard.snapshot(cvar), at = os.time() } }
			changed = true
		end
		session.menuSince = nil
		-- From the moment the connection is up, loading included: a join cut
		-- short still names the server (measured: one left a setting changed
		-- after 4 minutes of loading, never in game).
		local address = meowy.ServerNow and meowy.ServerNow()
		if isstring(address) then
			local cur = session.cur
			if cur.address and address ~= cur.address then
				-- Another server, without the menu in between: the last one's part ends here.
				ended(Guard.closePart(cur, cvar))
				session.cur = { snap = Guard.snapshot(cvar), address = address, previousName = cur.name, at = os.time() }
				cur = session.cur
				changed = true
			elseif not cur.address then
				cur.address = address
				changed = true
			end
			-- The name the join reported; right after a hop it's still the last server's for a moment.
			local name = meowy.JoiningName
			if isstring(name) and name ~= "" and name ~= cur.name and name ~= cur.previousName then
				cur.name = name
				changed = true
			end
		end
		if changed then save() end
		return
	end
	if session then
		session.menuSince = session.menuSince or now
		-- Back at the menu for a moment and not reconnecting: the session is over.
		if now - session.menuSince < 3 or IsValid(overlay) or not menuVisible() then return end
		local s = session
		session = nil
		ended(Guard.closePart(s.cur, cvar))
		save()
	end
	-- Not over another window of the pack's (another of these, the start-up
	-- questions, Meowy settings), and not before a put-back has run.
	if #pending > 0 and menuVisible() and not IsValid(overlay) and not IsValid(UI.current) and next(inFlight) == nil then
		nextPending()
	end
end)

-- Meowy settings: the switch, and the servers always put back, each with Remove.
local function serverRow(address, onRemove)
	local p = vgui.Create("DPanel")
	p.text = UI.Text(p, address, "MeowyPack.Body", UI.C.text)
	p.button = UI.Button(p, "Remove", "secondary", function() onRemove(p) end)
	function p:Fit(width)
		local th = self.text:Fit(width - self.button.w - UI.S(12))
		self.h = math.max(th, self.button.h)
		self.text:SetPos(0, math.floor((self.h - th) / 2))
		self.button:SetPos(width - self.button.w, math.floor((self.h - self.button.h) / 2))
		self:SetSize(width, self.h)
		return self.h
	end
	p.Paint = function() return true end
	return p
end
if settingsSections then
	settingsSections[#settingsSections + 1] = function(refresh)
		local label = UI.Label(nil, "Settings guard")
		label.gapBefore = UI.S(22)
		local blocks = { label }
		local t = UI.Toggle(nil, "Ask when a server changed your settings",
			"When you leave a server whose Lua changed your graphics, network or other settings, they're listed with "
			.. "Put back. If the game closed while you were there, you're asked at the next start.",
			guardOn(), function(on) setGuard(on) end)
		t.meowyToggle = "settings_guard"
		t.gapBefore = UI.S(10)
		blocks[#blocks + 1] = t
		local list = alwaysList()
		local note = UI.Text(nil, #list == 0 and "No server's changes are put back without asking."
			or "These servers' changes are always put back, without asking:", "MeowyPack.Small", UI.C.dim)
		note.gapBefore = UI.S(10)
		blocks[#blocks + 1] = note
		for _, address in ipairs(list) do
			blocks[#blocks + 1] = serverRow(address, function()
				saveAlways(Guard.listRemove(alwaysList(), address))
				log("settings guard: %s's changes are asked about again", address)
				if refresh then refresh() end
			end)
		end
		return blocks
	end
end

-- The backups: one copy of config.cfg per start, when it changed.
if meowy.ConfigBackup then
	local ok, what = meowy.ConfigBackup()
	if ok == false then log("settings backup: %s", tostring(what)) end
end

-- What a command does to the player's settings happens only at the main menu,
-- the menu up for a moment with no join running, and only after a prompt the
-- player answers there: a server's Lua can run these commands too (and
-- disconnect first, or open the console), but can't click.
local function atMenu()
	return not IsInGame() and not IsInLoading() and menuShownSince ~= nil and SysTime() - menuShownSince >= 2
end

-- meowy_restore.cfg (the player's binds and settings) stays in the game's cfg
-- folder only until the game has run it. Its last line (config_backup.cpp) is
-- meowy_restore_done, so the file is removed as soon as that runs: after every
-- line before it. If it hasn't run within RESTORE_FILE_SECONDS, it's removed
-- anyway; one left behind (the game closed in between) goes at start.
local RESTORE_FILE_SECONDS = 30
local function removeRestoreFile()
	if meowy.ConfigRestoreDone and not meowy.ConfigRestoreDone() then
		log("settings restore: couldn't remove meowy_restore.cfg from the game's cfg folder")
	end
end
removeRestoreFile()
local removeBy   -- SysTime() by which the file goes, while a restore's exec is under way
if concommand then
	concommand.Add("meowy_restore_done", function()
		if not removeBy then return end   -- no restore under way: nothing to do
		removeBy = nil
		removeRestoreFile()
	end, nil, "Meowy Client Pack: internal (the last line of meowy_restore.cfg)")
end
hook.Add("Think", "MeowyClientPack.SettingsRestoreFile", function()
	if not removeBy or SysTime() < removeBy then return end
	removeBy = nil
	removeRestoreFile()
	log("settings restore: meowy_restore.cfg removed; its last line didn't run within %d s", RESTORE_FILE_SECONDS)
end)

local restoring
hook.Add("Think", "MeowyClientPack.SettingsRestore", function()
	if not restoring or SysTime() < restoring.at then return end
	local b = restoring.b
	restoring = nil
	if not atMenu() then return end
	-- The copy to put back is written out first (meowy_restore.cfg): copying the
	-- settings now can make an 11th copy and drop the oldest, which may be this one.
	local done, file = meowy.ConfigRestore(b.name)
	if not done then
		MsgN("[Meowy Client Pack] settings restore: couldn't (" .. tostring(file) .. ")")
		return
	end
	local ok, what = meowy.ConfigBackup()
	if ok == false then
		removeRestoreFile()
		MsgN("[Meowy Client Pack] settings restore: your settings now couldn't be copied first (" .. tostring(what) .. "), so nothing was put back")
		return
	end
	removeBy = SysTime() + RESTORE_FILE_SECONDS
	meowy.ConsoleRun("exec " .. file)
	MsgN("[Meowy Client Pack] settings restore: put back your settings and binds from " .. b.when
		.. (what == "copied" and "; the ones you had are the newest copy in meowy_settings_backups" or ""))
end)
local function restore(b)
	-- The settings and binds as they are now go into a copy first (the game
	-- writes config.cfg, then it's copied), so a restore can always be undone.
	if meowy.ConsoleRun then meowy.ConsoleRun("host_writeconfig") end
	restoring = { b = b, at = SysTime() + 0.5 }
end

if concommand then
	concommand.Add("meowy_settings_backups", function()
		local list = meowy.ConfigBackups and meowy.ConfigBackups() or {}
		if #list == 0 then
			MsgN("[Meowy Client Pack] settings backups: none yet (one is made each start, when config.cfg changed)")
		else
			MsgN("[Meowy Client Pack] settings backups (config.cfg as it was at each start, newest first):")
			for i, b in ipairs(list) do MsgN(string.format("  %d. %s (%d bytes)", i, b.when, b.size)) end
			MsgN("  meowy_settings_restore <number> puts one back (your binds and settings), at the main menu")
		end
		MsgN("[Meowy Client Pack] settings guard: " .. (guardOn() and "on" or "off") .. "; always put back for: "
			.. (#alwaysList() > 0 and table.concat(alwaysList(), ", ") or "no server")
			.. " (Meowy settings, or meowy_settings_guard_forget <address>, takes one off)")
	end, nil, "Meowy Client Pack: list the copies of your settings (config.cfg)")
	concommand.Add("meowy_settings_restore", function(_, _, args)
		if not atMenu() then
			MsgN("[Meowy Client Pack] settings restore: only at the main menu")
			return
		end
		local list = meowy.ConfigBackups and meowy.ConfigBackups() or {}
		local b = list[math.floor(tonumber(args and args[1]) or 0)]
		if not b then
			MsgN("[Meowy Client Pack] settings restore: give a number from meowy_settings_backups")
			return
		end
		UI.Prompt({
			width = 560,
			icon = { "alert", "amber" },
			title = "Put back your settings from " .. b.when .. "?",
			body = "Your binds and settings go back to how they were then. The ones you have now are copied first, "
				.. "so meowy_settings_backups can bring them back.",
			closable = true,
			buttons = {
				{ "Cancel", "ghost" },
				{ "Put back", "primary", function() if atMenu() then restore(b) end end },
			},
		})
	end, nil, "Meowy Client Pack: put back a copy of your settings (meowy_settings_backups lists them)")
	-- The whole argument string: the console splits "a.b.c.d:port" at the colon.
	concommand.Add("meowy_settings_guard_forget", function(_, _, _, argStr)
		if not atMenu() then
			MsgN("[Meowy Client Pack] settings guard: only at the main menu")
			return
		end
		local address = isstring(argStr) and (argStr:gsub("%s", "")) or ""
		if not Guard.listHas(alwaysList(), address) then
			MsgN("[Meowy Client Pack] settings guard: " .. address .. " isn't on the always-put-back list")
			return
		end
		UI.Prompt({
			width = 520,
			icon = { "alert", "amber" },
			title = "Ask again about " .. address .. "?",
			body = "Its changes to your settings are put back without asking now. From here on you're asked each time.",
			closable = true,
			buttons = {
				{ "Cancel", "ghost" },
				{ "Ask again", "primary", function()
					saveAlways(Guard.listRemove(alwaysList(), address))
					MsgN("[Meowy Client Pack] settings guard: " .. address .. "'s changes are asked about again")
				end },
			},
		})
	end, nil, "Meowy Client Pack: stop always putting back a server's changes to your settings")
end
