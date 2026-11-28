-- The frame recorder (frames_logic.lua as `FramesLogic`): how smoothly the game
-- runs while you play, on any server, measured from the menu state (which sees
-- every frame), so no server's Lua is involved and nothing runs in the client
-- realm. A menu chunk of its own (module.cpp), after hooks.lua.
--
--   meowy_frame_recorder      1 = measure while in game (default), 0 = off
--   meowy_frames_status       this session's numbers, its last hitches with the
--                             files opened during them, and the sessions kept
--   meowy.FramesNow()         this session's summary (frames_logic.lua's), or nil
--
-- Only while the game is in front and a server is being played: a loading
-- screen, the game in the background (Windows and fps_max_nofocus slow it
-- down) and the first seconds after spawning aren't counted, and a gap of
-- more than two seconds (a breakpoint, the PC asleep) starts the next frame
-- fresh instead of counting as one.
--
-- Hitches are tagged with the files the game opened during them when the fast
-- file checks' hook is in (fs_cache.h's watch): names and times, kept in memory
-- and printed by the status command. Nothing is sent anywhere. The last
-- sessions' totals are kept in frames.txt (meowy.MenuFileRead/Write).
local Logic = FramesLogic
local meowy = meowy

local function log(fmt, ...) print("[Meowy Client Pack] " .. string.format(fmt, ...)) end

local setting = meowy.Setting and meowy.Setting("meowy_frame_recorder", "1",
	"Meowy Client Pack: measure how smoothly the game runs while you play (frame times and hitches, kept on this PC; meowy_frames_status shows them) (0 = off)")
local function on() return setting == nil or setting:GetBool() end

local SETTLE = 5      -- seconds after spawning before frames count
local MAX_GAP = 2     -- a longer gap isn't a frame

local rec            -- the session under way: { s = Logic state, started, server, last, opens }
local sessions = Logic.decodeSessions(meowy.MenuFileRead and meowy.MenuFileRead("frames") or "")
local watching = false

local function watch(want)
	if want == watching or not meowy.FileOpenWatch then return end
	watching = want
	meowy.FileOpenWatch(want)
end
local function focused()
	return not (system and system.HasFocus) or system.HasFocus()
end
local function mcoreOn()
	return tonumber(GetConVarString and GetConVarString("gmod_mcore_test") or "") == 1
end

local function finish()
	if not rec then return end
	local done = rec
	rec = nil
	watch(false)
	local kept = Logic.session(done.s, os.time(), done.server, meowy.VERSION, done.mcore)
	if not kept then return end
	log("frames on %s: %s", done.server ~= "" and done.server or "this server", Logic.line(Logic.summary(done.s)))
	sessions[#sessions + 1] = kept
	while #sessions > Logic.KEEP_SESSIONS do table.remove(sessions, 1) end
	if meowy.MenuFileWrite then meowy.MenuFileWrite("frames", Logic.encodeSessions(sessions)) end
end

function meowy.FramesNow()
	return rec and Logic.summary(rec.s) or nil
end

hook.Add("Think", "MeowyClientPack.Frames", function()
	local playing = IsInGame() and not IsInLoading()
	if not playing or not on() then
		if rec then finish() end
		return
	end
	local now = SysTime()
	if not rec then
		local address = meowy.ServerNow and meowy.ServerNow() or nil
		rec = { s = Logic.new(), started = now, server = isstring(address) and address or "", mcore = mcoreOn() }
		watch(true)
		return
	end
	local last = rec.last
	-- In the background no frame counts, and the first one back only starts the clock.
	rec.last = focused() and now or nil
	local opens = meowy.FileOpenCount and meowy.FileOpenCount() or nil
	local opensBefore = rec.opens
	rec.opens = opens
	if not last or not rec.last or now - rec.started < SETTLE then return end
	local dt = now - last
	if dt > MAX_GAP then return end   -- not a frame; the next one counts from here
	Logic.add(rec.s, dt * 1000, now - rec.started, opens and opensBefore and meowy.FileOpenRecent and function()
		return meowy.FileOpenRecent(opensBefore)
	end or nil)
end)

concommand.Add("meowy_frames_status", function()
	if not on() then
		print("[Meowy Client Pack] frames: off (meowy_frame_recorder 0)")
	elseif rec and rec.s.frames > 0 then
		print("[Meowy Client Pack] frames, this session: " .. Logic.line(Logic.summary(rec.s)))
		if #rec.s.hitches > 0 then
			print("  the last hitches (time into the session, length"
				.. (meowy.FileOpenRecent and ", files the game opened during it):" or "):"))
			for _, h in ipairs(rec.s.hitches) do print("    " .. Logic.hitchLine(h)) end
		end
	else
		print("[Meowy Client Pack] frames: nothing measured yet (it measures while you play, with the game in front)")
	end
	if #sessions > 0 then
		print("  earlier sessions, newest last:")
		for _, e in ipairs(sessions) do print("    " .. Logic.sessionLine(e)) end
	end
end)
