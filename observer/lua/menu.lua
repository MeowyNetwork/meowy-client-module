--[[
	Meowy observer: the menu side. One visit at a time, driven by the
	supervisor through $MEOWY_OBSERVER_DIR:

	  job.json  { id, address }   written by the supervisor
	  done.json { id, address, players, observed, still, samples, ... } or { id, error }

	A visit: connect; the moment loading ends, start the sampler in the
	client state (it settles, samples for 10 minutes and writes
	data/ldr/result.json); read its counts, disconnect, write
	done.json.

	The menu state only ticks while the game menu is on screen (main menu,
	loading screen, console), not while the game itself is in front. So the
	in-game part runs entirely in the client state, started from the loading
	screen's OnDeactivate (called as loading ends, while the menu still
	ticks), and the sampler brings the menu back when it's done. The menu
	state has no timer library either, so everything here runs from one
	Think hook.
]]

local observer = observer
-- Joining includes downloading the server's Workshop content (2.5 GB on some
-- servers, the first time only), so it gets a long time. Sampling is 60 s of
-- settling plus 10 minutes.
local JOIN_TIMEOUT, SAMPLE_TIMEOUT = 40 * 60, 15 * 60

local job, phase, since, lastJobText = nil, "idle", 0, nil
local stage = nil -- the last loading status seen this visit ("Retrieving Lua files...", a download...)

local function note(msg) observer.Note(os.date("%H:%M:%S ") .. msg) print("[meowy observer] " .. msg) end

-- What the loading screen shows; nil when not loading.
local function loadStatus()
	if not isfunction(GetLoadStatus) then return nil end
	local s = GetLoadStatus()
	if not isstring(s) then return nil end
	s = string.Trim(s)
	return s ~= "" and s or nil
end

