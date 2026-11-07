-- The Meowy console's rules (console.lua shows them). Runs as `ConsoleLogic`.
--
-- A line is { id, t (unix ms), s (severity: 0 message, 1 warning, 2 assert,
-- 3 error), c (0xRRGGBB or -1), x (the text) }, as meowy.ConsoleRead gives it
-- (console_capture.h). Everything here is plain Lua, so the tests run it as is.
local M = {}

M.MAX_KEEP = 50000   -- lines kept in the window
M.MAX_COPY = 5000    -- rows copied at most (the newest)

-- The rows shown: lines with the search text in them (plain text, any case);
-- with `badOnly`, warnings and errors only; with `collapse`, a line equal to
-- the shown line before it adds to that row's count instead.
function M.view(lines, query, badOnly, collapse)
	local q = (type(query) == "string" and query ~= "") and string.lower(query) or nil
	local out = {}
	for _, l in ipairs(lines) do
		if (not badOnly or (l.s or 0) >= 1) and (not q or string.find(string.lower(l.x or ""), q, 1, true)) then
			local last = out[#out]
			if collapse and last and last.line.x == l.x and last.line.s == l.s then
				last.count = last.count + 1
				last.lastT = l.t
			else
				out[#out + 1] = { line = l, count = 1, lastT = l.t }
			end
		end
	end
	return out
end

-- "21:04:12" for a time in ms (the PC's local time).
function M.clock(ms)
	return os.date("%H:%M:%S", math.floor((tonumber(ms) or 0) / 1000))
end

-- The rows as text: "21:04:12  text" (" (x3)" when collapsed), the newest
-- MAX_COPY at most.
function M.copyText(rows)
	local out, first = {}, math.max(1, #rows - M.MAX_COPY + 1)
	for i = first, #rows do
		local r = rows[i]
		out[#out + 1] = M.clock(r.line.t) .. "  " .. (r.line.x or "") .. (r.count > 1 and (" (x" .. r.count .. ")") or "")
	end
	return table.concat(out, "\n")
end

-- A typed line as the console reads it: commands split at ";" (outside quotes),
-- each into words; a quoted part is one word, quotes dropped.
function M.commands(line)
	local cmds, words, word, quoted, any = {}, {}, {}, false, false
	local function endWord()
		if any then words[#words + 1] = table.concat(word) end
		word, any = {}, false
	end
	local function endCommand()
		endWord()
		if #words > 0 then cmds[#cmds + 1] = words end
		words = {}
	end
	for ch in string.gmatch(type(line) == "string" and line or "", ".") do
		if quoted then
			if ch == '"' then quoted = false else word[#word + 1] = ch end
		elseif ch == '"' then
			quoted, any = true, true
		elseif ch == ";" then
			endCommand()
		elseif ch == " " or ch == "\t" then
			endWord()
		elseif ch ~= "\n" and ch ~= "\r" then
			word[#word + 1] = ch
			any = true
		end
	end
	endCommand()
	return cmds
end

-- Drops the oldest lines past `max`, in place.
function M.trim(lines, max)
	local extra = #lines - max
	if extra <= 0 then return lines end
	for i = 1, #lines - extra do lines[i] = lines[i + extra] end
	for i = #lines, #lines - extra + 1, -1 do lines[i] = nil end
	return lines
end

-- A line's colour: errors red, warnings yellow, otherwise its own or white.
function M.color(line)
	local s = line.s or 0
	if s >= 2 then return 255, 110, 110 end
	if s == 1 then return 255, 220, 120 end
	local c = tonumber(line.c) or -1
	if c < 0 then return 230, 230, 230 end
	return math.floor(c / 65536) % 256, math.floor(c / 256) % 256, c % 256
end

-- One line of the engine's `cvarlist`: "name   : value   : , "a", "cl"   : help"
-- -> { name, value, flags = { "a", "cl" }, help, cmd } (cmd: a command, whose
-- value column says "cmd"), or nil for anything else.
function M.parseCvar(text)
	if type(text) ~= "string" then return nil end
	local name, value, flags, help = string.match(text, "^([%w_%.%-%+]+)%s+:%s(.-)%s*:%s(.-)%s*:%s?(.*)$")
	if not name then return nil end
	local list = {}
	for f in string.gmatch(flags, '"([^"]+)"') do list[#list + 1] = f end
	return { name = name, value = value, flags = list, help = help or "", cmd = value == "cmd" }
end

-- Flags as words (the engine's short names in cvarlist).
M.FLAG_WORDS = {
	a = "saved", cl = "client", sv = "server", cheat = "cheat", rep = "replicated", nf = "not while connected",
	user = "user info", prot = "protected", sp = "single player", demo = "demo", norecord = "not in demos",
	ss = "split screen", ss_added = "split screen", clientcmd_can_execute = "server can run", server_can_execute = "server can run",
	nolog = "not logged", numeric = "number",
}
function M.flagWords(flags)
	local out = {}
	for _, f in ipairs(flags or {}) do out[#out + 1] = M.FLAG_WORDS[f] or f end
	return out
end

-- Suggestions for the word being typed, from the catalog (cvarlist entries):
-- names starting with it first (shortest first), then names containing it,
-- then names with its letters in order; at most `max`. Case doesn't matter.
function M.suggest(catalog, word, max)
	word = string.lower(type(word) == "string" and word or "")
	if word == "" then return {} end
	local starts, contains, loose = {}, {}, {}
	local pattern = word:gsub("[%^%$%(%)%%%.%[%]%*%+%-%?]", "%%%0"):gsub("(%%?.)", "%1.-")
	for _, e in ipairs(catalog or {}) do
		local n = string.lower(e.name)
		if string.sub(n, 1, #word) == word then starts[#starts + 1] = e
		elseif string.find(n, word, 1, true) then contains[#contains + 1] = e
		elseif #word >= 3 and string.find(n, pattern) then loose[#loose + 1] = e end
	end
	local function byLength(a, b) if #a.name ~= #b.name then return #a.name < #b.name end return a.name < b.name end
	table.sort(starts, byLength)
	table.sort(contains, byLength)
	table.sort(loose, byLength)
	local out = {}
	for _, group in ipairs({ starts, contains, loose }) do
		for _, e in ipairs(group) do
			if #out >= (max or 8) then return out end
			out[#out + 1] = e
		end
	end
	return out
end

-- The first word of a typed line, and whether more follows it.
function M.firstWord(line)
	local word, rest = string.match(type(line) == "string" and line or "", "^%s*([^%s;]*)(.*)$")
	return word or "", (rest or "") ~= ""
end

-- Saved lists (history, pinned commands) as text and back: one per line,
-- blank and over-long lines dropped, at most `max`, the newest last.
function M.parseList(text, max)
	local out = {}
	for line in string.gmatch((type(text) == "string" and text or "") .. "\n", "([^\r\n]*)\r?\n") do
		if line ~= "" and #line <= 1024 then out[#out + 1] = line end
	end
	while #out > (max or 200) do table.remove(out, 1) end
	return out
end
function M.formatList(list, max)
	local out, first = {}, math.max(1, #list - (max or 200) + 1)
	for i = first, #list do
		local s = tostring(list[i]):gsub("[\r\n]", " ")
		if s ~= "" then out[#out + 1] = s end
	end
	return table.concat(out, "\n")
end

-- History lines with `text` in them, newest first, each once.
function M.searchHistory(history, text, max)
	local q, seen, out = string.lower(text or ""), {}, {}
	for i = #history, 1, -1 do
		local h = history[i]
		if not seen[h] and (q == "" or string.find(string.lower(h), q, 1, true)) then
			seen[h] = true
			out[#out + 1] = h
			if #out >= (max or 8) then break end
		end
	end
	return out
end

-- Up/down through the typed history: `at` 0 is "nothing recalled"; returns the
-- new position and the line to show.
function M.recall(history, at, dir)
	local n = #history
	if n == 0 then return 0, "" end
	at = math.max(0, math.min(n, at + dir))
	if at == 0 then return 0, "" end
	return at, history[n - at + 1]
end

return M
