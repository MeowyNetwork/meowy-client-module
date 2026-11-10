-- The pack's settings (meowy.Setting), the first menu chunk module.cpp runs.
-- The game saves client-realm Lua convars, not menu-state ones, so each is kept
-- in the pack's own menu_settings.txt ("name=value" lines): read here, before
-- any is created, and written within a second of a change. Every menu chunk
-- creates its settings with meowy.Setting.
--
-- Servers can't change the settings that guard the player. A server's client
-- Lua reaches every console variable through RunConsoleCommand (Garry's Mod
-- only blocks a fixed list of names there, and no flag stops it), so for the
-- PROTECTED settings the pack keeps the value it approved and puts back any
-- other change before anything reads it or it is saved. A change is approved
-- when the menu itself makes it (Meowy settings, the Meowy console, the
-- prompts: menu Lua's RunConsoleCommand and meowy.ConsoleRun, which no server
-- can call), or when it happens before any join this start (the player's own
-- config files and the game's console; no server Lua has run yet). After a
-- join, a change from anywhere else is put back: a server can queue commands
-- that run after `disconnect`, so "at the main menu" doesn't tell the player's
-- typing from a server's leftovers.
--
-- And servers can't read them: every setting is created hidden (FCVAR_HIDDEN),
-- which keeps it out of client-realm GetConVar, GetConVarString and
-- ConVarExists (measured in game, 2026-11-08), and out of find, cvarlist and the
-- console's completion. The menu's own GetConVar, GetConVarString and
-- ConVarExists hide them as well, so here they answer a pack setting's name with
-- its setting object (the policy and the put-back included); the Meowy console
-- lists them from meowy.SettingsList.
local meowy = meowy
local FCVAR_HIDDEN_FLAG = 16   -- FCVAR_HIDDEN; the menu state has no FCVAR_HIDDEN global

-- menu_settings.txt: one "name=value" per line (LF; a CR before it is
-- dropped), the first line for a name counts. Only whole lines: a value can't
-- hold a second setting (and none is ever written with a control character in
-- it, see clean below).
local saved, names, cvs, helps, defaults = {}, {}, {}, {}, {}
local function readSaved(text)
	local out = {}
	for line in string.gmatch(text .. "\n", "([^\n]*)\n") do
		line = string.gsub(line, "\r$", "")
		local name, value = string.match(line, "^([%w_]+)=(.*)$")
		if name and out[name] == nil then out[name] = value end
	end
	return out
end
saved = readSaved(meowy.MenuSettingsRead and meowy.MenuSettingsRead() or "")
-- A value as it's written: control characters (a CR or tab a server's
-- RunConsoleCommand put in an unprotected setting) left out.
local function clean(value) return (string.gsub(tostring(value or ""), "%c", "")) end

