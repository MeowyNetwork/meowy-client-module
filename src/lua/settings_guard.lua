-- The settings guard: what a server changed in your settings, put back in one
-- click. A session starts when a join starts (the snapshot is taken before any
-- server Lua runs) and ends a few seconds after you're back at the main menu
-- and not reconnecting (resilience.lua's card is down). If a watched setting
-- changed (settings_guard_logic.lua), a prompt lists them: Keep, Put back, or
-- always put back for that server. Changes you made yourself in that time show
-- too; that's what Keep is for. Meowy Network's own servers aren't second-
-- guessed: what changes there is the player's own choice in its FPS Booster,
-- applied again each join (measured on dev: 6 of its settings on every leave).
--
-- And config.cfg, the game's saved settings and binds, is copied once per start
-- (config_backup.h): meowy_settings_backups lists the copies,
-- meowy_settings_restore <n> puts one back.
--
-- Runs in its own function at the end of resilience.lua's chunk (module.cpp),
-- with `SettingsGuard` and resilience.lua's locals in scope: UI, log,
-- menuVisible, overlay.
local Guard = SettingsGuard

local guardSetting = meowy.Setting and meowy.Setting("meowy_settings_guard", "1",
	"Meowy Client Pack: when you leave a server that changed your settings, show what changed and offer to put it back (0 = off)")
local alwaysSetting = meowy.Setting and meowy.Setting("meowy_settings_guard_always", "",
	"Meowy Client Pack: servers whose changes to your settings are always put back (addresses, set from the prompt)")
local function guardOn() return guardSetting == nil or guardSetting:GetBool() end
local function alwaysList() return Guard.parseList(alwaysSetting and alwaysSetting:GetString() or "") end

local function cvar(name) return GetConVarString(name) end

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

local function ask(changes, s)
	local who = isstring(s.name) and s.name ~= "" and s.name or s.address or "The last server"
	if #who > 48 then who = who:sub(1, 45) .. "..." end
	local buttons = { { "Keep", "ghost", function() log("settings guard: kept %d changes from %s", #changes, tostring(s.address)) end } }
	if s.address then
		buttons[#buttons + 1] = { "Always put back here", "secondary", function()
			local list = Guard.listAdd(alwaysList(), s.address, 20)
			RunConsoleCommand("meowy_settings_guard_always", table.concat(list, " "))
			log("settings guard: put back %d settings; %s's changes are put back from now on", putBack(changes), s.address)
		end }
	end
	buttons[#buttons + 1] = { "Put back", "primary", function()
		log("settings guard: put back %d settings %s changed", putBack(changes), tostring(s.address))
	end }
	UI.Prompt({
		width = 600,
		icon = { "alert", "amber" },
		title = string.format("%s changed %d of your settings", who, #changes),
		body = Guard.describe(changes, 8),
		note = "Saved settings follow you to every server until they're changed back. "
			.. "Put back sets them to what they were when you joined.",
		closable = true,
		buttons = buttons,
	})
end

local session   -- { snap, name, address, menuSince } from a join's start until back at the menu
local nextCheck = 0
hook.Add("Think", "MeowyClientPack.SettingsGuard", function()
	local now = SysTime()
	if now < nextCheck then return end
	nextCheck = now + 0.5
	local loading, inGame = IsInLoading(), IsInGame()
	if loading or inGame then
		if not session and guardOn() then session = { snap = Guard.snapshot(cvar) } end
		if session then
			session.menuSince = nil
			if inGame and not loading then
				local address = meowy.ServerNow and meowy.ServerNow()
				if isstring(address) then session.address = address end
				if isstring(meowy.JoiningName) and meowy.JoiningName ~= "" then session.name = meowy.JoiningName end
			end
		end
		return
	end
	if not session then return end
	session.menuSince = session.menuSince or now
	-- Back at the menu for a moment and not reconnecting: the session is over.
	if now - session.menuSince < 3 or IsValid(overlay) or not menuVisible() then return end
	local s = session
	session = nil
	if not guardOn() then return end
	if s.address and meowy.IsOurServer and meowy.IsOurServer(s.address) then return end
	local changes = Guard.diff(s.snap, cvar)
	if #changes == 0 then return end
	if s.address and Guard.listHas(alwaysList(), s.address) then
		log("settings guard: put back %d settings %s changed (always for this server)", putBack(changes), s.address)
		return
	end
	log("settings guard: %s changed %d settings", tostring(s.address), #changes)
	ask(changes, s)
end)

-- The backups: one copy of config.cfg per start, when it changed.
if meowy.ConfigBackup then
	local ok, what = meowy.ConfigBackup()
	if ok == false then log("settings backup: %s", tostring(what)) end
end

if concommand then
	concommand.Add("meowy_settings_backups", function()
		local list = meowy.ConfigBackups and meowy.ConfigBackups() or {}
		if #list == 0 then
			MsgN("[Meowy Client Pack] settings backups: none yet (one is made each start, when config.cfg changed)")
		else
			MsgN("[Meowy Client Pack] settings backups (config.cfg as it was at each start, newest first):")
			for i, b in ipairs(list) do MsgN(string.format("  %d. %s (%d bytes)", i, b.when, b.size)) end
			MsgN("  meowy_settings_restore <number> puts one back (your binds and settings)")
		end
		MsgN("[Meowy Client Pack] settings guard: " .. (guardOn() and "on" or "off") .. "; always put back for: "
			.. (#alwaysList() > 0 and table.concat(alwaysList(), ", ") or "no server"))
	end, nil, "Meowy Client Pack: list the copies of your settings (config.cfg)")
	concommand.Add("meowy_settings_restore", function(_, _, args)
		local list = meowy.ConfigBackups and meowy.ConfigBackups() or {}
		local b = list[math.floor(tonumber(args and args[1]) or 0)]
		if not b then
			MsgN("[Meowy Client Pack] settings restore: give a number from meowy_settings_backups")
			return
		end
		local ok, why = meowy.ConfigRestore(b.name)
		if not ok then
			MsgN("[Meowy Client Pack] settings restore: couldn't (" .. tostring(why) .. ")")
			return
		end
		meowy.ConsoleRun("exec " .. why)
		MsgN("[Meowy Client Pack] settings restore: put back your settings and binds from " .. b.when)
	end, nil, "Meowy Client Pack: put back a copy of your settings (meowy_settings_backups lists them)")
end
