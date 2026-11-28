-- The frame recorder's rules, as pure functions (no GMod API), so they can be
-- tested with a plain Lua interpreter (tests/frames_logic_test.lua).
-- Lua 5.1 compatible: Garry's Mod runs LuaJIT.
--
-- While you play, the menu state sees every frame the game draws. The recorder
-- (frames.lua) hands each frame's time to M.add, which keeps:
--   * how many frames and how long, for the average;
--   * a histogram in half-millisecond steps up to 200 ms (longer frames are
--     counted in the last step, their time kept exact), for the median, the
--     99th percentile and the "1% low": the average frame rate of the slowest
--     1% of frames, which is what stutter feels like;
--   * hitches: a frame of 50 ms or more, or one of 25 ms or more that took four
--     times as long as the frames around it. The last ones are kept with when
--     they happened and the files the game opened during them, since a file
--     read on the game's own thread is the usual cause.
-- Nothing here is sent anywhere: the numbers go to the console
-- (meowy_frames_status) and the last sessions to frames.txt on this PC.

local M = {}

M.BIN_MS = 0.5
M.BINS = 400            -- 0.5 ms each: up to 200 ms
M.HITCH_MS = 50         -- always a hitch
M.HITCH_REL_MS = 25     -- a hitch when it's also HITCH_REL times the recent average
M.HITCH_REL = 4
M.KEEP_HITCHES = 20
M.KEEP_FILES = 6        -- files kept per hitch
M.SLOW_OPEN_MS = 1      -- an open at least this long is named; quicker ones are only counted
M.KEEP_SESSIONS = 10
M.MIN_SECONDS = 30      -- a session shorter than this isn't kept

-- A file's path as it's shown: what follows the game's own folder
-- (...\GarrysMod\garrysmod\), forward slashes. Any other path whole.
function M.shortName(name)
	local s = string.gsub(tostring(name or ""), "\\", "/")
	local lower, at, from = string.lower(s), nil, 1
	while true do
		local a, b = string.find(lower, "/garrysmod/", from, true)
		if not a then break end
		at, from = b, a + 1
	end
	return at and string.sub(s, at + 1) or s
end

function M.new()
	local s = { frames = 0, ms = 0, bins = {}, over = 0, overMs = 0, worst = 0, avg = nil, hitches = {}, hitchCount = 0 }
	for i = 1, M.BINS do s.bins[i] = 0 end
	return s
end

-- Whether a frame of `ms` is a hitch, given the recent average (nil at first).
function M.isHitch(ms, avg)
	if ms >= M.HITCH_MS then return true end
	return ms >= M.HITCH_REL_MS and avg ~= nil and ms >= avg * M.HITCH_REL
end

