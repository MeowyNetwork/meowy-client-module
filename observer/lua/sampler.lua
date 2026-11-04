--[[
	Meowy observer: the sampler. Runs in the observer's own client Lua state on
	a suspect server (the observer module puts it there) for 10 minutes and
	answers: of the players on the server, how many showed any sign of a person?

	Two kinds of evidence:

	  Near us: the engine only sends positions of players in this client's view
	  area (PVS); everyone else is "dormant" and frozen. So a player counts as
	  observed only if visible in most position samples, and as still only if,
	  across those samples, they neither moved nor turned their view.

	  Everyone: what every client gets for every player wherever they stand -
	  ping (scoreboard), score and deaths, DarkRP job and money (money only
	  outside paydays, when everyone's changes at once), chat and voice. A
	  player with none of these for the whole visit is "quiet"; quiet with a
	  low ping that never moved (hosted next to the server) is "idle".

	The counts (never names or positions) go to data/ldr/result.json,
	which the observer's supervisor sends to Meowy Network. The server's own
	client Lua shares this state, so the natives are captured first.

	It runs the whole visit by itself from the moment loading ends: the menu
	state, which started it, doesn't tick while the game is in front of it.
	When done it brings the game menu back, so the menu state wakes, picks up
	the result and disconnects.
]]

local SETTLE = 60 -- seconds after loading before sampling (spawn, the server's intro screens)
local TICK = 5 -- seconds between ticks (ping and the rest)
local TICKS = 120 -- 10 minutes
local POSITION_EVERY = 6 -- a position sample every 6 ticks: 20 in all
local OBSERVED_SHARE = 0.8 -- visible in at least this share of position samples
local PRESENT_SHARE = 0.8 -- connected for at least this share of ticks to be judged
local STILL_UNITS = 32 -- moved less than this (about half a player width)...
local STILL_DEGREES = 5 -- ...and turned less than this
local FLAT_PING_MAX = 15 -- ms: hosted next to the server (as in the backend's reports)
local FLAT_PING_DRIFT = 1 -- ms, over the whole visit
local PAYDAY_SHARE = 0.5 -- this many players' money changing in one tick is a payday

local player_GetAll, IsValid, LocalPlayer = player.GetAll, IsValid, LocalPlayer
local file_Write, file_CreateDir, util_TableToJSON = file.Write, file.CreateDir, util.TableToJSON
local timer_Create, timer_Remove, CurTime, math_abs, pcall = timer.Create, timer.Remove, CurTime, math.abs, pcall
local hook_Add, hook_Remove, timer_Simple = hook.Add, hook.Remove, timer.Simple
local activateMenu = gui and gui.ActivateGameUI
local id = "meowy_observer_" .. tostring(math.random(1e6))

-- player entity -> { ticks, pingMin, pingMax, seen = {{pos, yaw, pitch}}, active, frags, deaths, job, money }
local tracks = {}
local ticks, positions, selfAlive = 0, 0, 0

local function yawDelta(a, b)
	local d = math_abs(a - b) % 360
	return d > 180 and 360 - d or d
end

local function darkrp(ply, key)
	if not ply.getDarkRPVar then return nil end
	local ok, v = pcall(ply.getDarkRPVar, ply, key)
	return ok and v or nil
end

local function track(ply)
	local t = tracks[ply]
	if not t then
		t = { ticks = 0, seen = {}, active = false, frags = ply:Frags(), deaths = ply:Deaths(), job = darkrp(ply, "job"), money = darkrp(ply, "money") }
		tracks[ply] = t
	end
	return t
end

-- Chat and voice reach us from players anywhere (OOC chat is global on DarkRP).
-- Nothing is returned, so the server's own chat handling is untouched.
local function heard(ply)
	if IsValid(ply) and tracks[ply] then tracks[ply].active = true end
end

local function moved(t)
	if #t.seen < 2 then return nil end
	local first, dist, turned = t.seen[1], 0, 0
	for _, s in ipairs(t.seen) do
		local d = (s.pos - first.pos):Length()
		if d > dist then dist = d end
		local a = math.max(yawDelta(s.yaw, first.yaw), math_abs(s.pitch - first.pitch))
		if a > turned then turned = a end
	end
	return dist >= STILL_UNITS or turned >= STILL_DEGREES
end

local function finish()
	timer_Remove(id)
	hook_Remove("OnPlayerChat", id)
	hook_Remove("PlayerStartVoice", id)
	local me = LocalPlayer()
	local r = {
		players = 0, observed = 0, still = 0, samples = positions, seconds = ticks * TICK,
		visible = 0, selfAlive = selfAlive, judged = 0, active = 0, quiet = 0, flat = 0, idle = 0,
	}
	for ply, t in pairs(tracks) do
		if IsValid(ply) and ply ~= me and not ply:IsBot() then
			r.players = r.players + 1
			if #t.seen > 0 then r.visible = r.visible + 1 end
			local m = moved(t)
			if #t.seen >= positions * OBSERVED_SHARE and m ~= nil then
				r.observed = r.observed + 1
				if not m then r.still = r.still + 1 end
			end
			if m then t.active = true end
			if t.ticks >= ticks * PRESENT_SHARE then
				r.judged = r.judged + 1
				local flat = t.pingMax and t.pingMax > 0 and t.pingMax <= FLAT_PING_MAX and t.pingMax - t.pingMin <= FLAT_PING_DRIFT
				if flat then r.flat = r.flat + 1 end
				if t.active then
					r.active = r.active + 1
				else
					r.quiet = r.quiet + 1
					if flat then r.idle = r.idle + 1 end
				end
			end
		end
	end
	-- What Lua sees of each player right now, for the menu to check the module's
	-- native reads against (stays in this machine's result file).
	r.check = {}
	for _, ply in ipairs(player_GetAll()) do
		if IsValid(ply) and #r.check < 128 then
			local pos = ply:GetPos()
			r.check[#r.check + 1] = { index = ply:EntIndex(), x = pos.x, y = pos.y, z = pos.z, dormant = ply:IsDormant(), localPlayer = ply == me }
		end
	end
	file_CreateDir("ldr")
	file_Write("ldr/result.json", util_TableToJSON(r))
	-- Wake the menu state: it reads the result and disconnects.
	if activateMenu then pcall(activateMenu) end
end

local tickBody
local failed = false
-- Every tick reports progress (or the error that stopped it) in result.json, so
-- a visit that never finishes says how far it got and why.
local function tick()
	if failed then return end
	local ok, err = pcall(tickBody)
	if not ok then
		failed = true
		timer_Remove(id)
		hook_Remove("OnPlayerChat", id)
		hook_Remove("PlayerStartVoice", id)
		file_Write("ldr/result.json", util_TableToJSON({ error = tostring(err), ticks = ticks }))
		if activateMenu then pcall(activateMenu) end
	elseif ticks < TICKS then
		file_Write("ldr/result.json", util_TableToJSON({ started = true, ticks = ticks, of = TICKS }))
	end
end

tickBody = function()
	ticks = ticks + 1
	local me = LocalPlayer()
	local position = ticks % POSITION_EVERY == 1
	if position then
		positions = positions + 1
		if IsValid(me) and me:Alive() then selfAlive = selfAlive + 1 end
	end
	local all, moneyChanged = player_GetAll(), {}
	for _, ply in ipairs(all) do
		if IsValid(ply) and ply ~= me then
			local t = track(ply)
			t.ticks = t.ticks + 1
			local ping = ply:Ping()
			if ping then
				t.pingMin = t.pingMin and math.min(t.pingMin, ping) or ping
				t.pingMax = t.pingMax and math.max(t.pingMax, ping) or ping
			end
			local frags, deaths, job, money = ply:Frags(), ply:Deaths(), darkrp(ply, "job"), darkrp(ply, "money")
			if frags ~= t.frags or deaths ~= t.deaths or job ~= t.job then t.active = true end
			if money ~= t.money then moneyChanged[#moneyChanged + 1] = t end
			t.frags, t.deaths, t.job, t.money = frags, deaths, job, money
			if position and not ply:IsDormant() and ply:Alive() then
				local ang = ply:EyeAngles()
				t.seen[#t.seen + 1] = { pos = ply:GetPos(), yaw = ang.y, pitch = ang.p }
			end
		end
	end
	-- A payday changes everyone's money in the same tick; anything else is someone doing something.
	if #moneyChanged < math.max(2, #all * PAYDAY_SHARE) then
		for _, t in ipairs(moneyChanged) do t.active = true end
	end
	if ticks >= TICKS then finish() end
end

file_CreateDir("ldr")
file_Write("ldr/result.json", util_TableToJSON({ loaded = CurTime() }))
timer_Simple(SETTLE, function()
	hook_Add("OnPlayerChat", id, function(ply) heard(ply) end)
	hook_Add("PlayerStartVoice", id, function(ply) heard(ply) end)
	file_Write("ldr/result.json", util_TableToJSON({ started = true, ticks = 0, of = TICKS }))
	tick()
	timer_Create(id, TICK, TICKS - 1, tick)
end)
