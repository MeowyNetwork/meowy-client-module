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

print(("%d checks, %d failed"):format(count, failures))
os.exit(failures == 0 and 0 or 1)
