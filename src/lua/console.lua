-- The Meowy console (roadmap U8): the game's console lines (console_capture.h,
-- read with meowy.ConsoleRead), searchable, warnings and errors only, repeats
-- collapsed, copied as text, and a command line with history. `meowy_console`
-- opens it, at the main menu or over the game. Runs in a function of its own at
-- the end of resilience.lua's chunk, with ConsoleLogic (console_logic.lua).
--
-- It looks like the rest of the pack (ui.lua): a dark window drawn by hand with
-- a soft shadow, the pink accent used sparingly, Roboto, and a monospaced log.
-- A typed line is the player's own and runs as if typed into the game's console
-- (meowy.ConsoleRun: the engine's ClientCmd_Unrestricted).
if not (meowy.ConsoleCaptureStart and meowy.ConsoleRead) then return end

local started, why = meowy.ConsoleCaptureStart()
if not started then log("console: not capturing: %s", tostring(why)) end

local C, S = UI.C, UI.S
local PULL = 0.25          -- seconds between reads while the window is open
local READ_MAX = 5000      -- lines per read
local HISTORY_MAX, PINS_MAX = 200, 24
local CATALOG_AGE = 120    -- seconds before the command list is read again
local ERROR = Color(238, 104, 112)
local lines, lastId = {}, 0
local holder

-- What you typed (kept between starts) and the pinned commands.
local function loadList(kind, max)
	return ConsoleLogic.parseList(meowy.ConsoleFileRead and meowy.ConsoleFileRead(kind) or "", max)
end
local function saveList(kind, list, max)
	if meowy.ConsoleFileWrite then meowy.ConsoleFileWrite(kind, ConsoleLogic.formatList(list, max)) end
end
local history, recallAt = loadList("history", HISTORY_MAX), 0
local pins = loadList("pins", PINS_MAX)

-- Every command and setting, with its value, flags and help: the engine's own
-- `cvarlist`, run between two markers; its lines are read into the catalog and
-- left out of the window.
local BEGIN, FINISH = "__meowy_cvars_begin__", "__meowy_cvars_end__"
local catalog, catalogAt, collecting, asked = {}, nil, nil, false
local function askCatalog()
	if asked or not meowy.ConsoleRun then return end
	if catalogAt and SysTime() - catalogAt < CATALOG_AGE then return end
	asked = true
	meowy.ConsoleRun("echo " .. BEGIN)
	meowy.ConsoleRun("cvarlist")
	meowy.ConsoleRun("echo " .. FINISH)
end

local function pull()
	local got, last = meowy.ConsoleRead(lastId, READ_MAX)
	local added = 0
	if istable(got) then
		for _, l in ipairs(got) do
			local x = string.match(l.x or "", "^%s*(.-)%s*$")   -- echo adds a space at the end
			if x == BEGIN then
				collecting = {}
			elseif collecting and x == FINISH then
				if #collecting > 0 then catalog, catalogAt = collecting, SysTime() end
				collecting, asked = nil, false
			elseif collecting then
				local e = ConsoleLogic.parseCvar(x)
				if e then collecting[#collecting + 1] = e end
			else
				lines[#lines + 1] = l
				added = added + 1
			end
		end
	end
	if isnumber(last) then lastId = last end
	ConsoleLogic.trim(lines, ConsoleLogic.MAX_KEEP)
	return added
end

local function run(line)
	if not isstring(line) or line:match("^%s*$") then return end
	if history[#history] ~= line then
		history[#history + 1] = line
		while #history > HISTORY_MAX do table.remove(history, 1) end
		saveList("history", history, HISTORY_MAX)
	end
	recallAt = 0
	-- Echoed like the game's console does, so the answer reads in context.
	lines[#lines + 1] = { id = -1, t = os.time() * 1000, s = 0, c = 0x9A9EA8, x = "] " .. line }
	if meowy.ConsoleRun and meowy.ConsoleRun(line) then return end
	for _, words in ipairs(ConsoleLogic.commands(line)) do RunConsoleCommand(unpack(words)) end
end

local function pin(line)
	if not isstring(line) or line:match("^%s*$") then return end
	for i = #pins, 1, -1 do if pins[i] == line then table.remove(pins, i) end end
	pins[#pins + 1] = line
	while #pins > PINS_MAX do table.remove(pins, 1) end
	saveList("pins", pins, PINS_MAX)
end
local function unpin(line)
	for i = #pins, 1, -1 do if pins[i] == line then table.remove(pins, i) end end
	saveList("pins", pins, PINS_MAX)
end

local monoFor
local function fonts()
	UI.fonts()
	if monoFor == ScrH() then return end
	monoFor = ScrH()
	surface.CreateFont("MeowyPack.Mono", { font = "Consolas", size = S(15), weight = 500, antialias = true, extended = true })
end

-- A text box in the kit's style: a surface field, the accent edge while typing.
-- The field (e.box, which callers place) holds the text box set in by `inset`,
-- so an optional `prefix` mark drawn in that margin never meets the text.
local function field(parent, placeholder, inset, prefix)
	inset = inset or S(10)
	local box = vgui.Create("DPanel", parent)
	local e = vgui.Create("DTextEntry", box)
	e.box = box
	e:Dock(FILL)
	e:DockMargin(inset, 0, S(8), 0)
	e:SetFont("MeowyPack.Body")
	e:SetPlaceholderText(placeholder)
	e:SetPaintBackground(false)
	e:SetTextColor(C.text)
	e:SetCursorColor(C.text)
	e:SetHighlightColor(C.accentSoft)
	e:SetPlaceholderColor(C.dim)
	box.Paint = function(self, w, h)
		UI.box(S(8), 0, 0, w, h, C.surface, e:HasFocus() and C.accent or C.border)
		if prefix then draw.SimpleText(prefix, "MeowyPack.Heading", S(12), h / 2 - S(1), C.accent, UI.LEFT, UI.CENTER) end
		return true
	end
	box.OnMousePressed = function() e:RequestFocus() end
	e.Paint = function(self, w, h)
		self:DrawTextEntryText(C.text, C.accentSoft, C.text)
		if self:GetValue() == "" and not self:HasFocus() then
			draw.SimpleText(placeholder, "MeowyPack.Body", 0, h / 2, C.dim, UI.LEFT, UI.CENTER)
		end
		return true
	end
	return e
end

-- A pill that turns on and off.
local function pill(parent, text, on, onChange)
	local p = vgui.Create("DButton", parent)
	p:SetText("")
	p.on = on
	p:SetSize(UI.textW(text, "MeowyPack.Small") + S(28), S(30))
	p.Paint = function(self, w, h)
		local t = UI.hover(self, self:IsHovered())
		-- Opaque: UI.box paints the border colour under the fill.
		local fill = self.on and UI.mix(C.surface, C.accent, 0.22 + 0.08 * t) or UI.mix(C.surface, C.raised, t)
		UI.box(h / 2, 0, 0, w, h, fill, self.on and C.accent or C.border)
		draw.SimpleText(text, "MeowyPack.Small", w / 2, h / 2, self.on and C.text or C.body, UI.CENTER, UI.CENTER)
		return true
	end
	p.DoClick = function(self) self.on = not self.on onChange(self.on) end
	return p
end

local function open()
	if IsValid(holder) then
		holder:MakePopup()
		holder:MoveToFront()
		return
	end
	pull()
	fonts()
	local M, R, PAD = S(24), S(14), S(20)   -- shadow margin, corner radius, padding
	local state = { query = "", kinds = {}, counts = {}, find = false, pattern = false, collapse = true, view = {},
		offset = 0, follow = true, note = nil, noteUntil = 0, unseen = 0, cur = nil, selA = nil, selB = nil }
	surface.SetFont("MeowyPack.Mono")
	local _, lh = surface.GetTextSize("Ag")
	lh = lh + S(4)

	holder = vgui.Create("EditablePanel")
	holder.meowyConsole = true
	local cw = math.min(ScrW() - S(80), S(1080))
	local ch = math.min(ScrH() - S(80), S(680))
	holder:SetSize(cw + M * 2, ch + M * 2)
	holder:Center()
	holder.Paint = function(self, w, h)
		UI.shadow(R, M, M, w - M * 2, h - M * 2)
		UI.box(R, M, M, w - M * 2, h - M * 2, C.window, C.border)
	end

	-- Header: title, how many lines (or a short note), close.
	local header = vgui.Create("DPanel", holder)
	header.Paint = function(self, w, h)
		draw.SimpleText("Console", "MeowyPack.Title", 0, h / 2, C.text, UI.LEFT, UI.CENTER)
		local tw = UI.textW("Console", "MeowyPack.Title")
		local note = (state.note and SysTime() < state.noteUntil) and state.note
			or (#state.view == #lines and (#lines .. " lines") or (#state.view .. " of " .. #lines .. " lines"))
		draw.SimpleText(note, "MeowyPack.Small", tw + S(14), h / 2 + S(2), C.dim, UI.LEFT, UI.CENTER)
	end
	-- Dragging by the header.
	header.OnMousePressed = function(self, code)
		if code ~= MOUSE_LEFT then return end
		self.drag = { gui.MouseX() - holder.x, gui.MouseY() - holder.y }
		self:MouseCapture(true)
	end
	header.OnMouseReleased = function(self) self.drag = nil self:MouseCapture(false) end
	header.Think = function(self)
		if not self.drag then return end
		if not input.IsMouseDown(MOUSE_LEFT) then self.drag = nil self:MouseCapture(false) return end
		holder:SetPos(math.Clamp(gui.MouseX() - self.drag[1], -M, ScrW() - holder:GetWide() + M),
			math.Clamp(gui.MouseY() - self.drag[2], -M, ScrH() - holder:GetTall() + M))
	end
	local close = UI.Close(holder, function() if IsValid(holder) then holder:Remove() end end)

	-- Toolbar: search (only the lines with it, or every line with it marked and
	-- Enter going from one to the next), Lua patterns, Copy and Clear.
	local search = field(holder, "Search   (Ctrl+F)")
	search:SetUpdateOnType(true)
	local rebuild, jump
	search.OnValueChange = function(_, v) state.query = v or "" state.cur = nil rebuild() end
	search.OnKeyCodeTyped = function(self, code)
		if code == KEY_ENTER or code == KEY_F3 then
			jump((input and input.IsShiftDown and input.IsShiftDown()) and -1 or 1)
			return true
		end
		if code == KEY_ESCAPE then
			if (self:GetValue() or "") ~= "" then self:SetText("") state.query = "" rebuild() else holder:Remove() end
			return true
		end
	end
	local findPill = pill(holder, "Mark, don't filter", false, function(v) state.find = v rebuild() end)
	local patternPill = pill(holder, "Lua pattern", false, function(v) state.pattern = v rebuild() end)
	local collapse = pill(holder, "Collapse repeats", true, function(v) state.collapse = v rebuild() end)
	-- What kinds of lines show: none picked is every kind. Each with its count.
	local KIND_LABELS = { error = "Errors", warning = "Warnings", network = "Network", workshop = "Workshop", pack = "Meowy" }
	local kindChips = {}
	for _, k in ipairs(ConsoleLogic.KINDS) do
		local b = vgui.Create("DButton", holder)
		b:SetText("")
		b.meowyKind = k
		b.DoClick = function()
			state.kinds[k] = (not state.kinds[k]) or nil
			rebuild()
		end
		b.Paint = function(self, w, h)
			local on, t = state.kinds[k], UI.hover(self, self:IsHovered())
			local fill = on and UI.mix(C.surface, C.accent, 0.22 + 0.08 * t) or UI.mix(C.surface, C.raised, t)
			UI.box(h / 2, 0, 0, w, h, fill, on and C.accent or C.border)
			local n = state.counts[k] or 0
			local label = KIND_LABELS[k]
			local lw = UI.textW(label, "MeowyPack.Small")
			draw.SimpleText(label, "MeowyPack.Small", S(12), h / 2, on and C.text or C.body, UI.LEFT, UI.CENTER)
			local color = (k == "error" and n > 0) and ERROR or (k == "warning" and n > 0) and C.amber or C.dim
			draw.SimpleText(tostring(n), "MeowyPack.Small", S(12) + lw + S(7), h / 2, color, UI.LEFT, UI.CENTER)
			return true
		end
		b:SetSize(UI.textW(KIND_LABELS[k], "MeowyPack.Small") + UI.textW("0000", "MeowyPack.Small") + S(24), S(30))
		kindChips[#kindChips + 1] = b
	end
	local function selectedRows()
		if not state.selA then return nil end
		local a, b = math.min(state.selA, state.selB), math.max(state.selA, state.selB)
		local out = {}
		for i = a, math.min(b, #state.view) do out[#out + 1] = state.view[i] end
		return #out > 0 and out or nil
	end
	local function copyRows(rows, what)
		SetClipboardText(ConsoleLogic.copyText(rows))
		state.note, state.noteUntil = "Copied " .. math.min(#rows, ConsoleLogic.MAX_COPY) .. " " .. what, SysTime() + 2
	end
	local copy = UI.Button(holder, "Copy", "secondary", function()
		local sel = selectedRows()
		if sel then copyRows(sel, "selected lines") else copyRows(state.view, "lines") end
	end)
	local clear = UI.Button(holder, "Clear", "ghost", function()
		for i = #lines, 1, -1 do lines[i] = nil end
		rebuild()
	end)

	-- Pinned commands: one click runs one; right-click takes it off.
	local pinsRow = vgui.Create("DPanel", holder)
	pinsRow.Paint = function() end
	local entry
	local function refreshPins()
		for _, c in ipairs(pinsRow:GetChildren() or {}) do c:Remove() end
		local x = 0
		local function chip(text, tip, primary, onClick, onRight)
			local b = vgui.Create("DButton", pinsRow)
			b:SetText("")
			b:SetTooltip(tip)
			local label = #text > 32 and (text:sub(1, 31) .. "\226\128\166") or text
			local font = primary and "MeowyPack.Small" or "MeowyPack.Mono"
			b:SetSize(UI.textW(label, font) + S(22), S(28))
			b:SetPos(x, S(2))
			x = x + b:GetWide() + S(6)
			b.Paint = function(self, w, h)
				local t = UI.hover(self, self:IsHovered())
				UI.box(S(8), 0, 0, w, h, UI.mix(C.surface, C.raised, t), primary and C.border or UI.mix(C.border, C.accent, 0.5))
				draw.SimpleText(label, font, w / 2, h / 2, primary and C.dim or C.text, UI.CENTER, UI.CENTER)
				return true
			end
			b.DoClick = onClick
			b.DoRightClick = onRight
			b.meowyPin = primary and "+" or text
		end
		for _, cmd in ipairs(pins) do
			chip(cmd, "Run it (right-click: unpin)", false, function() run(cmd) state.follow = true end,
				function() unpin(cmd) refreshPins() end)
		end
		chip("+ Pin a command", "Pins what's typed below, or the last command you ran", true, function()
			local text = entry:GetValue()
			pin((text and text ~= "") and text or history[#history])
			refreshPins()
		end)
	end

	-- The log: a card, monospaced lines, a slim scrollbar.
	local logPanel = vgui.Create("DPanel", holder)
	entry = field(holder, "Type a command and press Enter    (Tab completes, Up and Down: earlier ones, Ctrl+R: search them)", S(30), "\226\128\186")
	entry:SetFont("MeowyPack.Mono")

	-- Above the command line: what matches the word being typed (with values and
	-- help), the setting or command once it's typed (its value, default, flags,
	-- help, and a reset), or earlier commands (Ctrl+R).
	local pop = vgui.Create("DPanel", holder)
	pop:SetVisible(false)
	local ROW = S(42)
	local p = { mode = nil, items = {}, sel = 1, dismissed = false, history = false, entry = nil }
	local reset = UI.Button(pop, "Reset to default", "secondary", function()
		if p.entry then run(p.entry.name .. ' "' .. tostring(p.default or "") .. '"') end
	end)
	reset:SetVisible(false)
	local function liveValue(e)
		local cv = (not e.cmd) and GetConVar and GetConVar(e.name) or nil
		if cv then return cv:GetString(), cv:GetDefault() end
		return e.value, nil
	end
	local function layoutPop()
		if not p.mode then pop:SetVisible(false) reset:SetVisible(false) return end
		local w = math.min(entry.box:GetWide(), S(720))
		local h
		if p.mode == "inspect" then
			h = S(92)
		else
			h = #p.items * ROW + S(12)
		end
		pop:SetSize(w, h)
		local bx, by = entry.box:GetPos()
		pop:SetPos(bx or 0, (by or 0) - h - S(6))
		pop:SetVisible(true)
		pop:MoveToFront()
		local showReset = p.mode == "inspect" and p.value ~= nil and p.default ~= nil and p.value ~= p.default
		reset:SetVisible(showReset)
		if showReset then reset:SetPos(w - reset:GetWide() - S(12), h - reset:GetTall() - S(12)) end
	end
	local function updatePop()
		local text = entry:GetValue() or ""
		local word, more = ConsoleLogic.firstWord(text)
		p.mode, p.items, p.entry = nil, {}, nil
		if p.history then
			p.items = ConsoleLogic.searchHistory(history, text, 8)
			if #p.items > 0 then p.mode = "history" end
		elseif not p.dismissed and word ~= "" then
			local exact
			for _, e in ipairs(catalog) do if string.lower(e.name) == string.lower(word) then exact = e break end end
			if more or (exact and #ConsoleLogic.suggest(catalog, word, 2) == 1) then
				if exact then
					p.mode, p.entry = "inspect", exact
					p.value, p.default = liveValue(exact)
				end
			else
				p.items = ConsoleLogic.suggest(catalog, word, 8)
				if #p.items > 0 then p.mode = "suggest" end
			end
		end
		p.sel = math.min(math.max(p.sel, 1), math.max(#p.items, 1))
		layoutPop()
	end
	local function accept(i)
		local item = p.items[i or p.sel]
		if not item then return end
		local text = p.mode == "history" and item or (item.name .. " ")
		p.history = false
		entry:SetText(text)
		entry:SetCaretPos(#text)
		entry:RequestFocus()
		updatePop()
	end
	pop.Paint = function(self, w, h)
		UI.box(S(10), 0, 0, w, h, C.window, C.border)
		if p.mode == "inspect" and p.entry then
			local e = p.entry
			draw.SimpleText(e.name, "MeowyPack.Mono", S(14), S(18), C.text, UI.LEFT, UI.CENTER)
			local nw = UI.textW(e.name, "MeowyPack.Mono")
			local fx = S(14) + nw + S(14)
			if e.cmd then
				draw.SimpleText("command", "MeowyPack.Mono", fx, S(18), C.dim, UI.LEFT, UI.CENTER)
			else
				local value = "= " .. tostring(p.value)
				draw.SimpleText(value, "MeowyPack.Mono", fx, S(18), C.accentHi, UI.LEFT, UI.CENTER)
				if p.default then
					draw.SimpleText("default " .. p.default, "MeowyPack.Mono", fx + UI.textW(value, "MeowyPack.Mono") + S(18), S(18), C.dim, UI.LEFT, UI.CENTER)
				end
			end
			local flags = table.concat(ConsoleLogic.flagWords(e.flags), " \194\183 ")
			draw.SimpleText(flags, "MeowyPack.Small", S(14), S(42), C.dim, UI.LEFT, UI.CENTER)
			local help = e.help ~= "" and e.help or "No description."
			draw.SimpleText(#help > 120 and (help:sub(1, 119) .. "\226\128\166") or help, "MeowyPack.Small", S(14), S(66), C.body, UI.LEFT, UI.CENTER)
			return true
		end
		for i, item in ipairs(p.items) do
			local y = S(6) + (i - 1) * ROW
			if i == p.sel then draw.RoundedBox(S(6), S(6), y, w - S(12), ROW, C.raised) end
			if p.mode == "history" then
				draw.SimpleText(item, "MeowyPack.Mono", S(16), y + ROW / 2, C.text, UI.LEFT, UI.CENTER)
			else
				draw.SimpleText(item.name, "MeowyPack.Mono", S(16), y + S(13), C.text, UI.LEFT, UI.CENTER)
				local value = item.cmd and "command" or tostring((liveValue(item)))
				draw.SimpleText(value, "MeowyPack.Mono", w - S(16), y + S(13), item.cmd and C.dim or C.accentHi, UI.RIGHT, UI.CENTER)
				local help = item.help or ""
				draw.SimpleText(#help > 90 and (help:sub(1, 89) .. "\226\128\166") or help, "MeowyPack.Small", S(16), y + S(30), C.dim, UI.LEFT, UI.CENTER)
			end
		end
		return true
	end
	pop.OnMousePressed = function(self, code)
		if code ~= MOUSE_LEFT or p.mode == "inspect" then return end
		local _, my = self:CursorPos()
		local i = math.floor((my - S(6)) / ROW) + 1
		if p.items[i] then accept(i) end
	end
	entry.OnChange = function()
		p.dismissed = false
		recallAt = 0
		askCatalog()
		updatePop()
	end
	entry.OnGetFocus = function() askCatalog() end

	local function maxOffset() return math.max(0, #state.view * lh - (logPanel:GetTall() - S(12))) end
	rebuild = function()
		state.view = ConsoleLogic.rows(lines, { query = state.query, pattern = state.pattern, find = state.find,
			kinds = state.kinds, collapse = state.collapse, flood = state.collapse })
		state.counts = ConsoleLogic.counts(lines)
		state.matches = ConsoleLogic.matchRows(state.view)
		if state.follow then
			state.offset, state.unseen = maxOffset(), 0
		else
			state.offset = math.min(state.offset, maxOffset())
		end
		if state.selA and state.selA > #state.view then state.selA, state.selB = nil, nil end
	end
	-- Next (1) or previous (-1) row with the search text, scrolled into the middle.
	jump = function(dir)
		local m = state.matches or {}
		if #m == 0 then return end
		local at = 1
		if state.cur then
			for i, r in ipairs(m) do if r == state.cur then at = i + dir break end end
			if at < 1 then at = #m elseif at > #m then at = 1 end
		elseif dir < 0 then
			at = #m
		end
		state.cur = m[at]
		state.follow = false
		state.offset = math.Clamp((state.cur - 1) * lh - logPanel:GetTall() / 2, 0, maxOffset())
		state.note, state.noteUntil = at .. " of " .. #m .. " matches", SysTime() + 3
	end

	-- Right-click on a line: a small menu in the pack's style.
	local menu
	local function closeMenu() if IsValid(menu) then menu:Remove() end menu = nil end
	local function openMenu(row)
		closeMenu()
		local text = row.line.x or ""
		local items = {}
		local sel = selectedRows()
		items[#items + 1] = { "Copy line", function() SetClipboardText(text) state.note, state.noteUntil = "Line copied", SysTime() + 1.5 end }
		if sel and #sel > 1 then items[#items + 1] = { "Copy " .. #sel .. " selected lines", function() copyRows(sel, "selected lines") end } end
		items[#items + 1] = { "Search for this", function()
			local q = text:sub(1, 64)
			search:SetText(q) state.query, state.find, state.cur = q, true, nil
			findPill.on = true
			rebuild() jump(1)
		end }
		items[#items + 1] = { "Only lines like this", function()
			local q = text:gsub("%d+", ""):sub(1, 40):match("^%s*(.-)%s*$")
			search:SetText(q) state.query, state.find = q, false
			findPill.on = false
			rebuild()
		end }
		local file = text:match("([%w_%-%./]+%.lua)")
		if file then items[#items + 1] = { "Which addon has " .. file:match("[^/]+$"), function() run("whereis " .. file) state.follow = true end } end
		menu = vgui.Create("DPanel", holder)
		local mw = S(40)
		for _, it in ipairs(items) do mw = math.max(mw, UI.textW(it[1], "MeowyPack.Body") + S(32)) end
		local ih = S(32)
		menu:SetSize(mw, #items * ih + S(8))
		local mx, my = holder:CursorPos()
		menu:SetPos(math.min(mx or 0, (holder:GetWide() or 0) - mw - S(30)), math.min(my or 0, (holder:GetTall() or 0) - (menu:GetTall() or 0) - S(30)))
		menu.Paint = function(self, w, h) UI.box(S(8), 0, 0, w, h, C.window, C.border) end
		for i, it in ipairs(items) do
			local b = vgui.Create("DButton", menu)
			b:SetText("")
			b:SetPos(S(4), S(4) + (i - 1) * ih)
			b:SetSize(mw - S(8), ih)
			b.meowyMenuItem = it[1]
			b.Paint = function(self, w, h)
				if self:IsHovered() then draw.RoundedBox(S(6), 0, 0, w, h, C.raised) end
				draw.SimpleText(it[1], "MeowyPack.Body", S(12), h / 2, C.text, UI.LEFT, UI.CENTER)
				return true
			end
			b.DoClick = function() closeMenu() it[2]() end
		end
		menu:MoveToFront()
	end
	state.openMenu = openMenu   -- for the tests

	local function rowAt(my)
		return math.floor((my - S(6) + state.offset % lh) / lh) + math.floor(state.offset / lh) + 1
	end
	local function pillRect(w, h)
		local label = state.unseen .. " new line" .. (state.unseen == 1 and "" or "s") .. "  \226\134\147"
		local pw = UI.textW(label, "MeowyPack.Small") + S(26)
		return (w - pw) / 2, h - S(40), pw, S(28), label
	end

	logPanel.Paint = function(self, w, h)
		UI.box(S(10), 0, 0, w, h, C.surface, C.border)
		local top = S(6)
		local first = math.floor(state.offset / lh) + 1
		local shift = state.offset % lh
		local tw = UI.textW("00:00:00", "MeowyPack.Mono") + S(14)
		local mx, my = self:CursorPos()
		local hoverRow = self:IsHovered() and (math.floor((my - top + shift) / lh) + first) or nil
		local selLo = state.selA and math.min(state.selA, state.selB)
		local selHi = state.selA and math.max(state.selA, state.selB)
		for i = first, math.min(#state.view, first + math.ceil(h / lh) + 1) do
			local row = state.view[i]
			local y = top + (i - first) * lh - shift
			if row.marker then
				-- A marker: a line across with its label (joined, in game, left).
				surface.SetDrawColor(C.accent.r, C.accent.g, C.accent.b, 90)
				surface.DrawRect(S(12), y + lh / 2, w - S(40), 1)
				local label = "  " .. ConsoleLogic.clock(row.line.t) .. "  " .. (row.line.x or "") .. "  "
				local lw = UI.textW(label, "MeowyPack.Small")
				draw.RoundedBox(S(6), S(40), y + S(2), lw, lh - S(4), C.surface)
				draw.SimpleText(label, "MeowyPack.Small", S(40), y + lh / 2, C.accentHi, UI.LEFT, UI.CENTER)
			else
				if selLo and i >= selLo and i <= selHi then
					draw.RoundedBox(S(4), S(4), y, w - S(18), lh, Color(C.accent.r, C.accent.g, C.accent.b, 48))
				elseif i == hoverRow then
					draw.RoundedBox(S(4), S(4), y, w - S(18), lh, Color(255, 255, 255, 6))
				end
				draw.SimpleText(ConsoleLogic.clock(row.line.t), "MeowyPack.Mono", S(12), y + lh / 2, C.dim, UI.LEFT, UI.CENTER)
				local text = row.line.x or ""
				-- The search text, marked.
				if row.match and state.query ~= "" then
					for _, sp in ipairs(ConsoleLogic.spans(text, state.query, state.pattern)) do
						local x0 = S(12) + tw + UI.textW(text:sub(1, sp[1] - 1), "MeowyPack.Mono")
						local sw = math.max(S(4), UI.textW(text:sub(sp[1], sp[2]), "MeowyPack.Mono"))
						draw.RoundedBox(S(3), x0 - S(1), y + S(1), sw + S(2), lh - S(2), i == state.cur and Color(226, 168, 62, 150) or Color(226, 168, 62, 60))
					end
				end
				local s = row.line.s or 0
				local col = s >= 2 and ERROR or s == 1 and C.amber or Color(ConsoleLogic.color(row.line))
				draw.SimpleText(text, "MeowyPack.Mono", S(12) + tw, y + lh / 2, col, UI.LEFT, UI.CENTER)
				if row.count > 1 then
					local span = ConsoleLogic.span(row)
					local label = "x" .. row.count .. (span ~= "" and (" " .. span) or "")
					local cwid = UI.textW(label, "MeowyPack.Small") + S(14)
					draw.RoundedBox(S(8), w - cwid - S(22), y + S(2), cwid, lh - S(4), C.raised)
					draw.SimpleText(label, "MeowyPack.Small", w - cwid / 2 - S(22), y + lh / 2, C.body, UI.CENTER, UI.CENTER)
				end
			end
		end
		-- New lines below while scrolled up: a pill that goes back down.
		if state.unseen > 0 and not state.follow then
			local px, py, pw, ph, label = pillRect(w, h)
			UI.box(ph / 2, px, py, pw, ph, C.accent)
			draw.SimpleText(label, "MeowyPack.Small", px + pw / 2, py + ph / 2, C.white, UI.CENTER, UI.CENTER)
		end
		-- Scrollbar: only when there's more than fits.
		local total = #state.view * lh
		if total > h - top * 2 then
			local trackH = h - S(16)
			local thumbH = math.max(S(30), trackH * (h / total))
			local thumbY = S(8) + (trackH - thumbH) * (state.offset / math.max(1, maxOffset()))
			local over = mx >= w - S(16)
			draw.RoundedBox(S(3), w - S(10), thumbY, S(5), thumbH, (over or self.dragging) and Color(90, 92, 102) or C.raised)
		end
		if #state.view == 0 then
			draw.SimpleText(#lines == 0 and "Nothing in the console yet" or "Nothing matches", "MeowyPack.Body", w / 2, h / 2, C.dim, UI.CENTER, UI.CENTER)
		end
	end
	logPanel.OnMouseWheeled = function(_, delta)
		state.offset = math.Clamp(state.offset - delta * lh * 3, 0, maxOffset())
		state.follow = state.offset >= maxOffset() - 1
		return true
	end
	logPanel:SetKeyboardInputEnabled(true)
	logPanel.OnMousePressed = function(self, code)
		closeMenu()
		local mx, my = self:CursorPos()
		local i = rowAt(my)
		if code == MOUSE_RIGHT then
			local row = state.view[i]
			if row and not row.marker then
				if not (state.selA and i >= math.min(state.selA, state.selB) and i <= math.max(state.selA, state.selB)) then
					state.selA, state.selB = i, i
				end
				openMenu(row)
			end
			return
		end
		if code ~= MOUSE_LEFT then return end
		self:RequestFocus()
		if mx >= self:GetWide() - S(16) then
			self.dragging = true
			self:MouseCapture(true)
			return
		end
		if state.unseen > 0 and not state.follow then
			local px, py, pw, ph = pillRect(self:GetWide(), self:GetTall())
			if mx >= px and mx <= px + pw and my >= py and my <= py + ph then
				state.follow, state.unseen = true, 0
				rebuild()
				return
			end
		end
		-- Press and drag selects lines; Ctrl+C or Copy copies them.
		if state.view[i] then
			if input.IsShiftDown and input.IsShiftDown() and state.selA then state.selB = i else state.selA, state.selB = i, i end
			self.selecting = true
			self:MouseCapture(true)
		else
			state.selA, state.selB = nil, nil
		end
	end
	logPanel.OnMouseReleased = function(self)
		self.dragging, self.selecting = false, false
		self:MouseCapture(false)
	end
	logPanel.Think = function(self)
		if self.selecting then
			if not input.IsMouseDown(MOUSE_LEFT) then self.selecting = false self:MouseCapture(false) return end
			local _, my = self:CursorPos()
			-- Near the top or bottom edge, the log scrolls along.
			if my < S(10) then state.offset = math.max(0, state.offset - lh) state.follow = false
			elseif my > self:GetTall() - S(10) then state.offset = math.min(maxOffset(), state.offset + lh) end
			state.selB = math.Clamp(rowAt(my), 1, #state.view)
			return
		end
		if not self.dragging then return end
		if not input.IsMouseDown(MOUSE_LEFT) then self.dragging = false self:MouseCapture(false) return end
		local _, my = self:CursorPos()
		state.offset = math.Clamp((my - S(8)) / math.max(1, self:GetTall() - S(16)) * maxOffset(), 0, maxOffset())
		state.follow = state.offset >= maxOffset() - 1
	end
	logPanel.OnKeyCodePressed = function(self, code)
		local ctrl = input.IsControlDown and input.IsControlDown()
		if ctrl and code == KEY_C then
			local sel = selectedRows()
			if sel then copyRows(sel, "selected lines") end
			return true
		end
		if ctrl and code == KEY_A then state.selA, state.selB = 1, #state.view return true end
		if ctrl and code == KEY_F then search:RequestFocus() return true end
		if code == KEY_ESCAPE then
			if state.selA then state.selA, state.selB = nil, nil else holder:Remove() end
			return true
		end
	end

	-- The command line: history on Up and Down (its prompt mark is the field's).
	entry.OnEnter = function(self)
		run(self:GetValue())
		self:SetText("")
		self:RequestFocus()
		p.history, p.dismissed = false, false
		updatePop()
		state.follow = true
		rebuild()
	end
	entry.OnKeyCodeTyped = function(self, code)
		local listing = p.mode == "suggest" or p.mode == "history"
		if code == KEY_TAB then
			if listing then accept() elseif p.mode == nil then askCatalog() end
			return true
		end
		if code == KEY_UP or code == KEY_DOWN then
			if listing then
				p.sel = math.Clamp(p.sel + (code == KEY_DOWN and 1 or -1), 1, #p.items)
				return true
			end
			local text
			recallAt, text = ConsoleLogic.recall(history, recallAt, code == KEY_UP and 1 or -1)
			self:SetText(text)
			self:SetCaretPos(#text)
			p.dismissed = true   -- a recalled line runs as is; no list over it
			updatePop()
			return true
		end
		if code == KEY_F and input and input.IsControlDown and input.IsControlDown() then
			search:RequestFocus()
			return true
		end
		if code == KEY_R and input and input.IsControlDown and input.IsControlDown() then
			p.history = not p.history
			p.sel = 1
			updatePop()
			return true
		end
		if code == KEY_ESCAPE then
			if p.mode then p.dismissed, p.history = true, false updatePop() else holder:Remove() end
			return true
		end
		if code == KEY_ENTER then self:OnEnter() return true end
	end

	-- Resizing from the bottom-right corner.
	local grip = vgui.Create("DButton", holder)
	grip:SetText("")
	grip:SetCursor("sizenwse")
	grip.Paint = function(self, w, h)
		surface.SetDrawColor(C.dim.r, C.dim.g, C.dim.b, self:IsHovered() and 255 or 120)
		for i = 1, 3 do surface.DrawLine(w - S(4) - i * S(4), h - S(4), w - S(4), h - S(4) - i * S(4)) end
		return true
	end
	grip.OnMousePressed = function(self) self.size = { gui.MouseX() - holder:GetWide(), gui.MouseY() - holder:GetTall() } self:MouseCapture(true) end
	grip.OnMouseReleased = function(self) self.size = nil self:MouseCapture(false) end
	grip.Think = function(self)
		if not self.size then return end
		if not input.IsMouseDown(MOUSE_LEFT) then self.size = nil self:MouseCapture(false) return end
		holder:SetSize(math.Clamp(gui.MouseX() - self.size[1], S(560) + M * 2, ScrW()), math.Clamp(gui.MouseY() - self.size[2], S(340) + M * 2, ScrH()))
	end

	holder.PerformLayout = function(self, w, h)
		local x, y, iw = M + PAD, M + S(10), w - M * 2 - PAD * 2
		header:SetPos(x, y)
		header:SetSize(iw - S(40), S(46))
		close:SetPos(M + (w - M * 2) - PAD - close:GetWide() + S(6), y + S(8))
		y = y + S(52)
		search.box:SetPos(x, y)
		search.box:SetSize(math.min(S(320), iw * 0.35), S(36))
		findPill:SetPos(x + search.box:GetWide() + S(12), y + S(3))
		patternPill:SetPos((findPill.x or 0) + findPill:GetWide() + S(8), y + S(3))
		clear:SetPos(x + iw - clear:GetWide(), y - S(1))
		copy:SetPos((clear.x or 0) - copy:GetWide() - S(8), y - S(1))
		y = y + S(44)
		-- The kinds of lines, then repeats.
		local cx = x
		for _, b in ipairs(kindChips) do
			b:SetPos(cx, y)
			cx = cx + b:GetWide() + S(6)
		end
		collapse:SetPos(cx + S(10), y)
		y = y + S(38)
		pinsRow:SetPos(x, y)
		pinsRow:SetSize(iw, S(32))
		y = y + S(40)
		local entryH = S(40)
		logPanel:SetPos(x, y)
		logPanel:SetSize(iw, h - M - PAD - entryH - S(10) - y)
		entry.box:SetPos(x, h - M - PAD - entryH)
		entry.box:SetSize(iw, entryH)
		grip:SetPos(w - M - S(20), h - M - S(20))
		grip:SetSize(S(18), S(18))
		layoutPop()
		rebuild()
	end
	holder.OnKeyCodePressed = function(self, code) if code == KEY_ESCAPE then self:Remove() end end
	holder.OnMousePressed = function() closeMenu() end

	local nextPull = 0
	holder.Think = function()
		local now = SysTime()
		if now < nextPull then return end
		nextPull = now + PULL
		local hadCatalog = catalogAt
		local added = pull()
		if added > 0 then
			if not state.follow then state.unseen = state.unseen + added end
			rebuild()
		end
		if catalogAt ~= hadCatalog then updatePop() end   -- the list arrived while typing
		if p.mode == "inspect" and p.entry then   -- the value as it is now (after a reset, say)
			p.value, p.default = liveValue(p.entry)
			layoutPop()
		end
	end
	holder:MakePopup()
	holder:SetKeyboardInputEnabled(true)
	UI.fadeIn(holder)
	refreshPins()
	askCatalog()
	rebuild()
	entry:RequestFocus()
end

-- Dividers in the log where a join starts, where you're in game, and where you
-- leave, so a session's lines are easy to find (kept while the window is shut
-- too: the lines are read up to now first, so a divider lands in its place).
local wasLoading, wasInGame = false, false
local function mark(text)
	pull()
	lines[#lines + 1] = { id = -2, t = os.time() * 1000, s = 0, c = -1, x = text, marker = true }
end
hook.Add("Think", "MeowyClientPack.ConsoleMarkers", function()
	local loading, ingame = IsInLoading(), IsInGame()
	if loading ~= wasLoading or ingame ~= wasInGame then
		local address = meowy.ServerNow and meowy.ServerNow() or nil
		local where = isstring(address) and address or "a server"
		if loading and not wasLoading then mark("Joining " .. where)
		elseif ingame and not wasInGame then mark("In game on " .. where)
		elseif wasInGame and not ingame and not loading then mark("Left the server") end
		wasLoading, wasInGame = loading, ingame
	end
end)

-- The console key (` unless rebound) opens this window instead of the game's
-- console: the moment the game's console shows, it's hidden again and this
-- window opens, or closes if it's open, so the key toggles it. Off, the key
-- opens the game's console as before (and meowy_console still opens this).
local keySetting = meowy.Setting and meowy.Setting("meowy_console_key", "1",
	"Meowy Client Pack: the console key opens the Meowy console (0 = Garry's Mod's console)")
local stockShown = false
hook.Add("Think", "MeowyClientPack.ConsoleKey", function()
	if not (keySetting and keySetting:GetBool() and gui and gui.IsConsoleVisible and meowy.ConsoleRun) then return end
	local shown = gui.IsConsoleVisible()
	if shown and not stockShown then
		meowy.ConsoleRun("hideconsole")
		if IsValid(holder) then holder:Remove() else open() end
	end
	stockShown = shown
end)

concommand.Add("meowy_console", function() open() end, nil,
	"Meowy Client Pack: the console, searchable, with warnings and errors only and repeats collapsed")
concommand.Add("meowy_console_status", function()
	log("console: %s", meowy.ConsoleState and meowy.ConsoleState() or "unavailable")
end, nil, "Meowy Client Pack: what the console capture is doing")
