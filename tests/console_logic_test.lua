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

print(("%d checks, %d failed"):format(count, failures))
os.exit(failures == 0 and 0 or 1)