-- What a protected setting may hold: a number, or for the guard's list,
-- addresses separated by spaces. Anything else (quotes, ";") is never kept.
-- Protected: what a hostile server gains from switching off, or on (the data
-- sandbox, updates, the settings guard, parties, the server-list reports,
-- Discord status, and what tells players a server lies about its player count:
-- the honest list, Meowy's server index and the Meowy home with its Find servers).
local function number(v) return string.match(v, "^%-?%d+%.?%d*$") ~= nil end
local function addresses(v) return #v <= 1024 and string.match(v, "^[%w%.:%[%]_ %-]*$") ~= nil end
local PROTECTED = {
	meowy_data_sandbox = number,
	meowy_updates = number,
	meowy_settings_guard = number,
	meowy_settings_guard_always = addresses,   -- a convar until #75's file; harmless after
	meowy_party = number,
	meowy_serverlist_reports = number,
	meowy_discord = number,
	meowy_honest_serverlist = number,
	meowy_server_index = number,
	meowy_menu = number,
	meowy_menu_version = number,   -- below "2", the next start switches the Meowy home back on (resilience.lua)
	-- Multicore rendering (multicore.lua): only the player turns it on or off,
	-- so a change of it is theirs, and resets its crash guard.
	meowy_multicore = number,
}

-- Meowy Network's signed policy (policy.h) can turn a setting off ("off
-- meowy_pak_cache"), or the whole pack: such a setting reads as off here,
-- whatever the player chose (their own value is kept and comes back when
-- the policy lets it). Every feature that reads its setting follows.
local policyOff, policyAt, policySeq = {}, -math.huge, nil
local function killed(name)
	local now = SysTime()
	if now - policyAt > 2 then
		policyAt = now
		local st = meowy.PolicyState and meowy.PolicyState()
		policyOff = {}
		if istable(st) then
			for _, n in ipairs(st.off or {}) do policyOff[n] = true end
			policyOff["*"] = st.pack_enabled == false
			if st.seq ~= policySeq then
				if policySeq ~= nil and isstring(st.notice) and st.notice ~= "" then
					print("[Meowy Client Pack] Meowy Network: " .. st.notice)
				end
				policySeq = st.seq
			end
		end
	end
	return policyOff[name] == true or policyOff["*"] == true
end

-- --- Approved changes ------------------------------------------------------------

local kept = {}         -- protected name -> the value the pack approved
local approvals = {}    -- protected name -> { value = <normalized>, untilT = SysTime() }
local lastRevert = {}   -- protected name -> SysTime() of the last put-back sent
local told = {}         -- protected name -> true once this join's put-back was logged
local joinedEver, wasJoining = false, false
local APPROVAL_SECONDS = 10

-- Quotes dropped and spaces collapsed: the engine's own handling of a
-- command's arguments differs from how they were passed, not in this.
local function normalized(v)
	return (string.gsub(string.gsub(tostring(v or ""), '"', ""), "%s+", " "):match("^%s*(.-)%s*$"))
end
local function approve(name, value)
	name = string.lower(normalized(name))
	if PROTECTED[name] then approvals[name] = { value = normalized(value), untilT = SysTime() + APPROVAL_SECONDS } end
end
-- A console line the menu runs: each command in it (";" or a line break
-- outside quotes ends one) approves its own value.
local function approveLine(line)
	if not isstring(line) then return end
	local cur, quoted = {}, false
	local function finish()
		local name, rest = string.match(table.concat(cur), "^%s*(%S+)%s*(.-)%s*$")
		if name then approve(name, rest) end
		cur = {}
	end
	for ch in string.gmatch(line, ".") do
		if ch == '"' then quoted = not quoted end
		if (ch == ";" and not quoted) or ch == "\n" or ch == "\r" then finish() else cur[#cur + 1] = ch end
	end
	finish()
end

-- The menu's own ways to run commands approve what they set. A server's
-- client Lua has its own RunConsoleCommand and no meowy.ConsoleRun.
local rawRun = RunConsoleCommand
RunConsoleCommand = function(cmd, ...)
	local parts = {}
	for i = 1, select("#", ...) do parts[i] = tostring((select(i, ...))) end
	approve(cmd, table.concat(parts, " "))
	return rawRun(cmd, ...)
end
local rawConsoleRun = meowy.ConsoleRun
if rawConsoleRun then
	meowy.ConsoleRun = function(line, ...)
		approveLine(line)
		return rawConsoleRun(line, ...)
	end
end

local function joining() return (IsInLoading and IsInLoading()) or (IsInGame and IsInGame()) or false end

-- A protected setting's value: the approved one. A change since is taken when
-- approved (or before any join), otherwise put back and logged.
local function settle(name)
	local raw, k = cvs[name]:GetString(), kept[name]
	if raw == k then
		lastRevert[name] = nil
		return k
	end
	if joining() then joinedEver = true end
	local ok = PROTECTED[name](raw)
	local a = approvals[name]
	if ok and a and SysTime() <= a.untilT and a.value == normalized(raw) then
		approvals[name] = nil
		kept[name] = raw
		return raw
	end
	if ok and not joinedEver then
		kept[name] = raw
		return raw
	end
	local now = SysTime()
	if not lastRevert[name] or now - lastRevert[name] > 0.25 then
		lastRevert[name] = now
		rawRun(name, k)
	end
	if not told[name] then
		told[name] = true
		print(string.format("[Meowy Client Pack] %s was changed to \"%s\" from outside the pack's settings, so it was put back to \"%s\". "
			.. "Servers can't change this setting; change it in the Meowy console (meowy_console) or Meowy settings.",
			name, string.sub(string.gsub(raw, "[%c\"]", "?"), 1, 64), k))
	end
	return k
end

local function current(name) return PROTECTED[name] and settle(name) or cvs[name]:GetString() end
local function truthy(v) local n = tonumber(v) return n ~= nil and n ~= 0 end
local function int(v) local n = tonumber(v) or 0 return n < 0 and math.ceil(n) or math.floor(n) end

-- One object per setting: asked for twice (another chunk reading a setting it
-- doesn't own), the first one comes back. meowy.SettingOf(name) is that object,
-- or nil: how a chunk reads another's setting the way its owner does (the policy
-- and the put-back included), never GetConVar's raw value.
local objects = {}
function meowy.SettingOf(name) return objects[name] end

function meowy.Setting(name, default, help)
	if objects[name] then return objects[name] end
	names[#names + 1] = name
	local start = saved[name] or default
	-- A protected value that couldn't be set through the pack (saved by an older
	-- version, or edited) starts from the default.
	if PROTECTED[name] and not PROTECTED[name](start) then start = default end
	-- FCVAR_ARCHIVE is dropped by the menu state anyway (menu_settings.txt keeps them).
	local cv = CreateConVar(name, start, (FCVAR_ARCHIVE or 128) + FCVAR_HIDDEN_FLAG, help)
	cvs[name] = cv
	helps[name], defaults[name] = help, default
	if PROTECTED[name] then kept[name] = cv:GetString() end
	-- The four readers are made once, on the object itself (some hooks read
	-- them every frame); any other ConVar method comes through the metatable.
	local o = setmetatable({}, { __index = function(_, k)
		local f = cv[k]
		if isfunction(f) then return function(_, ...) return f(cv, ...) end end
		return f
	end })
	o.GetBool = function() return not killed(name) and truthy(current(name)) end
	o.GetInt = function() if killed(name) then return 0 end return int(current(name)) end
	o.GetFloat = function() if killed(name) then return 0 end return tonumber(current(name)) or 0 end
	o.GetString = function() if killed(name) then return "0" end return current(name) end
	objects[name] = o
	return o
end

-- The pack's settings for the Meowy console (hidden from cvarlist): name,
-- value as it reads, default and help, in the order they were made.
function meowy.SettingsList()
	local out = {}
	for _, name in ipairs(names) do
		out[#out + 1] = { name = name, value = objects[name]:GetString(), default = defaults[name], help = helps[name] or "" }
	end
	return out
end

-- The menu's convar lookups, for a pack setting's name (any case, as the
-- console's): its object. Anything else as before.
local function ours(name) return isstring(name) and objects[string.lower(name)] or nil end
local rawGetConVar, rawGetConVarString, rawConVarExists = GetConVar, GetConVarString, ConVarExists
if rawGetConVar then
	GetConVar = function(name, ...)
		local o = ours(name)
		if o then return o end
		return rawGetConVar(name, ...)
	end
end
if rawGetConVarString then
	GetConVarString = function(name, ...)
		local o = ours(name)
		if o then return o:GetString() end
		return rawGetConVarString(name, ...)
	end
end
if rawConVarExists then
	ConVarExists = function(name, ...)
		if ours(name) then return true end
		return rawConVarExists(name, ...)
	end
end

concommand.Add("meowy_policy_status", function()
	local st = meowy.PolicyState and meowy.PolicyState()
	print("[Meowy Client Pack] policy: " .. (istable(st) and st.describe or "unavailable"))
end)

-- Four times a second, protected changes are settled (a server's put back
-- within a quarter of a second; anything reading one settles it first, so a
-- server's value is never what's read). Once a second, what changed is saved:
-- approved values only, the file's text made again only when a value differs.
local SETTLE_EVERY = 0.25
local nextSettle, nextSave, written, text, values = 0, 0, nil, nil, {}
hook.Add("Think", "MeowyClientPack.SaveSettings", function()
	local now, j = SysTime(), joining()
	if j and not wasJoining then told = {} end   -- each join logs its own put-backs
	if j then joinedEver = true end
	wasJoining = j
	if now >= nextSettle then
		nextSettle = now + SETTLE_EVERY
		for name in pairs(PROTECTED) do if cvs[name] then settle(name) end end
	end
	if now < nextSave then return end
	nextSave = now + 1
	local changed = text == nil or #values ~= #names
	for i, name in ipairs(names) do
		local v = clean(current(name))
		if values[i] ~= v then values[i], changed = v, true end
	end
	if changed then
		local lines = {}
		for i, name in ipairs(names) do lines[i] = name .. "=" .. values[i] end
		text = table.concat(lines, "\n")
	end
	if written == nil then written = text return end -- in effect at start; nothing to write
	if text ~= written and meowy.MenuSettingsWrite(text) then written = text end
end)
