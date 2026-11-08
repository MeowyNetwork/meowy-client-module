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
-- The session is kept on disk while it runs (settings_guard_session.txt): when
-- the game closes on a server (a quit, a crash), the game has saved the
-- server's changes to config.cfg, and the next start asks about them.
--
-- The servers whose changes are always put back are in the pack's own file
-- (settings_guard_always.txt), not in a convar: any server's Lua can read a
-- convar, and the list names other servers you play on. Meowy settings lists
-- them, each with Remove.
--
-- And config.cfg, the game's saved settings and binds, is copied once per start
-- (config_backup.h): meowy_settings_backups lists the copies,
-- meowy_settings_restore <n> puts one back (at the main menu, typed in the
-- console; never from a server).
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

local function cvar(name) return GetConVarString(name) end
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
-- value a server left behind can't carry a second command.
local function plainNumber(v) return isstring(v) and v:match("^%-?%d*%.?%d+$") ~= nil end
local function putBack(changes)
	local n = 0
	for _, c in ipairs(changes) do
		if plainNumber(c.was) then
			if meowy.ConsoleRun then meowy.ConsoleRun(c.name .. " " .. c.was) else RunConsoleCommand(c.name, c.was) end
			n = n + 1
		end
	end
	return n
end

-- The prompts, one at a time (each a window of UI's, so UI.current), at the main menu.
local prompts = {}
local function whoOf(part)
	local who = isstring(part.name) and part.name ~= "" and part.name or part.address or "the last server"
	if #who > 48 then who = who:sub(1, 45) .. "..." end
	return who
end
local function ask(part)
	local changes = part.changes
	local buttons = { { "Keep", "ghost", function()
		log("settings guard: kept %d changes from %s", #changes, tostring(part.address))
	end } }
	if part.address then
		buttons[#buttons + 1] = { "Always put back here", "secondary", function()
			saveAlways(Guard.listAdd(alwaysList(), part.address, ALWAYS_MAX))
			log("settings guard: put back %d settings; %s's changes are put back from now on", putBack(changes), part.address)
		end }
	end
	buttons[#buttons + 1] = { "Put back", "primary", function()
		log("settings guard: put back %d settings changed on %s", putBack(changes), tostring(part.address))
	end }
	UI.Prompt({
		width = 600,
		icon = { "alert", "amber" },
		title = string.format("%d %s changed while you were on %s", #changes, #changes == 1 and "setting" or "settings", whoOf(part)),
		body = Guard.describe(changes, 8),
		note = (part.closed and "The game closed while you were there, so they were saved. " or "")
			.. "Saved settings follow you to every server until they're changed back. Put back sets each to what it "
			.. "was when you joined. Changed one yourself, in Options? Keep leaves them all as they are.",
		closable = true,
		buttons = buttons,
	})
end

-- A part that ended: kept (ours), put back at once (always), or asked about.
local function settle(part)
	local v = Guard.verdict(part, isOurs, alwaysList())
	if v == "putback" then
		log("settings guard: put back %d settings %s changed (always for this server)", putBack(part.changes), part.address)
		return part.changes
	elseif v == "ask" then
		log("settings guard: %s changed %d settings", tostring(part.address), #part.changes)
		prompts[#prompts + 1] = part
	elseif v == "keep" then
		log("settings guard: %d changes on %s kept (Meowy Network's own server)", #part.changes, part.address)
	end
end

-- A session that ended: the parts that ended before it (still in place), then the last server's.
-- closed: the game closed on that server (settled at the next start).
local function finish(s, closed)
	for _, p in ipairs(s.parts or {}) do
		local still = Guard.stillChanged(p.changes, cvar)
		if #still > 0 then settle({ address = p.address, name = p.name, changes = still }) end
	end
	if s.cur then
		local last = Guard.closePart(s.cur, cvar)
		if closed then
			last.changes = Guard.only(last.changes, archived)
			last.closed = true
		end
		settle(last)
	end
end

local function save(s) fileWrite("session", s and Guard.encodeSession(s) or "") end

-- Left by a game that closed during a session; settled once config.cfg has run
-- (a few seconds after start), or as the next join starts.
local leftover = Guard.decodeSession(fileRead("session"))
local START, SETTLE = SysTime(), 8

local session   -- { parts, cur = { snap, address, name }, menuSince } from a join's start until back at the menu
local nextCheck, menuShownSince = 0, nil
hook.Add("Think", "MeowyClientPack.SettingsGuard", function()
	local now = SysTime()
	local loading, inGame = IsInLoading(), IsInGame()
	if loading or inGame then menuShownSince = nil else menuShownSince = menuShownSince or now end
	if now < nextCheck then return end
	nextCheck = now + 0.5
	if leftover and (loading or inGame or now - START >= SETTLE) then
		local s = leftover
		leftover = nil
		if guardOn() then finish(s, true) end
		save(session)
	end
	if loading or inGame then
		if not guardOn() then
			if session then session = nil save(nil) end
			return
		end
		local changed = false
		if not session then
			session = { parts = {}, cur = { snap = Guard.snapshot(cvar) } }
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
				local part = Guard.closePart(cur, cvar)
				local putNow
				if Guard.verdict(part, isOurs, alwaysList()) == "ask" then
					-- Asked about at the session's end, the changes still in place then.
					log("settings guard: %s changed %d settings before you joined %s", tostring(part.address), #part.changes, address)
					session.parts[#session.parts + 1] = part
				else
					putNow = settle(part)
				end
				session.cur = { snap = Guard.snapshotAfter(cvar, putNow), address = address, previousName = cur.name }
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
		if changed then save(session) end
		return
	end
	if session then
		session.menuSince = session.menuSince or now
		-- Back at the menu for a moment and not reconnecting: the session is over.
		if now - session.menuSince >= 3 and not IsValid(overlay) and menuVisible() then
			local s = session
			session = nil
			save(nil)
			if guardOn() then finish(s, false) end
		end
	end
	-- Not over another window of the pack's (another of these, the start-up questions, Meowy settings).
	if #prompts > 0 and not session and menuVisible() and not IsValid(overlay) and not IsValid(UI.current) then
		ask(table.remove(prompts, 1))
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

-- A copy is put back only at the main menu, with no join running and the menu
-- up for a moment, and only when typed in a console that's open: a server's Lua
-- can run this command too, and can disconnect first in the same breath.
local function consoleOpen()
	if gui and gui.IsConsoleVisible and gui.IsConsoleVisible() then return true end
	local world = vgui and vgui.GetWorldPanel and vgui.GetWorldPanel()
	for _, p in ipairs(world and world.GetChildren and world:GetChildren() or {}) do
		if IsValid(p) and p.meowyConsole and p:IsVisible() then return true end   -- the Meowy console (console.lua)
	end
	return false
end
local function atMenu()
	return not IsInGame() and not IsInLoading() and menuShownSince ~= nil and SysTime() - menuShownSince >= 2
end
local restoring
local function restore(b)
	-- The settings and binds as they are now go into a copy first (the game
	-- writes config.cfg, then it's copied), so a restore can always be undone.
	if meowy.ConsoleRun then meowy.ConsoleRun("host_writeconfig") end
	restoring = { b = b, at = SysTime() + 0.5 }
end
hook.Add("Think", "MeowyClientPack.SettingsRestore", function()
	if not restoring or SysTime() < restoring.at then return end
	local b = restoring.b
	restoring = nil
	if not atMenu() then return end
	local ok, what = meowy.ConfigBackup()
	if ok == false then
		MsgN("[Meowy Client Pack] settings restore: your settings now couldn't be copied first (" .. tostring(what) .. "), so nothing was put back")
		return
	end
	local done, why = meowy.ConfigRestore(b.name)
	if not done then
		MsgN("[Meowy Client Pack] settings restore: couldn't (" .. tostring(why) .. ")")
		return
	end
	meowy.ConsoleRun("exec " .. why)
	MsgN("[Meowy Client Pack] settings restore: put back your settings and binds from " .. b.when
		.. (what == "copied" and "; the ones you had are the newest copy in meowy_settings_backups" or ""))
end)

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
		if not atMenu() or not consoleOpen() then
			MsgN("[Meowy Client Pack] settings restore: only at the main menu, typed in the console")
			return
		end
		local list = meowy.ConfigBackups and meowy.ConfigBackups() or {}
		local b = list[math.floor(tonumber(args and args[1]) or 0)]
		if not b then
			MsgN("[Meowy Client Pack] settings restore: give a number from meowy_settings_backups")
			return
		end
		restore(b)
	end, nil, "Meowy Client Pack: put back a copy of your settings (meowy_settings_backups lists them)")
	-- The whole argument string: the console splits "a.b.c.d:port" at the colon.
	concommand.Add("meowy_settings_guard_forget", function(_, _, _, argStr)
		local address = isstring(argStr) and (argStr:gsub("%s", "")) or ""
		if not Guard.listHas(alwaysList(), address) then
			MsgN("[Meowy Client Pack] settings guard: " .. address .. " isn't on the always-put-back list")
			return
		end
		saveAlways(Guard.listRemove(alwaysList(), address))
		MsgN("[Meowy Client Pack] settings guard: " .. address .. "'s changes are asked about again")
	end, nil, "Meowy Client Pack: stop always putting back a server's changes to your settings")
end
