-- The frame recorder's rules (src/lua/frames_logic.lua) and its menu glue
-- (src/lua/frames.lua) against a stubbed menu state, no game:
--   lua tests/frames_logic_test.lua        (from the repository root; Lua 5.1+)
-- What it can't show: that the menu state's Think runs once per drawn frame
-- in game, or what fs_cache's watch sees (fs_cache_smoke).
local here = (arg and arg[0] or ""):match("^(.*)[/\\]") or "."
local function readFile(p) local f = assert(io.open(p, "rb")) local s = f:read("*a") f:close() return s end
local M = dofile(here .. "/../src/lua/frames_logic.lua")
local loadstring = loadstring or load

local failures, count = 0, 0
-- check(name, got) wants true; check(name, got, want) wants that, nil included.
local function check(name, got, ...)
	count = count + 1
	local want = true
	if select("#", ...) > 0 then want = (...) end
	if got ~= want then
		failures = failures + 1
		print(("FAIL %s: got %s, want %s"):format(name, tostring(got), tostring(want)))
	end
end
local function near(name, got, want, within)
	check(name .. (" (got %s, want %s)"):format(tostring(got), tostring(want)), math.abs(got - want) <= (within or 0.01))
end

-- --- Counting ------------------------------------------------------------------
do
	local s = M.new()
	check("empty: no frames", M.summary(s).frames, 0)
	check("empty: no rate", M.summary(s).fps, 0)
	check("empty: no low", M.low(s, 0.01), 0)
	check("a frame of no time isn't counted", M.add(s, 0), false)
	check("nor one that isn't a number", M.add(s, "x"), false)
	check("still empty", s.frames, 0)

	-- 990 frames of 10 ms and 10 of 40 ms: 100 fps mostly, 25 fps in the slowest 1%.
	for i = 1, 990 do M.add(s, 10, i / 100) end
	for i = 1, 10 do M.add(s, 40, 10 + i) end
	local sum = M.summary(s)
	check("frames counted", sum.frames, 1000)
	near("average rate", sum.fps, 1000 * 1000 / (990 * 10 + 10 * 40), 0.1)
	near("the median frame", sum.median, 10.25, 0.26)
	near("1% low: the slowest ten frames", sum.low1, 1000 / 40.25, 0.3)
	near("0.1% low: the slowest one", sum.low01, 1000 / 40.25, 0.3)
	check("99% of frames are under", sum.p99 <= 10.5)
	check("worst", sum.worst, 40)
	check("40 ms after 10 ms frames is a hitch", sum.hitches, 10)
	near("seconds", sum.seconds, 10, 0.5)
	check("hitches a minute", sum.perMinute > 50)
	check("the line reads", M.line(sum):find("1%% low 25 fps") ~= nil)
end

