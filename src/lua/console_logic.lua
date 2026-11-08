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

-- A line's kind, for the source chips: "error", "warning", "pack", "network",
-- "workshop" or "other" (worked out once, kept on the line as `k`).
M.KINDS = { "error", "warning", "network", "workshop", "pack" }
local NETWORK = { "connecting to", "connected to", "disconnect", "dropped", "timed out", "timing out", "retrying",
	"steam auth", "lost connection", "kicked", "server is full", "signon", "connection to server" }
local WORKSHOP = { "workshop", "mounting", "mounted", "downloading", "downloaded", ".gma", "fastdl" }
function M.kind(line)
	if line.k then return line.k end
	local x, s, k = string.lower(line.x or ""), line.s or 0, nil
	if s >= 2 or string.find(x, "[error]", 1, true) or string.find(x, "lua error", 1, true) then k = "error"
	elseif s == 1 or string.find(x, "^warning") then k = "warning"
	elseif string.find(x, "^%[meowy client pack%]") then k = "pack"
	else
		for _, w in ipairs(NETWORK) do if string.find(x, w, 1, true) then k = "network" break end end
		if not k then for _, w in ipairs(WORKSHOP) do if string.find(x, w, 1, true) then k = "workshop" break end end end
	end
	line.k = k or "other"
	return line.k
end

-- How many lines of each kind.
function M.counts(lines)
	local out = { error = 0, warning = 0, network = 0, workshop = 0, pack = 0, other = 0 }
	for _, l in ipairs(lines) do
		if not l.marker then
			local k = M.kind(l)
			out[k] = (out[k] or 0) + 1
		end
	end
	return out
end

-- Whether `query` works as a Lua pattern (the ".*" search).
function M.validPattern(query)
	return (pcall(string.find, "", query))
end

