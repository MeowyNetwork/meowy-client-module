--[[
	Meowy observer: the menu side. One visit at a time, driven by the
	supervisor through $MEOWY_OBSERVER_DIR:

	  job.json  { id, address }   written by the supervisor
	  done.json { id, address, players, observed, still, samples } or { id, error }

	A visit: connect, wait until in game, settle for SETTLE seconds, run the
	sampler in the client state (it writes data/meowy_observer/result.json),
	wait for its counts, disconnect, write done.json. The menu state has no
	timer library, so everything runs from one Think hook.
]]

local observer = observer
local SETTLE, JOIN_TIMEOUT, SAMPLE_TIMEOUT = 60, 300, 780

local job, phase, since, lastJobText = nil, "idle", 0, nil

local function note(msg) observer.Note(os.date("%H:%M:%S ") .. msg) print("[meowy observer] " .. msg) end

local function done(result)
	result.id = job.id
	result.address = job.address
	observer.WriteDone(util.TableToJSON(result))
	note("done: " .. util.TableToJSON(result))
	if IsInGame() or IsInLoading() then RunConsoleCommand("disconnect") end
	job, phase = nil, "idle"
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
	if phase == "idle" then
		local text = observer.ReadJob()
		if not text or text == lastJobText then return end
		lastJobText = text
		local j = util.JSONToTable(text)
		if not istable(j) or not isstring(j.id) or not isstring(j.address) or not j.address:match("^%d+%.%d+%.%d+%.%d+:%d+$") then
			note("job rejected: " .. string.sub(text, 1, 120) .. " (parsed: " .. type(j) .. ")")
			return
		end
		job, phase, since = j, "joining", now
		note("visiting " .. j.address .. " (JoinServer " .. tostring(isfunction(JoinServer)) .. ")")
		-- The way the server browser joins; Lua's RunConsoleCommand("connect") is blocked.
		if isfunction(JoinServer) then JoinServer(j.address) else RunConsoleCommand("connect", j.address) end

	elseif phase == "joining" then
		if IsInGame() and not IsInLoading() then
			note("in game")
			phase, since = "settling", now
		elseif now - since > JOIN_TIMEOUT then
			done({ error = "could not join" })
		end

	elseif phase == "settling" then
		if not IsInGame() then return done({ error = "left during the visit" }) end
		if now - since < SETTLE then return end
		file.Delete("meowy_observer/result.json")
		local ok, err = observer.RunClient(observer.SAMPLER)
		if not ok then return done({ error = "sampler: " .. tostring(err) }) end
		note("sampler started")
		phase, since = "sampling", now

	elseif phase == "sampling" then
		if not IsInGame() then return done({ error = "left during the visit" }) end
		local r = util.JSONToTable(file.Read("meowy_observer/result.json", "DATA") or "")
		if istable(r) and r.still ~= nil then
			return done({ players = r.players, observed = r.observed, still = r.still, samples = r.samples })
		end
		if now - since > SAMPLE_TIMEOUT then done({ error = "sampler timed out" }) end
	end
end

note("ready")
