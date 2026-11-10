-- Multicore rendering, on by default (meowy_multicore). gmod_mcore_test moves
-- the material system's and bone setup's work off the game's main thread
-- (2.65 ms a frame with it on, 5.30 ms off, on our map), takes effect at
-- once, isn't saved between starts and starts at 0. So the pack sets it at
-- every start, on every server the player joins:
--   * only while the setting is on (read through the policy: the kill switch
--     turns it off), and the crash guard allows it (multicore_logic.lua);
--   * never when mat_queue_mode is 0 or 1: that's the player's own choice;
--   * through meowy.ConsoleRun, the menu's own way to change things, at start,
--     before any join: the settings guard's snapshot (taken as a join starts)
--     has it as the player's own value, not a server's change.
-- A config file that turns it off again in the first HELD_WATCH seconds
-- (autoexec.cfg: the player's) is left alone for that start. Turning the
-- setting off sets it back to 0 at once, if the pack had set it.
--
-- The crash guard's state is multicore_guard.txt (meowy.MenuFileRead/Write).
-- Its `running` marker is cleared at a clean exit by native code (module.cpp's
-- DllMain at process exit, meowy.MulticoreGuardAtExit: the menu state has no
-- dependable "the game is closing" hook), and by the ShutDown hook here too,
-- should the menu state get one. Neither is relied on: a marker left behind
-- counts as a crash only when Garry's Mod's crash handler wrote a report
-- (crashes\*.txt) during that session (meowy.CrashSince, given the session's
-- start time kept in the file); otherwise that session counts as clean.
--
-- meowy.SetMulticoreManaged(bool) tells native, which tells our server's
-- Lua (meowy.MulticoreManaged in the client realm), so Meowy Network's FPS
-- Booster leaves multicore to the pack.
--
-- Runs in its own function at the end of resilience.lua's chunk (module.cpp),
-- with `MulticoreLogic` and resilience.lua's locals in scope: UI, log,
-- menuVisible, overlay, settingsSections.
local Logic = MulticoreLogic

local setting = meowy.Setting and meowy.Setting("meowy_multicore", "1",
	"Meowy Client Pack: turn on multicore rendering (gmod_mcore_test 1) at every start, unless mat_queue_mode is 0 or 1; off after two crashes in a row with it on (0 = off)")
local function settingOn() return setting == nil or setting:GetBool() end

-- Whether the kill switch (or the whole pack's) has it off: then a change of
-- the setting's value isn't the player's.
local function policyOff()
	local st = meowy.PolicyState and meowy.PolicyState()
	if not istable(st) then return false end
	if st.pack_enabled == false then return true end
	for _, n in ipairs(st.off or {}) do
		if n == "meowy_multicore" then return true end
	end
	return false
end

local function cvarText(name) return GetConVarString and GetConVarString(name) or "" end
local function mcoreOn() return tonumber(cvarText("gmod_mcore_test")) == 1 end

local HELD_WATCH = 30   -- as home.lua: config.cfg and autoexec.cfg run a few seconds after this chunk
local mc = {
	started = SysTime(), next = 0,
	applied = false,     -- the pack set gmod_mcore_test 1 and hasn't set it back
	seenOn = false,      -- read back as 1 since (the command ran)
	configOff = false,   -- a config file turned it off again this start
	want = nil, setting = nil,
	pending = nil, pendingUntil = 0,   -- a click's value, until the console has run its command
	policyOff = nil, policyAt = nil,   -- the kill switch, and when it last changed
}
-- The setting as the pack acts on it: a click's value at once (its command
-- may only run at the next frame), then the setting's own.
local function settingNow()
	local v = settingOn()
	if mc.pending ~= nil then
		if v == mc.pending or SysTime() > mc.pendingUntil then mc.pending = nil else return mc.pending end
	end
	return v
end

-- The guard's file, and what native writes there at a clean exit.
local guard
local function save()
	local text = Logic.encode(guard)
	if meowy.MenuFileWrite then meowy.MenuFileWrite("multicore_guard", text) end
	if meowy.MulticoreGuardAtExit then meowy.MulticoreGuardAtExit(Logic.encode(Logic.cleanExit(guard))) end
end

local function managed(on)
	if meowy.SetMulticoreManaged then meowy.SetMulticoreManaged(on) end
end
local function apply()
	if meowy.ConsoleRun then meowy.ConsoleRun("gmod_mcore_test 1") else RunConsoleCommand("gmod_mcore_test", "1") end
	mc.applied, mc.seenOn = true, false
	managed(true)
end
local function unapply(why)
	if mc.applied then
		if meowy.ConsoleRun then meowy.ConsoleRun("gmod_mcore_test 0") else RunConsoleCommand("gmod_mcore_test", "0") end
		log("multicore: gmod_mcore_test 0 (%s)", why)
	end
	mc.applied = false
	managed(false)
end

-- What the pack wants now, applied when it changes (only then: a server or
-- the player setting gmod_mcore_test themselves isn't fought).
local function settle(why)
	local want = Logic.want(settingNow(), guard, cvarText("mat_queue_mode"), mc.configOff)
	if want == mc.want then return end
	mc.want = want
	if want and not mc.applied then
		apply()
		log("multicore: gmod_mcore_test 1 (%s)", why)
	elseif not want and mc.applied then
		unapply(why)
	end
end

-- The player's own toggle (Meowy settings, the Meowy console): the guard starts over.
local function playerChose()
	guard = Logic.playerChose(guard)
	mc.configOff = false
	save()
end

-- At start: the guard's verdict on the last sessions, then this one marked running.
local notice
do
	local queue = cvarText("mat_queue_mode")
	local last = Logic.decode(meowy.MenuFileRead and meowy.MenuFileRead("multicore_guard") or "")
	-- A marker left behind is a crash only with a crash report from that session.
	local crashed = false
	if last.running and meowy.CrashSince then
		local ok, r = pcall(meowy.CrashSince, last.started)
		crashed = ok and r == true
	end
	local on, how
	guard, on, notice, how = Logic.start(last, settingOn() and not Logic.queueIsChoice(queue), crashed, os.time())
	save()
	mc.setting, mc.policyOff = settingOn(), policyOff()
	if how == "no_report" then
		log("multicore: the last session didn't record a clean exit, but Garry's Mod wrote no crash report in it: counted as clean")
	elseif how == "crashed" then
		log("multicore: the last session crashed (Garry's Mod wrote a crash report) with multicore %s", last.mode)
	end
	if notice == "off" then
		log("multicore: the game closed unexpectedly twice in a row with it on, so it's off (round %d of %d)", guard.reverts, Logic.ROUNDS)
	elseif notice == "back_on" then
		log("multicore: the game closed unexpectedly without it too, so it's back on")
	end
	mc.want = on
	if on then
		apply()
		log("multicore: gmod_mcore_test 1 (on by default; Meowy settings turns it off)")
	else
		managed(false)
		if Logic.queueIsChoice(queue) then
			log("multicore: left alone (mat_queue_mode %s is your own choice)", queue)
		end
	end
end

-- For the PC doctor (doctor.lua): what the pack is doing with it.
function meowy.MulticoreStatus()
	return Logic.status(mc.applied, settingOn(), guard, policyOff())
end

hook.Add("ShutDown", "MeowyClientPack.Multicore", function()
	if not guard or not guard.running then return end
	guard = Logic.cleanExit(guard)
	if meowy.MenuFileWrite then meowy.MenuFileWrite("multicore_guard", Logic.encode(guard)) end
end)

hook.Add("Think", "MeowyClientPack.Multicore", function()
	local now = SysTime()
	if now < mc.next then return end
	mc.next = now + 0.5
	local joining = IsInLoading() or IsInGame()

	-- The setting changed (the Meowy console, the policy; a click already acted).
	-- The setting reads the policy up to 2 s late (settings.lua): a change
	-- within 3 s of the policy's is the policy's, not the player's.
	local pOff = policyOff()
	if pOff ~= mc.policyOff then mc.policyOff, mc.policyAt = pOff, now end
	local on = settingNow()
	if on ~= mc.setting then
		mc.setting = on
		if not pOff and now - (mc.policyAt or -math.huge) > 3 then playerChose() end
		settle(on and "turned on" or "turned off")
	end

	if mc.applied then
		if mcoreOn() then
			mc.seenOn = true
		elseif mc.seenOn and not joining and now - mc.started < HELD_WATCH then
			-- Back to 0 at the menu right after start: a config file of the
			-- player's (autoexec.cfg). Theirs, for this start.
			mc.configOff, mc.applied = true, false
			managed(false)
			log("multicore: gmod_mcore_test went back to 0 %.1f s after start (a config file); left off this start", now - mc.started)
		end
	end
	-- mat_queue_mode set to 0 or 1 since (config.cfg, Options): the player's choice.
	settle("mat_queue_mode " .. cvarText("mat_queue_mode"))

	-- The session's mode follows what the game has, whoever set it: a crash
	-- with multicore on counts as one with it on.
	local mode = mcoreOn() and "on" or "off"
	if guard.running and mode ~= guard.mode and (not mc.applied or mc.seenOn) then
		guard.mode = mode
		save()
	end

	-- The notice from the start, over nothing else of the pack's.
	if notice and now - mc.started > 4 and menuVisible() and not IsInGame() and not IsInLoading()
		and not IsValid(overlay) and not IsValid(UI.current) then
		local title, body = Logic.noticeText(notice)
		notice = nil
		if title then
			UI.Prompt({
				icon = { "alert", "amber" },
				title = title,
				body = body,
				closable = true,
				buttons = { { "OK", "primary" } },
			})
		end
	end
end)

-- Meowy settings: the switch. Shown on while the pack has it on; off when
-- the player turned it off or the guard did (turning it on again starts the
-- guard over).
local function setMulticore(on)
	-- Through the console: Garry's Mod refuses ConVar:SetBool on these menu convars.
	RunConsoleCommand("meowy_multicore", on and "1" or "0")
	if policyOff() then return end   -- kept for when the policy lets it; nothing changes now
	-- Acted on now: the command may only run at the next frame.
	mc.pending, mc.pendingUntil, mc.setting = on, SysTime() + 5, on
	playerChose()
	local want = Logic.want(on, guard, cvarText("mat_queue_mode"), false)
	mc.want = want
	if want and not mc.applied then
		apply()
		log("multicore: gmod_mcore_test 1 (turned on in Meowy settings)")
	elseif not want then
		unapply("turned off in Meowy settings")
	end
end

if settingsSections then
	settingsSections[#settingsSections + 1] = function()
		local label = UI.Label(nil, "Performance")
		label.gapBefore = UI.S(22)
		local desc = "Runs part of the game's drawing on other CPU cores (gmod_mcore_test 1): about twice the frame rate "
			.. "on most PCs. The pack turns it on at every start, on every server you join, unless you set "
			.. "mat_queue_mode to 0 or 1 yourself. If the game closes unexpectedly twice in a row with it on, it's "
			.. "turned off and you're told."
		if guard and guard.off and settingOn() then
			desc = desc .. " It's off now: the game closed unexpectedly twice in a row with it on. Turn it on to try again."
		end
		local t = UI.Toggle(nil, "Multicore rendering", desc,
			settingOn() and not (guard and guard.off), function(on) setMulticore(on) end)
		t.meowyToggle = "multicore"
		t.gapBefore = UI.S(10)
		return { label, t }
	end
end
