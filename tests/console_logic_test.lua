-- The Meowy console's rules (src/lua/console_logic.lua), no game: lua tests/console_logic_test.lua
local here = (arg and arg[0] or ""):match("^(.*)[/\\]") or "."
local function readFile(p) local f = assert(io.open(p, "rb")) local s = f:read("*a") f:close() return s end
local load = loadstring or load
local M = assert(load(readFile(here .. "/../src/lua/console_logic.lua"), "console_logic"))()

local count, failures = 0, 0
local function check(name, got, want)
	count = count + 1
	if got ~= want then
		failures = failures + 1
		print(("FAIL %s: got %s, want %s"):format(name, tostring(got), tostring(want)))
	end
end

local function L(id, s, x, t) return { id = id, t = t or id * 1000, s = s, c = -1, x = x } end
local lines = {
	L(1, 0, "Mounted 3 addons"),
	L(2, 1, "Warning: missing texture"),
	L(3, 3, "[ERROR] lua/x.lua:1: attempt to index nil"),
	L(4, 3, "[ERROR] lua/x.lua:1: attempt to index nil"),
	L(5, 3, "[ERROR] lua/x.lua:1: attempt to index nil"),
	L(6, 0, "Mounted 3 addons"),
}

-- view
local v = M.view(lines, "", false, false)
check("all lines", #v, 6)
v = M.view(lines, "", false, true)
check("repeats collapsed", #v, 4)
check("collapsed count", v[3].count, 3)
check("collapsed: the last time kept", v[3].lastT, 5000)
check("not collapsed when not next to each other", v[4].line.id, 6)
v = M.view(lines, "", true, true)
check("warnings and errors only", #v, 2)
v = M.view(lines, "MOUNTED", false, false)
check("search, any case", #v, 2)
v = M.view(lines, "x.lua:1", false, false)
check("search is plain text (dots and colons)", #v, 3)
v = M.view(lines, "%d", false, false)
check("search: pattern characters are text", #v, 0)

-- commands
local function words(line)
	local out = {}
	for _, c in ipairs(M.commands(line)) do out[#out + 1] = table.concat(c, "|") end
	return table.concat(out, " / ")
end
check("one command", words("net_graph 1"), "net_graph|1")
check("several, split at ;", words("net_graph 1; cl_showfps 1"), "net_graph|1 / cl_showfps|1")
check("quoted words kept whole", words('say "hello; there" now'), "say|hello; there|now")
check("an empty quoted word", words('name ""'), "name|")
check("blank parts dropped", words(" ; ;; status ;"), "status")
check("nothing", #M.commands(""), 0)
check("not a string", #M.commands(nil), 0)

-- parts and prepare: the game's console-window commands act on this window
check("parts: as typed, split outside quotes", table.concat(M.parts(' status ;say "a; b" ; ;retry'), "|"), 'status|say "a; b"|retry')
local MARK = "echo " .. M.CLEAR_MARK
local sent, close = M.prepare("clear", true)
check("prepare: clear marks the window, then clears the game's", sent, MARK .. ";clear")
check("prepare: clear doesn't close", close, false)
check("prepare: clear in any case, in the middle", (M.prepare("status; CLEAR; net_graph 1", true)), "status;" .. MARK .. ";CLEAR;net_graph 1")
check("prepare: a word starting with clear isn't clear", (M.prepare("cleardecals", true)), "cleardecals")
check("prepare: quoted ; kept", (M.prepare('say "clear; x"', true)), 'say "clear; x"')
sent, close = M.prepare("showconsole", true)
check("prepare: showconsole leaves it open", sent .. "|" .. tostring(close), "|false")
sent, close = M.prepare("toggleconsole", true)
check("prepare: toggleconsole closes it", sent .. "|" .. tostring(close), "|true")
sent, close = M.prepare("status; hideconsole", true)
check("prepare: hideconsole closes it, the rest runs", sent .. "|" .. tostring(close), "status|true")
sent, close = M.prepare("toggleconsole", false)
check("prepare: the key not taken over, the game's console as asked", sent .. "|" .. tostring(close), "toggleconsole|false")

-- con_filter, as the game's console does it
check("con_filter: off", M.conFilter(0, "a", ""), nil)
check("con_filter: on without text", M.conFilter("1", "", ""), nil)
local cf = M.conFilter("1", "Connect", "")
check("con_filter: lines with the text, any case", tostring(M.passes(cf, "Connecting to x")) .. tostring(M.passes(cf, "hello")), "truefalse")
cf = M.conFilter(1, "", "spam")
check("con_filter: the out text hidden", tostring(M.passes(cf, "SPAM here")) .. tostring(M.passes(cf, "fine")), "falsetrue")
check("con_filter: none passes everything", M.passes(nil, "x"), true)

-- copyText, clock
local text = M.copyText(M.view(lines, "", false, true))
check("copied as lines", select(2, text:gsub("\n", "")) + 1, 4)
check("copied with the count", text:find("attempt to index nil (x3)", 1, true) ~= nil, true)
check("a clock", M.clock(0):match("^%d%d:%d%d:%d%d$") ~= nil, true)
local many = {}
for i = 1, M.MAX_COPY + 10 do many[i] = { line = L(i, 0, "l" .. i), count = 1 } end
local copied = M.copyText(many)
check("at most MAX_COPY rows copied", select(2, copied:gsub("\n", "")) + 1, M.MAX_COPY)
check("the newest copied", copied:sub(-#("l" .. (M.MAX_COPY + 10))), "l" .. (M.MAX_COPY + 10))

-- trim
local t = { 1, 2, 3, 4, 5 }
M.trim(t, 3)
check("trim keeps the newest", table.concat(t, ","), "3,4,5")
M.trim(t, 10)
check("trim: under the limit, unchanged", table.concat(t, ","), "3,4,5")
-- With slack: nothing until past max + slack, then back to max in one go.
t = {}
for i = 1, 12 do t[i] = i end
M.trim(t, 8, 4)
check("trim with slack: within it, unchanged", #t, 12)
t[13] = 13
M.trim(t, 8, 4)
check("trim with slack: past it, back to max", #t, 8)
check("trim with slack: the newest kept, in order", table.concat(t, ","), "6,7,8,9,10,11,12,13")
check("trim with slack: nothing left past the end", t[9], nil)
local big = {}
for i = 1, M.MAX_KEEP + M.TRIM_SLACK + 1 do big[i] = i end
M.trim(big, M.MAX_KEEP, M.TRIM_SLACK)
check("trim: the window's numbers", #big == M.MAX_KEEP and big[1] == M.TRIM_SLACK + 2 and big[#big] == M.MAX_KEEP + M.TRIM_SLACK + 1, true)

-- The window's own lines (dividers, echoes) placed among the console's
-- lines by id: each just before the first line past its afterId.
local function merge(reads, inserts, from)
	local out = {}
	for _, read in ipairs(reads) do
		for _, l in ipairs(read.lines) do
			from = M.placeDue(inserts, from, l.id, out)
			out[#out + 1] = l
		end
		from = M.placeDue(inserts, from, read.last + 1, out)
	end
	return out, from
end
local function texts(list)
	local o = {}
	for _, l in ipairs(list) do o[#o + 1] = l.x end
	return table.concat(o, ",")
end
local ins = {}
M.addInsert(ins, { afterId = 2, line = { x = "Joining", marker = true } })
M.addInsert(ins, { afterId = 5, line = { x = "In game", marker = true } })
local merged, from = merge({ { lines = { L(1, 0, "a"), L(2, 0, "b"), L(3, 0, "c"), L(4, 0, "d"), L(5, 0, "e"), L(6, 0, "f") }, last = 6 } }, ins, 1)
check("markers land between the right lines", texts(merged), "a,b,Joining,c,d,e,In game,f")
check("markers: all placed", from, 3)
-- Read in pieces (the window open through the join): the same places.
merged, from = merge({ { lines = { L(1, 0, "a"), L(2, 0, "b") }, last = 2 }, { lines = { L(3, 0, "c"), L(4, 0, "d") }, last = 4 },
	{ lines = {}, last = 5 }, { lines = { L(6, 0, "f") }, last = 6 } }, ins, 1)
check("markers: read in pieces, the same places", texts(merged), "a,b,Joining,c,d,In game,f")
-- Made after the newest line: placed at the end of the read that reaches it.
merged, from = merge({ { lines = { L(3, 0, "c") }, last = 3 } }, ins, 2)
check("markers: one made at the newest line, at the end", texts(merged), "c")
check("markers: not placed before its line", from, 2)
merged, from = merge({ { lines = { L(4, 0, "d"), L(5, 0, "e") }, last = 5 } }, ins, from)
check("markers: placed once its line is read", texts(merged) .. "|" .. from, "d,e,In game|3")
-- An echo made at the same id as a divider goes after it; never among those placed.
ins = {}
M.addInsert(ins, { afterId = 7, line = { x = "Left" } })
M.addInsert(ins, { afterId = 4, line = { x = "] status" } })
check("addInsert: in id order", ins[1].line.x .. "," .. ins[2].line.x, "] status,Left")
M.addInsert(ins, { afterId = 7, line = { x = "] echo" } })
check("addInsert: after one made at the same id", ins[3].line.x, "] echo")
M.addInsert(ins, { afterId = 1, line = { x = "late" } }, nil, 3)
check("addInsert: never among the placed ones", ins[3].line.x, "late")
local dropped = M.addInsert(ins, { afterId = 9, line = { x = "z" } }, 3)
check("addInsert: the oldest go past max", dropped .. " " .. #ins .. " " .. ins[1].line.x, "2 3 late")

-- color
check("error red", select(1, M.color({ s = 3 })), 255)
check("warning yellow", select(2, M.color({ s = 1 })), 220)
local r, g, b = M.color({ s = 0, c = 0x3366FF })
check("a line's own colour", r .. "," .. g .. "," .. b, "51,102,255")
check("no colour: white-ish", select(1, M.color({ s = 0, c = -1 })), 230)

-- recall
local h = { "first", "second", "third" }
local at, line = M.recall(h, 0, 1)
check("up: the newest", line, "third")
at, line = M.recall(h, at, 1)
check("up again: older", line, "second")
at, line = M.recall(h, at, 1)
at, line = M.recall(h, at, 1)
check("up stops at the oldest", line, "first")
at, line = M.recall(h, at, -1)
check("down: newer", line, "second")
at, line = M.recall(h, 1, -1)
check("down past the newest: empty", line .. "|" .. at, "|0")
check("no history", select(2, M.recall({}, 0, 1)), "")

-- parseCvar: the engine's cvarlist lines
local e = M.parseCvar('net_graph                                : 1        : , "a", "cl"      : Draw the network usage graph, = 2 draws data')
check("cvar: name", e and e.name, "net_graph")
check("cvar: value", e and e.value, "1")
check("cvar: flags", e and table.concat(e.flags, ","), "a,cl")
check("cvar: help keeps commas", e and e.help, "Draw the network usage graph, = 2 draws data")
check("cvar: not a command", e and e.cmd, false)
local cmd = M.parseCvar("retry                                    : cmd      :                  : Retry connection to last server.")
check("command: no flags", cmd and #cmd.flags, 0)
check("command: marked", cmd and cmd.cmd, true)
check("no help: empty", M.parseCvar('net_graphpos : 1 : , "a" : ').help, "")
check("headers aren't entries", M.parseCvar("cvar list"), nil)
check("rules aren't entries", M.parseCvar("--------------"), nil)
check("the count isn't an entry", M.parseCvar("  9 convars/concommands for [net_graph]"), nil)
check("flag words", table.concat(M.flagWords({ "a", "cheat", "weird" }), ","), "saved,cheat,weird")

-- suggest: starting with it, then containing it, then letters in order
local cat = {
	M.parseCvar('net_graph : 1 : , "a" : g'), M.parseCvar('net_graphheight : 64 : , "a" : h'),
	M.parseCvar('cl_showfps : 0 : , "cl" : fps'), M.parseCvar('mat_queue_mode : -1 : , "a" : q'),
	M.parseCvar('r_drawviewmodel : 1 : , "cl" : vm'),
}
local function names(list) local out = {} for _, s in ipairs(list) do out[#out + 1] = s.name end return table.concat(out, ",") end
check("suggest: starting with it, shortest first", names(M.suggest(cat, "net", 8)), "net_graph,net_graphheight")
check("suggest: any case", names(M.suggest(cat, "NET_GRAPHH", 8)), "net_graphheight")
check("suggest: containing it, after", names(M.suggest(cat, "fps", 8)), "cl_showfps")
check("suggest: letters in order (3 or more)", names(M.suggest(cat, "mqm", 8)), "mat_queue_mode")
check("suggest: not for two letters", names(M.suggest(cat, "mq", 8)), "")
check("suggest: at most max", #M.suggest(cat, "e", 1), 1)
check("suggest: pattern characters are plain", names(M.suggest(cat, "%.", 8)), "")
check("suggest: nothing typed", #M.suggest(cat, "", 8), 0)

-- firstWord
local w1, more1 = M.firstWord("  net_graph 1")
check("first word", w1 .. "|" .. tostring(more1), "net_graph|true")
local w2, more2 = M.firstWord("retry")
check("first word alone", w2 .. "|" .. tostring(more2), "retry|false")
check("first word before ;", (M.firstWord("status;echo hi")), "status")

-- saved lists
local list = M.parseList("a\r\nb\n\nc\n" .. string.rep("x", 1100) .. "\n", 200)
check("list: blank and long lines dropped", table.concat(list, ","), "a,b,c")
check("list: at most max, the newest kept", table.concat(M.parseList("1\n2\n3\n4", 2), ","), "3,4")
check("list: back to text", M.formatList({ "a", "b\nc" }, 10), "a\nb c")
check("list: round trip", table.concat(M.parseList(M.formatList({ "x", "y" }, 10), 10), ","), "x,y")

-- searchHistory: newest first, each once
local hist = { "status", "net_graph 1", "retry", "net_graph 0", "status" }
check("history search", table.concat(M.searchHistory(hist, "net", 8), "|"), "net_graph 0|net_graph 1")
check("history: all, newest first, once each", table.concat(M.searchHistory(hist, "", 8), "|"), "status|net_graph 0|retry|net_graph 1")

-- kinds, for the source chips
check("kind: error by severity", M.kind({ s = 3, x = "x" }), "error")
check("kind: [ERROR] text", M.kind({ s = 0, x = "[ERROR] lua/a.lua:1: oops" }), "error")
check("kind: warning", M.kind({ s = 1, x = "x" }), "warning")
check("kind: the pack", M.kind({ s = 0, x = "[Meowy Client Pack] prewarm: ok" }), "pack")
check("kind: network", M.kind({ s = 0, x = "Connecting to 1.2.3.4:27015..." }), "network")
check("kind: workshop", M.kind({ s = 0, x = "Mounted 3 of 3 workshop addons!" }), "workshop")
check("kind: other", M.kind({ s = 0, x = "hello" }), "other")
local kl = { { s = 3, x = "a" }, { s = 1, x = "b" }, { s = 0, x = "Dropped from server" }, { s = 0, x = "x", marker = true } }
local counts = M.counts(kl)
check("counts", counts.error .. counts.warning .. counts.network .. counts.other, "1110")

-- spans: where the search text is
local sp = M.spans("Net graph NET", "net", false)
check("spans: any case, every one", #sp .. ":" .. sp[1][1] .. "-" .. sp[1][2] .. "," .. sp[2][1] .. "-" .. sp[2][2], "2:1-3,11-13")
check("spans: a Lua pattern", #M.spans("rp_downtown_v2 rp_city", "rp_%a+", true), 2)
check("spans: a broken pattern finds nothing", #M.spans("abc", "[a", true), 0)
check("spans: plain text keeps pattern characters", #M.spans("100% done", "100%", false), 1)
check("valid pattern", tostring(M.validPattern("a%d+")) .. tostring(M.validPattern("[a")), "truefalse")

-- rows: filter or mark, kinds, collapse and flood, markers
local rl = {
	L(1, 0, "alpha", 1000), L(2, 3, "boom", 2000), L(3, 0, "spam", 3000), L(4, 0, "other", 3500),
	L(5, 0, "spam", 4000), L(6, 0, "spam", 5000), L(7, 0, "beta", 6000),
}
rl[8] = { id = -2, t = 7000, s = 0, x = "Joining 1.2.3.4:27015", marker = true }
rl[9] = L(9, 0, "spam", 8000)
local rows = M.rows(rl, { query = "a", find = false })
check("rows: filter keeps matching lines (and markers)", #rows, 7)
rows = M.rows(rl, { query = "spam", find = true })
check("rows: mark keeps every line", #rows, 9)
check("rows: matches marked", #M.matchRows(rows), 4)
rows = M.rows(rl, { kinds = { error = true } })
check("rows: one kind (and markers)", #rows, 2)
rows = M.rows(rl, { conFilter = M.conFilter(1, "SPAM", "") })
check("rows: con_filter (and markers)", #rows, 5)
rows = M.rows(rl, { collapse = true })
check("rows: only next-to-each-other repeats folded", #rows, 8)
rows = M.rows(rl, { collapse = true, flood = true })
check("rows: flood folds a repeat a few rows back", #rows, 7)
local spamRow
for _, r in ipairs(rows) do if r.line.x == "spam" and not spamRow then spamRow = r end end
check("rows: flood count", spamRow.count, 3)
check("rows: a marker ends the flood window", rows[#rows].line.x .. rows[#rows].count, "spam1")
check("span: how long it went on", M.span({ count = 3, firstT = 3000, lastT = 5000 }), "in 2 s")
check("span: nothing for one", M.span({ count = 1, firstT = 0, lastT = 9000 }), "")
check("span: minutes", M.span({ count = 2, firstT = 0, lastT = 300000 }), "in 5 min")
local old = { L(1, 0, "x", 0) }
for i = 2, M.FLOOD_ROWS + 5 do old[i] = L(i, 0, "filler " .. i, i) end
old[#old + 1] = L(99, 0, "x", 100)
check("rows: too far back isn't flood", #M.rows(old, { collapse = true, flood = true }), #old)

-- The clear mark console.lua passes (its own, per start).
check("prepare: a given mark", (M.prepare("clear", true, "__m_1f__")), "echo __m_1f__;clear")

-- Secrets: lines with a password or rcon aren't kept, and their echo hides it.
check("secret: rcon_password", M.secret("rcon_password hunter2"), true)
check("secret: any case, any part", M.secret('connect 1.2.3.4:27015; PASSWORD "x"'), true)
check("secret: rcon", M.secret("rcon kick bob"), true)
check("secret: quoted command", M.secret('"sv_password" x'), true)
check("secret: a word that only starts like one", M.secret("passwordless 1"), false)
check("secret: inside a quoted say", M.secret('say "password hunter2"'), false)
check("secret: plain", M.secret("net_graph 1"), false)
check("masked: the secret part", M.masked('connect 1.2.3.4:27015; password "hunter2"'), "connect 1.2.3.4:27015; password ********")
check("masked: the command alone", M.masked("rcon_password"), "rcon_password")
check("masked: plain as typed", M.masked("say hi;  retry"), "say hi;  retry")
check("withoutSecrets", table.concat(M.withoutSecrets({ "a", "rcon_password x", "b", "password y" }), ","), "a,b")
-- A bind or alias that holds one is a secret too, however it's quoted.
check("secret: a bind holding rcon_password", M.secret('bind k "rcon_password hunter2"'), true)
check("secret: an alias holding one in its second command", M.secret('alias go "status; rcon_password hunter2"'), true)
check("secret: unquoted", M.secret("bind k rcon_password hunter2"), true)
check("secret: an alias of an alias", M.secret('alias a "alias b password"'), true)
check("secret: an ordinary bind", M.secret('bind k "say hi"'), false)
check("secret: a bind with no command", M.secret("bind k"), false)
check("masked: the bind's command hidden", M.masked('status; bind k "rcon_password hunter2"'), "status; bind k ********")

-- A default that can be put back as one value.
check("plainValue: a number", M.plainValue("0"), true)
check("plainValue: text", M.plainValue("some text"), true)
check("plainValue: a quote", M.plainValue('0";bind w kill;"'), false)
check("plainValue: a ;", M.plainValue("0;unbindall"), false)
check("plainValue: a line break", M.plainValue("0\nunbindall"), false)
check("plainValue: not a string", M.plainValue(nil), false)

print(("%d checks, %d failed"):format(count, failures))
os.exit(failures == 0 and 0 or 1)