-- Where `query` is in `text`: { {first, last}, ... } (byte positions), plain
-- text any case, or a Lua pattern; at most 32.
function M.spans(text, query, pattern)
	local out = {}
	if type(text) ~= "string" or type(query) ~= "string" or query == "" then return out end
	if pattern and not M.validPattern(query) then return out end
	local hay, q = string.lower(text), pattern and query or string.lower(query)
	local at = 1
	while at <= #hay and #out < 32 do
		local a, b = string.find(hay, q, at, not pattern)
		if not a then break end
		if b < a then b = a end   -- an empty match: one character, and move on
		out[#out + 1] = { a, b }
		at = b + 1
	end
	return out
end

M.FLOOD_ROWS, M.FLOOD_SECONDS = 20, 10

-- The rows shown. opts:
--   query, pattern     the search (plain text, or a Lua pattern);
--   find               true: every line stays and matches are only marked
--                      (false: only matching lines);
--   kinds              a set of kinds to show (nil or empty: every kind);
--   collapse           a line equal to the one shown before it adds to its count;
--   flood              with collapse, a line equal to one shown in the last
--                      FLOOD_ROWS rows within FLOOD_SECONDS adds to that row too.
-- Markers (lines with `marker`) always show and never fold. A row: { line,
-- count, firstT, lastT, match (it has the search text) }.
function M.rows(lines, opts)
	opts = opts or {}
	local query = (type(opts.query) == "string" and opts.query ~= "") and opts.query or nil
	if query and opts.pattern and not M.validPattern(query) then query = nil end
	local anyKind = not opts.kinds or next(opts.kinds) == nil
	local out, recent = {}, {}
	for _, l in ipairs(lines) do
		if l.marker then
			out[#out + 1] = { line = l, count = 1, firstT = l.t, lastT = l.t, marker = true }
			recent = {}
		elseif (anyKind or opts.kinds[M.kind(l)]) and M.passes(opts.conFilter, l.x) then
			local match = query and #M.spans(l.x, query, opts.pattern) > 0
			if opts.find or not query or match then
				local last, folded = out[#out], false
				if opts.collapse and last and not last.marker and last.line.x == l.x and last.line.s == l.s then
					last.count, last.lastT, folded = last.count + 1, l.t, true
				elseif opts.collapse and opts.flood then
					local row = recent[l.x]
					if row and #out - row.at < M.FLOOD_ROWS and (l.t or 0) - (row.r.lastT or 0) <= M.FLOOD_SECONDS * 1000 then
						row.r.count, row.r.lastT, folded = row.r.count + 1, l.t, true
					end
				end
				if not folded then
					out[#out + 1] = { line = l, count = 1, firstT = l.t, lastT = l.t, match = match or false }
					recent[l.x] = { r = out[#out], at = #out }
				end
			end
		end
	end
	return out
end

-- Rows with the search text, as row numbers (for next and previous).
function M.matchRows(rows)
	local out = {}
	for i, r in ipairs(rows) do if r.match then out[#out + 1] = i end end
	return out
end

-- How long a folded row went on: "", "in 4 s", "in 2 min".
function M.span(row)
	local s = math.floor(((row.lastT or 0) - (row.firstT or 0)) / 1000)
	if row.count <= 1 or s < 1 then return "" end
	if s < 120 then return "in " .. s .. " s" end
	return "in " .. math.floor(s / 60) .. " min"
end

-- "21:04:12" for a time in ms (the PC's local time).
function M.clock(ms)
	return os.date("%H:%M:%S", math.floor((tonumber(ms) or 0) / 1000))
end

-- The rows as text: "21:04:12  text" (" (x3)" when collapsed), the newest
-- MAX_COPY at most.
function M.copyText(rows, max)
	local out, first = {}, math.max(1, #rows - (max or M.MAX_COPY) + 1)
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

-- A typed line split at ";" (outside quotes), each part as typed (trimmed).
function M.parts(line)
	local out, cur, quoted = {}, {}, false
	local function finish()
		local p = string.match(table.concat(cur), "^%s*(.-)%s*$")
		if p ~= "" then out[#out + 1] = p end
		cur = {}
	end
	for ch in string.gmatch(type(line) == "string" and line or "", ".") do
		if ch == '"' then quoted = not quoted end
		if ch == ";" and not quoted then
			finish()
		elseif ch ~= "\n" and ch ~= "\r" then
			cur[#cur + 1] = ch
		end
	end
	finish()
	return out
end

-- A typed line as the Meowy console runs it. Commands for the game's console
-- window act on this one instead: `clear` empties it (an echoed mark goes
-- first, so what ran before it is cleared and what runs after it stays; the
-- game's own console is cleared too), and while the console key opens this
-- window, `showconsole` leaves it open and `hideconsole` and `toggleconsole`
-- close it (the game's console would only flash). Returns the line for the
-- engine (as typed when none of these is in it; "" when nothing is left) and
-- whether to close the window. `mark` is the clear mark to echo (console.lua's
-- own, with a part no server knows; M.CLEAR_MARK without one).
M.CLEAR_MARK = "__meowy_console_clear__"
function M.prepare(line, ownsKey, mark)
	mark = mark or M.CLEAR_MARK
	local out, close, changed = {}, false, false
	for _, part in ipairs(M.parts(line)) do
		local w = string.lower((M.firstWord(part)))
		if w == "clear" then
			out[#out + 1] = "echo " .. mark
			out[#out + 1] = part
			changed = true
		elseif ownsKey and (w == "hideconsole" or w == "toggleconsole") then
			close, changed = true, true
		elseif ownsKey and w == "showconsole" then
			changed = true
		else
			out[#out + 1] = part
		end
	end
	if not changed then return type(line) == "string" and line or "", false end
	return table.concat(out, ";"), close
end

-- Commands whose words are a secret (a password, an rcon command, a server's
-- Steam token): a line with one isn't kept in the history, and its echo shows
-- the command without them.
M.SECRET_COMMANDS = { password = true, rcon_password = true, sv_password = true, rcon = true,
	sv_setsteamaccount = true, cl_steamauth_token = true }
-- Commands that hold another command line (a key's or an alias's), whose
-- words from the third on are checked the same way.
M.WRAPPERS = { bind = true, bindtoggle = true, bind_mac = true, alias = true }
local secretPart
local function secretLine(line, depth)
	for _, part in ipairs(M.parts(line)) do
		if secretPart(part, depth) then return true end
	end
	return false
end
-- "secret": the part's own command is one; "wrapped": a bind or alias holds one.
function secretPart(part, depth)
	local word = string.lower((string.gsub(M.firstWord(part), '"', "")))
	if M.SECRET_COMMANDS[word] then return "secret" end
	if M.WRAPPERS[word] and (depth or 0) < 3 then
		local words = M.commands(part)[1] or {}
		if #words >= 3 and secretLine(table.concat(words, " ", 3), (depth or 0) + 1) then return "wrapped" end
	end
	return nil
end
function M.secret(line)
	return secretLine(type(line) == "string" and line or "", 0)
end
function M.masked(line)
	if not M.secret(line) then return type(line) == "string" and line or "" end
	local out = {}
	for _, part in ipairs(M.parts(line)) do
		local kind = secretPart(part, 0)
		if kind == "secret" then
			local word, more = M.firstWord(part)
			out[#out + 1] = more and (word .. " ********") or word
		elseif kind == "wrapped" then
			local words = M.commands(part)[1]
			out[#out + 1] = words[1] .. " " .. words[2] .. " ********"
		else
			out[#out + 1] = part
		end
	end
	return table.concat(out, "; ")
end
-- History as saved by an older version: without the secret lines.
function M.withoutSecrets(list)
	local out = {}
	for _, line in ipairs(list or {}) do if not M.secret(line) then out[#out + 1] = line end end
	return out
end

-- Whether a setting's default can be put back as one value: a server's Lua can
-- make any default, and a quote, ";" or line break in it would be a second
-- command.
function M.plainValue(v)
	return type(v) == "string" and not string.find(v, '[";\r\n]') and #v <= 512
end

-- The game's con_filter, which acts on its own console's text and so never
-- on these lines: with con_filter_enable 1, only lines with con_filter_text
-- (when set) and without con_filter_text_out (when set), any case. nil when
-- it's off.
function M.conFilter(enable, text, out)
	if tonumber(enable) ~= 1 then return nil end
	text, out = string.lower(text or ""), string.lower(out or "")
	if text == "" and out == "" then return nil end
	return { text = text, out = out }
end
function M.passes(filter, x)
	if not filter then return true end
	x = string.lower(x or "")
	if filter.text ~= "" and not string.find(x, filter.text, 1, true) then return false end
	if filter.out ~= "" and string.find(x, filter.out, 1, true) then return false end
	return true
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
	nolog = "not logged", numeric = "number", pack = "Meowy Client Pack",
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

-- Lua errors, as OnLuaError gives them (message, realm, stack, addon title,
-- Workshop ID), grouped by addon and then by message: { [addon key] = { title,
-- id, last, count, errors = { [message] = { message, count, first, last, realm,
-- stack, server } } } }. `now` is unix seconds. At most 200 messages an addon.
function M.recordError(groups, message, realm, stack, title, id, now, server)
	message = tostring(message or "?")
	local wsid = (id ~= nil and tostring(id) ~= "0" and tostring(id) ~= "") and tostring(id) or nil
	local key = wsid or (type(title) == "string" and title) or "unknown"
	local g = groups[key]
	if not g then
		g = { key = key, title = type(title) == "string" and title or nil, id = wsid, count = 0, errors = {}, n = 0 }
		groups[key] = g
	end
	local e = g.errors[message]
	if not e then
		if g.n >= 200 then return g end
		e = { message = message, count = 0, first = now, realm = realm, stack = type(stack) == "table" and stack or {} }
		g.errors[message] = e
		g.n = g.n + 1
	end
	e.count, e.last, e.server = e.count + 1, now, server or e.server
	e.realm = realm or e.realm
	g.count, g.last = g.count + 1, now
	return g
end

-- The groups, newest first, each with its errors newest first: a list of
-- { group, errors = { ... } }.
function M.errorList(groups)
	local out = {}
	for _, g in pairs(groups) do
		local errs = {}
		for _, e in pairs(g.errors) do errs[#errs + 1] = e end
		table.sort(errs, function(a, b) if a.last ~= b.last then return a.last > b.last end return a.message < b.message end)
		out[#out + 1] = { group = g, errors = errs }
	end
	table.sort(out, function(a, b) if a.group.last ~= b.group.last then return a.group.last > b.group.last end return a.group.key < b.group.key end)
	return out
end

-- How many errors in all.
function M.errorCount(groups)
	local n = 0
	for _, g in pairs(groups) do n = n + g.count end
	return n
end

-- A stack as lines: "1. Draw - addons/foo/lua/autorun/cl_foo.lua:12".
function M.stackLines(stack, max)
	local out = {}
	for i, s in ipairs(type(stack) == "table" and stack or {}) do
		if #out >= (max or 8) then break end
		local fn = (type(s.Function) == "string" and s.Function ~= "") and s.Function or "unknown"
		out[#out + 1] = i .. ". " .. fn .. " - " .. tostring(s.File or "?") .. ":" .. tostring(s.Line or "?")
	end
	return out
end

-- An error as a Markdown report: the error and its stack, the addon (and when
-- it was last updated), how often and where, and the game and pack versions.
-- Paths are masked by the caller (meowy.MaskPaths).
function M.report(group, e, info)
	info = info or {}
	local addon = group.title or "an unknown addon"
	if group.id then addon = addon .. " (Workshop " .. group.id .. (info.updated and (", updated " .. info.updated) or "") .. ")" end
	local where = e.server and ("on " .. e.server) or "at the main menu"
	local out = {
		"**Lua error** in " .. addon,
		e.count .. "x, first " .. M.clock((e.first or 0) * 1000) .. ", last " .. M.clock((e.last or 0) * 1000)
			.. ", " .. tostring(e.realm or "?") .. " realm, " .. where,
		"```",
		e.message,
	}
	for _, line in ipairs(M.stackLines(e.stack, 12)) do out[#out + 1] = line end
	out[#out + 1] = "```"
	if info.game then out[#out + 1] = info.game end
	return table.concat(out, "\n")
end

-- The badge's text: "3 Lua errors in Foo HUD" / "… in 2 addons".
function M.badgeText(groups)
	local n, addons, only = 0, 0, nil
	for _, g in pairs(groups) do
		n, addons = n + g.count, addons + 1
		only = g.title or "an unknown addon"
	end
	if n == 0 then return nil end
	local what = n == 1 and "1 Lua error" or (n .. " Lua errors")
	return addons == 1 and (what .. " in " .. only) or (what .. " in " .. addons .. " addons")
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