-- --- Hitches -------------------------------------------------------------------
do
	check("50 ms is always a hitch", M.isHitch(50, nil), true)
	check("49 ms with no history isn't", M.isHitch(49, nil), false)
	check("30 ms at 144 fps is", M.isHitch(30, 7), true)
	check("30 ms at 60 fps isn't", M.isHitch(30, 16.7), false)
	check("24 ms never is", M.isHitch(24, 2), false)

	local s = M.new()
	for i = 1, 100 do M.add(s, 8, i) end
	local asked = 0
	local files = function()
		asked = asked + 1
		return { { name = "materials/a.vmt", ms = 1 }, { name = "models/big.mdl", ms = 60 }, { name = "sound/x.wav", ms = 5 } }
	end
	check("an ordinary frame doesn't ask for files", M.add(s, 8, 101, files), false)
	check("so nothing was asked", asked, 0)
	check("a long frame is a hitch", M.add(s, 120, 102, files), true)
	check("and its files were asked for once", asked, 1)
	local h = s.hitches[1]
	check("the slowest open first", h.files[1].name, "models/big.mdl")
	check("how many were opened", h.opened, 3)
	check("when", h.at, 102)
	check("the hitch's line", M.hitchLine(h), "1:42  120 ms  3 files opened, 66 ms in all: models/big.mdl (60 ms), sound/x.wav (5 ms), materials/a.vmt (1 ms)")
	check("a hitch doesn't move the recent average", s.avg, 8)
	-- More files than are kept.
	local many = {}
	for i = 1, 20 do many[i] = { name = "f" .. i, ms = i } end
	M.add(s, 200, 130, function() return many end)
	check("only the slowest few are kept", #s.hitches[2].files, M.KEEP_FILES)
	check("the line counts them all", M.hitchLine(s.hitches[2]):find("20 files opened, 210 ms in all: f20 (20 ms)", 1, true) ~= nil)
	-- Quick opens (a search-path probe each) are counted, not named.
	M.add(s, 80, 135, function()
		return { { name = "a", ms = 0.02 }, { name = "b", ms = 0.4 }, { name = "D:\\Steam\\steamapps\\common\\GarrysMod\\garrysmod\\addons\\x\\models\\slow.mdl", ms = 30 } }
	end)
	check("quick opens are only counted, and the path is shortened", M.hitchLine(s.hitches[3]),
		"2:15  80 ms  3 files opened, 30 ms in all: addons/x/models/slow.mdl (30 ms)")
	M.add(s, 80, 136, function() return { { name = "a", ms = 0.02 } } end)
	check("one quick open", M.hitchLine(s.hitches[4]), "2:16  80 ms  1 file opened, 0 ms in all")
	check("a path outside the game stays whole", M.shortName("C:\\Windows\\Fonts\\arial.ttf"), "C:/Windows/Fonts/arial.ttf")
	check("the last game folder in the path counts", M.shortName("d:\\x\\GarrysMod\\garrysmod\\gameinfo.txt"), "gameinfo.txt")
	-- No watch: no file list.
	M.add(s, 90, 140)
	check("without the watch, just the time", M.hitchLine(s.hitches[5]), "2:20  90 ms")
	M.add(s, 90, 150, function() return {} end)
	check("with it and nothing opened, it says so", M.hitchLine(s.hitches[6]), "2:30  90 ms  no file opened")
	-- Only the last ones are kept; the count is of all.
	for i = 1, 40 do M.add(s, 60, 200 + i) end
	check("the last hitches are kept", #s.hitches, M.KEEP_HITCHES)
	check("all are counted", s.hitchCount, 46)
	-- A frame longer than the histogram.
	local t = M.new()
	for i = 1, 99 do M.add(t, 5, i) end
	M.add(t, 1000, 100)
	near("a one-second frame is the 1% low", M.low(t, 0.01), 1, 0.01)
	check("and the worst", M.summary(t).worst, 1000)
end

-- --- Sessions kept -------------------------------------------------------------
do
	local s = M.new()
	for i = 1, 100 do M.add(s, 10, i) end
	check("a session of a second isn't kept", M.session(s, 1700000000, "1.2.3.4:27015", "1.9.0", true), nil)
	for i = 1, 6000 do M.add(s, 10, i) end
	local e = M.session(s, 1700000000, "1.2.3.4:27015 x\n", "1.9.0", true)
	check("a minute is", e ~= nil)
	check("its server, cleaned", e.server, "1.2.3.4:27015x")
	check("its rate", e.fps, 100)
	local text = M.encodeSessions({ e })
	local back = M.decodeSessions(text)
	check("round trip: one session", #back, 1)
	check("round trip: rate", back[1].fps, 100)
	check("round trip: server", back[1].server, "1.2.3.4:27015x")
	check("round trip: multicore", back[1].mcore, 1)
	check("round trip: pack", back[1].pack, "1.9.0")
	check("encode is stable", M.encodeSessions(back), text)
	check("the session's line", M.sessionLine(back[1]):find("100 fps average", 1, true) ~= nil)
	local many = {}
	for i = 1, 25 do many[i] = e end
	check("only the newest are kept", #M.decodeSessions(M.encodeSessions(many)), M.KEEP_SESSIONS)
	check("rubbish reads as nothing", #M.decodeSessions("hello\n\0\nfps=abc\n"), 0)
	check("no text reads as nothing", #M.decodeSessions(nil), 0)
	check("nothing kept writes nothing", M.encodeSessions({}), "")
end

-- --- The glue, in a stubbed menu state -----------------------------------------
local chunk = "local FramesLogic = (function()\n" .. readFile(here .. "/../src/lua/frames_logic.lua") .. "\nend)()\n"
	.. readFile(here .. "/../src/lua/frames.lua")
local function world(opts)
	opts = opts or {}
	local w = { file = opts.file or "", inGame = false, loading = false, sys = 0, now = 1700000000, focus = true, on = opts.off ~= true,
		printed = {}, hooks = {}, cmds = {}, opens = 0, opened = {}, watch = nil, mcore = "1" }
	local env = setmetatable({}, { __index = _G })
	env.print = function(s) w.printed[#w.printed + 1] = tostring(s) end
	env.isstring = function(v) return type(v) == "string" end
	env.IsInGame = function() return w.inGame end
	env.IsInLoading = function() return w.loading end
	env.SysTime = function() return w.sys end
	env.GetConVarString = function(name) return name == "gmod_mcore_test" and w.mcore or "" end
	env.system = { HasFocus = function() return w.focus end }
	env.os = setmetatable({ time = function() return w.now end }, { __index = os })
	env.hook = { Add = function(_, id, f) w.hooks[id] = f end }
	env.concommand = { Add = function(name, f) w.cmds[name] = f end }
	env.meowy = {
		VERSION = "1.9.0",
		Setting = function(name, default) w.created = { name, default } return { GetBool = function() return w.on end } end,
		MenuFileRead = function(kind) w.readKind = kind return w.file end,
		MenuFileWrite = function(kind, text) w.writeKind = kind w.file = text return true end,
		ServerNow = function() return "5.6.7.8:27015" end,
	}
	if not opts.noWatch then
		env.meowy.FileOpenWatch = function(on) w.watch = on end
		env.meowy.FileOpenCount = function() return w.opens end
		env.meowy.FileOpenRecent = function(after)
			local out = {}
			for i = after + 1, w.opens do out[#out + 1] = w.opened[i] end
			return out
		end
	end
	local f = assert(loadstring(chunk, "frames"))
	if setfenv then setfenv(f, env) else f = assert(load(chunk, "frames", "t", env)) end
	f()
	w.meowy = env.meowy
	w.think = function() w.hooks["MeowyClientPack.Frames"]() end
	-- n frames of ms each.
	w.frames = function(n, ms)
		for _ = 1, n do
			w.sys = w.sys + ms / 1000
			w.think()
		end
	end
	w.open = function(name, ms) w.opens = w.opens + 1 w.opened[w.opens] = { name = name, ms = ms } end
	w.out = function() return table.concat(w.printed, "\n") end
	return w
end

do
	local w = world()
	check("glue: on by default", w.created[2], "1")
	check("glue: reads its own file", w.readKind, "frames")
	w.think()
	check("glue: nothing at the menu", w.meowy.FramesNow(), nil)
	w.cmds.meowy_frames_status()
	check("glue: the status says so", w.out():find("nothing measured yet", 1, true) ~= nil)

	-- In game: the first seconds aren't counted, then every frame is.
	w.inGame = true
	w.think()
	check("glue: the watch goes on in game", w.watch, true)
	w.frames(300, 10)   -- 3 s: still settling
	check("glue: the first seconds aren't counted", w.meowy.FramesNow().frames, 0)
	w.frames(300, 10)   -- past 5 s
	local n = w.meowy.FramesNow().frames
	check("glue: then frames are", n > 90 and n < 110)
	w.frames(6000, 10)
	near("glue: the rate", w.meowy.FramesNow().fps, 100, 0.5)

	-- A hitch, with the files opened in that frame only.
	w.open("models/old.mdl", 3)
	w.frames(1, 10)
	w.open("models/props/crate.mdl", 70)
	w.open("materials/crate.vmt", 2)
	w.frames(1, 90)
	check("glue: the hitch is counted", w.meowy.FramesNow().hitches, 1)
	w.printed = {}
	w.cmds.meowy_frames_status()
	check("glue: the status names the file", w.out():find("90 ms  2 files opened, 72 ms in all: models/props/crate.mdl (70 ms), materials/crate.vmt (2 ms)", 1, true) ~= nil)
	check("glue: not an earlier frame's file", w.out():find("old.mdl", 1, true), nil)

	-- In the background, or after a long gap, frames aren't counted.
	local before = w.meowy.FramesNow().frames
	w.focus = false
	w.frames(50, 100)
	check("glue: in the background nothing is counted", w.meowy.FramesNow().frames, before)
	check("glue: nor as hitches", w.meowy.FramesNow().hitches, 1)
	w.focus = true
	w.frames(1, 10)   -- the first frame back only starts the clock
	check("glue: the first frame back isn't one", w.meowy.FramesNow().frames, before)
	w.frames(1, 10)
	check("glue: the next is", w.meowy.FramesNow().frames, before + 1)
	w.frames(1, 5000)
	check("glue: a gap of seconds isn't a frame", w.meowy.FramesNow().hitches, 1)
	w.frames(2, 10)

	-- A loading screen ends the session; it's kept and logged.
	w.printed = {}
	w.loading = true
	w.think()
	check("glue: leaving ends the session", w.meowy.FramesNow(), nil)
	check("glue: the watch goes off", w.watch, false)
	check("glue: it's logged", w.out():find("frames on 5.6.7.8:27015: 100 fps average", 1, true) ~= nil)
	check("glue: and kept", w.writeKind, "frames")
	local kept = M.decodeSessions(w.file)
	check("glue: one session kept", #kept, 1)
	check("glue: with multicore as it was", kept[1].mcore, 1)
	check("glue: and when", kept[1].at, 1700000000)

	-- The next start lists it.
	local w2 = world({ file = w.file })
	w2.cmds.meowy_frames_status()
	check("glue: earlier sessions are listed", w2.out():find("5.6.7.8:27015  100 fps average", 1, true) ~= nil)

	-- A short visit isn't kept.
	local w3 = world()
	w3.inGame = true
	w3.think()
	w3.frames(1000, 10)
	w3.inGame = false
	w3.think()
	check("glue: a short session isn't kept", w3.file, "")
	check("glue: nor logged", #w3.printed, 0)
end

do
	-- Off: nothing measured, nothing watched.
	local w = world({ off = true })
	w.inGame = true
	w.think()
	w.frames(1000, 10)
	check("glue: off, nothing measured", w.meowy.FramesNow(), nil)
	check("glue: off, no watch", w.watch, nil)
	w.cmds.meowy_frames_status()
	check("glue: off, the status says how", w.out():find("meowy_frame_recorder 0", 1, true) ~= nil)
	-- Turned off while playing: the session ends there.
	local w2 = world()
	w2.inGame = true
	w2.think()
	w2.frames(7000, 10)
	w2.on = false
	w2.think()
	check("glue: turned off mid-session, it ends", w2.meowy.FramesNow(), nil)
	check("glue: and is kept", #M.decodeSessions(w2.file), 1)
	-- A build without the watch still measures.
	local w3 = world({ noWatch = true })
	w3.inGame = true
	w3.think()
	w3.frames(700, 10)
	w3.frames(1, 90)
	check("glue: without the watch, hitches are still counted", w3.meowy.FramesNow().hitches, 1)
	w3.cmds.meowy_frames_status()
	check("glue: just without files", w3.out():find("  90 ms", 1, true) ~= nil and w3.out():find("opened", 1, true) == nil)
end

print(("%d checks, %d failures"):format(count, failures))
os.exit(failures == 0 and 0 or 1)