-- One frame. ms: its time. at: seconds into the session. files: nil, or a
-- function giving the files opened during it ({ { name, ms }, ... }), asked
-- only for a hitch. Returns whether it was one.
function M.add(s, ms, at, files)
	if type(ms) ~= "number" or ms ~= ms or ms <= 0 then return false end
	s.frames = s.frames + 1
	s.ms = s.ms + ms
	local bin = math.floor(ms / M.BIN_MS) + 1
	if bin > M.BINS then
		s.over, s.overMs = s.over + 1, s.overMs + ms
	else
		s.bins[bin] = s.bins[bin] + 1
	end
	if ms > s.worst then s.worst = ms end
	local hitch = M.isHitch(ms, s.avg)
	if hitch then
		s.hitchCount = s.hitchCount + 1
		local h = { at = at or 0, ms = ms, files = {} }
		local list = files and files() or nil
		if type(list) == "table" then
			-- The slowest opens first: they are the likeliest cause. Only the ones
			-- that took time are named; a frame can probe dozens of search
			-- folders for one file, each in microseconds.
			table.sort(list, function(a, b) return (tonumber(a.ms) or 0) > (tonumber(b.ms) or 0) end)
			h.opened, h.openMs = #list, 0
			for _, f in ipairs(list) do
				local t = tonumber(f.ms) or 0
				h.openMs = h.openMs + t
				if t >= M.SLOW_OPEN_MS and #h.files < M.KEEP_FILES then
					h.files[#h.files + 1] = { name = M.shortName(f.name), ms = t }
				end
			end
		end
		s.hitches[#s.hitches + 1] = h
		if #s.hitches > M.KEEP_HITCHES then table.remove(s.hitches, 1) end
	else
		-- The recent average follows ordinary frames only: a hitch doesn't make
		-- the next one look normal.
		s.avg = s.avg and (s.avg + (ms - s.avg) * 0.05) or ms
	end
	return hitch
end

-- The frame time below which `fraction` of the frames fall (0.5: the median).
function M.percentile(s, fraction)
	if s.frames == 0 then return 0 end
	local want, seen = s.frames * fraction, 0
	for i = 1, M.BINS do
		seen = seen + s.bins[i]
		if seen >= want then return (i - 0.5) * M.BIN_MS end
	end
	return s.over > 0 and s.overMs / s.over or M.BINS * M.BIN_MS
end

-- The average frame rate of the slowest `fraction` of frames (0.01: the 1% low).
function M.low(s, fraction)
	if s.frames == 0 then return 0 end
	local want = math.max(1, math.floor(s.frames * fraction + 0.5))
	local taken, ms = math.min(want, s.over), 0
	if taken > 0 then ms = s.overMs / s.over * taken end
	for i = M.BINS, 1, -1 do
		if taken >= want then break end
		local n = math.min(s.bins[i], want - taken)
		taken, ms = taken + n, ms + n * (i - 0.5) * M.BIN_MS
	end
	if ms <= 0 then return 0 end
	return taken * 1000 / ms
end

local function round(v, places)
	local m = 10 ^ (places or 0)
	return math.floor(v * m + 0.5) / m
end

-- What a session came to.
function M.summary(s)
	local seconds = s.ms / 1000
	return {
		frames = s.frames,
		seconds = round(seconds, 0),
		fps = s.ms > 0 and round(s.frames * 1000 / s.ms, 1) or 0,
		median = round(M.percentile(s, 0.5), 1),
		p99 = round(M.percentile(s, 0.99), 1),
		low1 = round(M.low(s, 0.01), 1),
		low01 = round(M.low(s, 0.001), 1),
		hitches = s.hitchCount,
		perMinute = seconds > 0 and round(s.hitchCount * 60 / seconds, 1) or 0,
		worst = round(s.worst, 0),
	}
end

-- One line of a summary.
function M.line(sum)
	return string.format("%d fps average, 1%% low %d fps, 0.1%% low %d fps, median frame %.1f ms, 99%% under %.1f ms, "
		.. "%d hitches (%.1f a minute), worst %d ms, over %d:%02d",
		math.floor(sum.fps + 0.5), math.floor(sum.low1 + 0.5), math.floor(sum.low01 + 0.5), sum.median, sum.p99,
		sum.hitches, sum.perMinute, sum.worst, math.floor(sum.seconds / 60), sum.seconds % 60)
end

-- One line of a hitch: when, how long, and what was opened.
function M.hitchLine(h)
	local text = string.format("%d:%02d  %d ms", math.floor(h.at / 60), math.floor(h.at % 60), math.floor(h.ms + 0.5))
	if h.opened == nil then return text end
	if h.opened == 0 then return text .. "  no file opened" end
	text = text .. string.format("  %d file%s opened, %.0f ms in all", h.opened, h.opened == 1 and "" or "s", h.openMs or 0)
	if #h.files == 0 then return text end
	local parts = {}
	for i, f in ipairs(h.files) do parts[i] = string.format("%s (%.0f ms)", f.name, f.ms) end
	return text .. ": " .. table.concat(parts, ", ")
end

-- --- The sessions kept (frames.txt) -----------------------------------------------
-- One line per session, the newest last: "key=value" pairs separated by spaces.
-- at: when it ended (unix time). server: its address. pack: the pack's version.
-- mcore: multicore rendering was on (0/1).

local FIELDS = { "at", "seconds", "frames", "fps", "low1", "low01", "median", "p99", "hitches", "worst", "mcore" }

local function word(v) return (string.gsub(tostring(v or ""), "[^%w%.:_/%-]", "")) end

function M.encodeSessions(list)
	local lines = {}
	for i = math.max(1, #list - M.KEEP_SESSIONS + 1), #list do
		local e, parts = list[i], {}
		for _, k in ipairs(FIELDS) do parts[#parts + 1] = k .. "=" .. string.format("%.10g", tonumber(e[k]) or 0) end
		parts[#parts + 1] = "server=" .. string.sub(word(e.server), 1, 48)
		parts[#parts + 1] = "pack=" .. string.sub(word(e.pack), 1, 24)
		lines[#lines + 1] = table.concat(parts, " ")
	end
	return #lines > 0 and table.concat(lines, "\n") .. "\n" or ""
end

function M.decodeSessions(text)
	local list = {}
	for line in string.gmatch((type(text) == "string" and text or "") .. "\n", "([^\n]*)\n") do
		local e, any = {}, false
		for k, v in string.gmatch(line, "([%w_]+)=(%S*)") do
			if k == "server" or k == "pack" then e[k] = word(v) else e[k] = tonumber(v) or 0 end
			any = true
		end
		if any and (e.frames or 0) > 0 then
			for _, k in ipairs(FIELDS) do e[k] = e[k] or 0 end
			e.server, e.pack = e.server or "", e.pack or ""
			list[#list + 1] = e
		end
	end
	while #list > M.KEEP_SESSIONS do table.remove(list, 1) end
	return list
end

-- A finished session as it's kept, or nil when it was too short to mean anything.
function M.session(s, at, server, pack, mcore)
	local sum = M.summary(s)
	if sum.seconds < M.MIN_SECONDS then return nil end
	return {
		at = math.floor(tonumber(at) or 0), seconds = sum.seconds, frames = sum.frames, fps = sum.fps, low1 = sum.low1,
		low01 = sum.low01, median = sum.median, p99 = sum.p99, hitches = sum.hitches, worst = sum.worst,
		mcore = mcore and 1 or 0, server = word(server), pack = word(pack),
	}
end

-- One line of a kept session.
function M.sessionLine(e)
	return string.format("%s  %d fps average, 1%% low %d, %d hitches, worst %d ms, %d min, multicore %s, pack %s",
		e.server ~= "" and e.server or "?", math.floor(e.fps + 0.5), math.floor(e.low1 + 0.5), e.hitches, e.worst,
		math.floor(e.seconds / 60 + 0.5), e.mcore == 1 and "on" or "off", e.pack ~= "" and e.pack or "?")
end

return M