-- (Only the load screen says whether we're loading: the status text stays set
-- after a refused connect, so it's for the log only.)
local function loading()
	-- IsInLoading comes from GMod's own menu scripts (menu/loading.lua). When the
	-- main menu fails to load ("Menu failed to load") it can be missing: then the
	-- status text, while not in game, is the best there is.
	if isfunction(IsInLoading) then return IsInLoading() end
	return loadStatus() ~= nil and not IsInGame()
end

-- A failure, with where the join got to.
local function why(err)
	return stage and (err .. " (last stage: " .. string.sub(stage, 1, 120) .. ")") or err
end

local function done(result)
	-- The sampler's file is readable by any server's client Lua: don't leave one
	-- server's numbers (or the position check) for the next server to find.
	file.Delete("ldr/result.json")
	result.id = job.id
	result.address = job.address
	observer.WriteDone(util.TableToJSON(result))
	note("done: " .. util.TableToJSON(result))
	if IsInGame() or loading() then RunConsoleCommand("disconnect") end
	job, phase = nil, "idle"
end

-- The module can read player positions natively (from the engine, not the
-- server's Lua), for status-only visits. It's trusted only after matching what
-- the Lua sampler saw at the end of a normal visit; a crash during that check
-- marks it bad for good (the module does that on the next start).
-- "ok <position source> <yaw source>" once checked: vtable (GetAbsOrigin /
-- GetAbsAngles) or fields (m_vecOrigin / m_angEyeAngles[1], the networked values).
local function nativeUsable() return observer.NativeState():sub(1, 3) == "ok " end

local function yawDiff(a, b)
	local d = math.abs(a - b) % 360
	return d > 180 and 360 - d or d
end

local function fmt(x, y, z) return string.format("(%d %d %d)", x, y, z) end

local function checkNative(check)
	local state = observer.NativeState()
	if state:sub(1, 2) == "ok" or state:sub(1, 3) == "bad" or not istable(check) then return end
	observer.SetNativeState("pending")
	local players, why = observer.NativePlayers()
	if not players then
		observer.SetNativeState("bad: " .. tostring(why))
		return note("native check: refused (" .. tostring(why) .. ")")
	end
	local byIndex = {}
	for _, p in ipairs(players) do byIndex[p.index] = p end
	-- Players can move a little between the two reads.
	local compared, vtable, fields, yaws, absYaw, eyeYaw, sample = 0, 0, 0, 0, 0, 0, nil
	for _, c in ipairs(check) do
		if not c.dormant then
			compared = compared + 1
			local p = byIndex[c.index]
			if p and not p.dormant then
				if math.sqrt((p.x - c.x) ^ 2 + (p.y - c.y) ^ 2 + (p.z - c.z) ^ 2) < 300 then vtable = vtable + 1 end
				if p.nx and math.sqrt((p.nx - c.x) ^ 2 + (p.ny - c.y) ^ 2 + (p.nz - c.z) ^ 2) < 300 then fields = fields + 1 end
				if c.yaw then
					yaws = yaws + 1
					if yawDiff(p.yaw, c.yaw) < 20 then absYaw = absYaw + 1 end
					if p.eyeYaw and yawDiff(p.eyeYaw, c.yaw) < 20 then eyeYaw = eyeYaw + 1 end
				end
			end
			if not sample and not c.localPlayer then
				sample = string.format("#%d Lua %s yaw %s; vtable %s yaw %s; fields %s yaw %s", c.index, fmt(c.x, c.y, c.z), tostring(c.yaw and math.floor(c.yaw)),
					p and fmt(p.x, p.y, p.z) or "missing", p and tostring(math.floor(p.yaw)) or "-",
					p and p.nx and fmt(p.nx, p.ny, p.nz) or "none", p and p.eyeYaw and tostring(math.floor(p.eyeYaw)) or "none")
			end
		end
	end
	note("native check: read " .. observer.NativeInfo())
	if #players == 0 then
		observer.SetNativeState("unknown")
		return note("native check: no players read natively, next time")
	end
	note(string.format("native check: %d read natively, %d in view to Lua; positions matched vtable %d, fields %d; yaw matched abs %d, eye %d of %d; fields %s; e.g. %s",
		#players, compared, vtable, fields, absYaw, eyeYaw, yaws, observer.NativeSimTime(), tostring(sample)))
	if compared < 2 or yaws < 2 then
		observer.SetNativeState("unknown")
		return note("native check: too few to compare (" .. compared .. " in view, " .. yaws .. " with a yaw), next time")
	end
	local pos = vtable >= compared * 0.9 and "vtable" or fields >= compared * 0.9 and "fields" or nil
	local yaw = absYaw >= yaws * 0.9 and "absyaw" or eyeYaw >= yaws * 0.9 and "eyeyaw" or nil
	if pos and yaw then
		observer.SetNativeState("ok " .. pos .. " " .. yaw)
		note("native check: ok (" .. pos .. " positions, " .. yaw .. ")")
	else
		observer.SetNativeState(string.format("bad: positions vtable %d / fields %d, yaw abs %d / eye %d, of %d", vtable, fields, absYaw, eyeYaw, compared))
		note("native check: failed")
	end
end

-- A player's position and yaw from the source the check chose, or nil.
local function nativeRead(p)
	local state = observer.NativeState()
	local x, y, z = p.x, p.y, p.z
	if state:find(" fields", 1, true) then x, y, z = p.nx, p.ny, p.nz end
	local yaw = state:find(" eyeyaw", 1, true) and p.eyeYaw or p.yaw
	if not x or not yaw then return nil end
	return { x = x, y = y, z = z, yaw = yaw }
end

-- Native position samples during a status-only visit, per player index:
-- { n = reads present, seen = {{x,y,z,yaw}} from fresh reads, toggles = times it
-- went in or out of view, lastVis, lastSim }.
--
-- A read is fresh when the player is in view (not dormant) and its simulation
-- time moved on since the last read: the server sent us something new about
-- them. In view with the same simulation time is stale (not "standing still")
-- and its position isn't used. Dormant players (out of view, or withheld by
-- the server) give no position at all.
local nativeTracks, nativeReads, nativeWhy = {}, 0, nil
local visibleReads, staleReads, unprovenReads = 0, 0, 0
local selfFirst, selfMoved = nil, false

local function nativeReset()
	nativeTracks, nativeReads, nativeWhy = {}, 0, nil
	visibleReads, staleReads, unprovenReads = 0, 0, 0
	selfFirst, selfMoved = nil, false
end

local function apart(a, b) return math.sqrt((a.x - b.x) ^ 2 + (a.y - b.y) ^ 2 + (a.z - b.z) ^ 2) end

local function nativeSample()
	if not nativeUsable() then return end
	local players, why = observer.NativePlayers()
	if not players then nativeWhy = why return end
	nativeReads = nativeReads + 1
	for _, p in ipairs(players) do
		if p.localPlayer then
			-- We never move on purpose; if we did (killed, moved by the server),
			-- players leaving our view no longer means they moved.
			local me = nativeRead(p)
			if me then
				if not selfFirst then selfFirst = me elseif apart(me, selfFirst) > 64 then selfMoved = true end
			end
		else
			local t = nativeTracks[p.index]
			if not t then t = { n = 0, seen = {}, toggles = 0 } nativeTracks[p.index] = t end
			t.n = t.n + 1
			local vis = not p.dormant
			if vis then
				visibleReads = visibleReads + 1
				local fresh
				if p.simTime == nil then
					unprovenReads = unprovenReads + 1
					fresh = true -- positions still count for stillness; the AFK share needs proof
				elseif t.lastSim ~= nil and p.simTime == t.lastSim then
					staleReads = staleReads + 1
					fresh = false
				else
					fresh = true
				end
				local at = fresh and nativeRead(p)
				if at then t.seen[#t.seen + 1] = at end
			end
			if p.simTime ~= nil then t.lastSim = p.simTime end
			if t.lastVis ~= nil and t.lastVis ~= vis then t.toggles = t.toggles + 1 end
			t.lastVis = vis
		end
	end
end

-- A few stale reads are dropped one by one (a player the server stopped
-- updating, a burst of loss); this many means the connection stalled.
local MOVE_UNITS, TURN_DEGREES, STALE_MAX = 32, 5, 0.3

local function nativeCounts()
	if nativeReads < 10 then return nil end
	local r = { players = 0, visible = 0, observed = 0, still = 0, moved = 0 }
	for _, t in pairs(nativeTracks) do
		r.players = r.players + 1
		if #t.seen > 0 then r.visible = r.visible + 1 end
		local f, dist, turned = t.seen[1], 0, 0
		for _, s in ipairs(t.seen) do
			dist = math.max(dist, apart(s, f))
			local a = math.abs(s.yaw - f.yaw) % 360
			turned = math.max(turned, a > 180 and 360 - a or a)
		end
		local movedInView = #t.seen >= 2 and (dist >= MOVE_UNITS or turned >= TURN_DEGREES)
		-- Went out of view (or came into it) while we stood still: they moved.
		local crossed = not selfMoved and t.toggles > 0 and #t.seen >= 1
		if #t.seen >= nativeReads * 0.8 and #t.seen >= 2 then
			r.observed = r.observed + 1
			if not movedInView and t.toggles == 0 then r.still = r.still + 1 end
		end
		if movedInView or crossed then r.moved = r.moved + 1 end
	end
	-- Proof the reads were fresh, or no "moved" (and so no AFK share): every
	-- read had a simulation time, and hardly any stood still.
	local sim = observer.NativeSimTime():match("^%S+")
	local staleShare = visibleReads > 0 and staleReads / visibleReads or 1
	r.check = { sim = sim, visibleReads = visibleReads, staleReads = staleReads, unprovenReads = unprovenReads, selfMoved = selfMoved }
	note(string.format("native positions: %d players, %d seen, %d observed, %d still, %d moved; simulation time %s, %d of %d reads stale, %d without one%s",
		r.players, r.visible, r.observed, r.still, r.moved, sim, staleReads, visibleReads, unprovenReads, selfMoved and ", we moved" or ""))
	if sim ~= "ok" or unprovenReads > 0 or staleShare > STALE_MAX then
		note("native positions: not proven fresh, no AFK share from this visit")
		r.moved = nil
	end
	return r
end

-- Status-only visits (servers whose anticheat stops the sampler): no code in
-- the server's game at all. The in-game menu stays open so this state keeps
-- ticking, and `status` runs every STATUS_EVERY seconds; its tables land in the
-- console log, which the supervisor reads (pings of every player).
local STATUS_EVERY, STATUS_RUNS = 30, 20
local statusRuns, nextStatus = 0, 0

local function statusTick(now)
	if not IsInGame() then return done({ error = "left during the visit (status only)" }) end
	if now < nextStatus then return end
	if statusRuns >= STATUS_RUNS then
		local native = nativeCounts()
		if not native then note("native positions: none (" .. tostring(nativeWhy or observer.NativeState()) .. ")") end
		return done({ statusOnly = true, runs = statusRuns, native = native })
	end
	RunConsoleCommand("gameui_activate") -- keep the menu up: closing it would stop this state
	RunConsoleCommand("status")
	nativeSample()
	statusRuns, nextStatus = statusRuns + 1, now + STATUS_EVERY
end

-- Loading has ended: hand the visit to the client state now, while we still tick.
local function loaded(how)
	if phase ~= "joining" then return end
	-- The load screen also closes on a kick or a failed connect: that's the
	-- joining phase's business (and the supervisor's, from the console).
	if not IsInGame() then return note("load screen closed, not in game") end
	if job.mode == "status" then
		RunConsoleCommand("gameui_activate")
		note("in game (" .. how .. "), status only: " .. STATUS_RUNS .. " status runs, " .. STATUS_EVERY .. " s apart")
		phase, since, statusRuns, nextStatus = "status", SysTime(), 0, SysTime() + 5
		nativeReset()
		return
	end
	file.Delete("ldr/result.json")
	local ok, err = observer.RunClient(observer.SAMPLER)
	if not ok then return done({ error = "sampler: " .. tostring(err) }) end
	note("in game (" .. how .. "), sampler started")
	phase, since = "sampling", SysTime()
end

-- The loading screen's OnDeactivate, wrapped on the panel the engine uses.
local function hookLoadPanel()
	if not isfunction(GetLoadPanel) then return end
	local p = GetLoadPanel()
	if not IsValid(p) or p.MeowyObserverHooked then return end
	p.MeowyObserverHooked = true
	local original = p.OnDeactivate
	p.OnDeactivate = function(self, ...)
		if original then original(self, ...) end
		local ok, err = pcall(loaded, "load screen closed")
		if not ok then note("error: " .. tostring(err)) end
	end
end

local nextLook, step, errors, ticks = 0, nil, 0, 0
hook.Add("Think", "MeowyObserver", function()
	local now = SysTime()
	if now < nextLook then return end
	nextLook = now + 1
	ticks = ticks + 1
	if ticks == 1 then note("first check, job file: " .. (observer.ReadJob() and "present" or "missing")) end
	-- Errors here would only reach a console nobody watches: log the first few.
	local ok, err = pcall(step, now)
	if not ok and errors < 5 then
		errors = errors + 1
		note("error: " .. tostring(err))
	end
end)

step = function(now)
	-- A new job while busy: the supervisor gave up on this one (it saw the
	-- disconnect in the console log, or the game closed). Drop it and take the new one.
	if phase ~= "idle" then
		local text = observer.ReadJob()
		if text and text ~= lastJobText then
			note("new job while " .. phase .. ", dropping " .. job.address)
			if IsInGame() or loading() then RunConsoleCommand("disconnect") end
			job, phase = nil, "idle"
		end
	end

	if phase == "idle" then
		local text = observer.ReadJob()
		if not text or text == lastJobText then return end
		lastJobText = text
		local j = util.JSONToTable(text)
		if not istable(j) or not isstring(j.id) or not isstring(j.address) or not j.address:match("^%d+%.%d+%.%d+%.%d+:%d+$") then
			note("job rejected: " .. string.sub(text, 1, 120) .. " (parsed: " .. type(j) .. ")")
			return
		end
		job, phase, since, stage = j, "joining", now, nil
		hookLoadPanel()
		note("visiting " .. j.address .. " (JoinServer " .. tostring(isfunction(JoinServer)) .. ")")
		-- Of a server's downloads (Workshop included), only maps by default; "all"
		-- for servers whose map comes inside an addon not tagged as a map.
		RunConsoleCommand("cl_downloadfilter", j.filter == "all" and "all" or "mapsonly")
		-- The way the server browser joins; Lua's RunConsoleCommand("connect") is blocked.
		if isfunction(JoinServer) then JoinServer(j.address) else RunConsoleCommand("connect", j.address) end

	elseif phase == "joining" then
		hookLoadPanel()
		local s = loadStatus()
		if s and s ~= stage then
			-- Each new stage in the observer's log: where a slow or failed join got to.
			stage = s
			note("stage: " .. string.sub(s, 1, 120))
		end
		if IsInGame() and not loading() then
			-- Only if the load screen hook didn't fire (we woke up in game).
			loaded("noticed in game")
		elseif now - since > 45 and not loading() and not IsInGame() then
			-- Back on the menu: refused or kicked (full, whitelist, lobby-only...).
			done({ error = why("back on the menu while joining (refused or kicked)") })
		elseif now - since > JOIN_TIMEOUT then
			done({ error = why("still joining after " .. math.floor(JOIN_TIMEOUT / 60) .. " min") })
		end

	elseif phase == "status" then
		statusTick(now)

	elseif phase == "sampling" then
		-- The result first: the sampler brings the menu back once it's written.
		local r = util.JSONToTable(observer.TakeClientResult() or file.Read("ldr/result.json", "DATA") or "")
		if istable(r) and r.still ~= nil then
			local ok, err = pcall(checkNative, r.check)
			if not ok then note("native check error: " .. tostring(err)) end
			return done({
				players = r.players, observed = r.observed, still = r.still, samples = r.samples,
				visible = r.visible, selfAlive = r.selfAlive, judged = r.judged,
				active = r.active, quiet = r.quiet, flat = r.flat, idle = r.idle,
			})
		end
		if istable(r) and r.error then return done({ error = "sampler failed after " .. tostring(r.ticks) .. " ticks: " .. string.sub(tostring(r.error), 1, 150) }) end
		if not IsInGame() then return done({ error = "left during the visit (disconnected before the sampler finished)" }) end
		if now - since > SAMPLE_TIMEOUT then
			local progress = not istable(r) and "no result file"
				or r.loaded and "never started sampling (the 60 s wait didn't end)"
				or r.ticks and ("stopped at tick " .. tostring(r.ticks) .. " of " .. tostring(r.of))
				or "unknown state"
			done({ error = "sampler timed out: " .. progress })
		end
	end
end

note("ready")
