--[[
	Meowy observer: the sampler. Runs in the observer's own client Lua state on
	a suspect server (the observer module puts it there) and answers one
	question: of the players this client can see, how many stood still?

	The engine only sends positions of players in this client's view area
	(PVS); everyone else is "dormant" and frozen. So a player counts as
	observed only if they were visible in most samples, and as still only if,
	across those samples, they neither moved nor turned their view.

	Every SAMPLE_SECONDS for SAMPLES samples; then the counts (never names or
	positions) go to data/meowy_observer/result.json, which the observer's
	supervisor sends to Meowy Network. The server's own client Lua shares this
	state, so the natives are captured first in case it replaced them later.
]]

local SAMPLE_SECONDS, SAMPLES = 30, 20 -- 10 minutes
local OBSERVED_SHARE = 0.8 -- visible in at least this share of samples
local STILL_UNITS = 32 -- moved less than this (about half a player width)...
local STILL_DEGREES = 5 -- ...and turned less than this

local player_GetAll, IsValid, LocalPlayer = player.GetAll, IsValid, LocalPlayer
local file_Write, file_CreateDir, util_TableToJSON = file.Write, file.CreateDir, util.TableToJSON
local timer_Create, timer_Remove, CurTime, math_abs = timer.Create, timer.Remove, CurTime, math.abs
local id = "meowy_observer_" .. tostring(math.random(1e6))

local tracks = {} -- player entity -> { samples = n, seen = { {pos, yaw, pitch} } }
local taken = 0

local function yawDelta(a, b)
	local d = math_abs(a - b) % 360
	return d > 180 and 360 - d or d
end

local function finish()
	timer_Remove(id)
	local me = LocalPlayer()
	local result = { players = 0, observed = 0, still = 0, samples = taken, seconds = taken * SAMPLE_SECONDS }
	for ply, t in pairs(tracks) do
		if IsValid(ply) and ply ~= me then
			result.players = result.players + 1
			if #t.seen >= taken * OBSERVED_SHARE and #t.seen >= 2 then
				result.observed = result.observed + 1
				local first, moved, turned = t.seen[1], 0, 0
				for _, s in ipairs(t.seen) do
					local d = (s.pos - first.pos):Length()
					if d > moved then moved = d end
					local a = math.max(yawDelta(s.yaw, first.yaw), math_abs(s.pitch - first.pitch))
					if a > turned then turned = a end
				end
				if moved < STILL_UNITS and turned < STILL_DEGREES then result.still = result.still + 1 end
			end
		end
	end
	file_CreateDir("meowy_observer")
	file_Write("meowy_observer/result.json", util_TableToJSON(result))
end

local function sample()
	taken = taken + 1
	local me = LocalPlayer()
	for _, ply in ipairs(player_GetAll()) do
		if IsValid(ply) and ply ~= me then
			local t = tracks[ply]
			if not t then t = { seen = {} } tracks[ply] = t end
			if not ply:IsDormant() and ply:Alive() then
				local ang = ply:EyeAngles()
				t.seen[#t.seen + 1] = { pos = ply:GetPos(), yaw = ang.y, pitch = ang.p }
			end
		end
	end
	if taken >= SAMPLES then finish() end
end

file_CreateDir("meowy_observer")
file_Write("meowy_observer/result.json", util_TableToJSON({ started = CurTime() }))
sample()
timer_Create(id, SAMPLE_SECONDS, SAMPLES - 1, sample)
