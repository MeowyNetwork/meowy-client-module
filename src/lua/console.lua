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
local READ_MAX = 5000      -- lines per read while it's open (it opens with all of them: ConsoleLogic.MAX_KEEP)
local HISTORY_MAX, PINS_MAX = 200, 24
local CATALOG_AGE = 120    -- seconds before the command list is read again
local INSERTS_MAX = 200    -- the window's own lines (dividers, echoed commands) kept for when it opens again
local OVERLAY_LINES, OVERLAY_SHOW = 8, 20   -- the overlay's lines, and seconds each shows
local ERROR = Color(238, 104, 112)
-- The console's lines are only read into Lua while the window is open: the
-- capture (console_capture.h) keeps the last 50,000 itself, so the window reads
-- them all when it opens and lets them go when it closes, and nothing is read
-- at a join while it's shut.
local lines, lastId = {}, 0
local floorId = 0          -- the window shows the lines after this one (a Clear, or a typed `clear`, moves it)
local inserts, nextInsert = {}, 1   -- the window's own lines by place (ConsoleLogic.placeDue); from nextInsert on, not in `lines` yet
local hidden = {}          -- { from, to }: ids of cvarlist's output, read into the command list, left out when read again
local clears = 0           -- times the window was emptied (an open window starts over)
local holder
local errorGroups, errorsChanged, lastErrorAt = {}, 0, nil   -- Lua errors (OnLuaError, below)
-- The overlay (meowy_console_overlay, below): its mode, read a few times a
-- second, and the newest lines it can show, kept as they're read: [1] warnings
-- and errors, [2] every line. ringId: the newest line they've seen.
local overlayMode, ring, ringId = 0, { {}, {} }, 0
-- The console key opens this window (the hook at the end); typed console
-- commands for the game's console window act on this one while it does.
local keySetting = meowy.Setting and meowy.Setting("meowy_console_key", "1",
	"Meowy Client Pack: the console key opens the Meowy console (0 = Garry's Mod's console)")

-- Emptied (Clear, or a typed `clear` at the line `atId`): opened again, the
-- window starts after it, without the dividers and echoes it had.
local function clearLines(atId)
	for i = #lines, 1, -1 do lines[i] = nil end
	clears = clears + 1
	floorId = math.max(floorId, atId or lastId)
	local left = {}
	for i = nextInsert, #inserts do left[#left + 1] = inserts[i] end
	inserts, nextInsert = left, 1
end

-- A line of the window's own, at its place after line `afterId`.
local function addInsert(afterId, line)
	local dropped = ConsoleLogic.addInsert(inserts, { afterId = afterId, line = line }, INSERTS_MAX, nextInsert)
	nextInsert = math.max(1, nextInsert - dropped)
end

-- The overlay's lines, kept as they arrive (only while it's on).
local function ringPush(r, l)
	r[#r + 1] = l
	if #r > OVERLAY_LINES then table.remove(r, 1) end
end
local function ringAdd(l)
	ringPush(ring[2], l)
	local k = ConsoleLogic.kind(l)
	if k == "error" or k == "warning" then ringPush(ring[1], l) end
end

-- The game's con_filter settings, as ConsoleLogic.conFilter reads them.
local function conFilterNow()
	if not GetConVar then return nil end
	local e, t, o = GetConVar("con_filter_enable"), GetConVar("con_filter_text"), GetConVar("con_filter_text_out")
	return e and ConsoleLogic.conFilter(e:GetString(), t and t:GetString(), o and o:GetString()) or nil
end

-- What you typed (kept between starts) and the pinned commands.
local function loadList(kind, max)
	return ConsoleLogic.parseList(meowy.ConsoleFileRead and meowy.ConsoleFileRead(kind) or "", max)
end
local function saveList(kind, list, max)
	if meowy.ConsoleFileWrite then meowy.ConsoleFileWrite(kind, ConsoleLogic.formatList(list, max)) end
end
-- Lines typed or pinned with a secret in them (passwords, rcon, a bind or
-- alias holding one) are never kept; older versions kept them, so they're
-- dropped from what was saved.
local history, recallAt = loadList("history", HISTORY_MAX), 0
local pins = loadList("pins", PINS_MAX)
do
	local clean = ConsoleLogic.withoutSecrets(history)
	if #clean ~= #history then history = clean saveList("history", history, HISTORY_MAX) end
	local cleanPins = ConsoleLogic.withoutSecrets(pins)
	if #cleanPins ~= #pins then pins = cleanPins saveList("pins", pins, PINS_MAX) end
end

-- The pack's marks in the console's text (a `clear`, the command list's start
-- and end) carry a part made at each start, so a server printing the plain
-- words can't empty the window or start a list; each also counts only while
-- the pack is waiting for it.
local NONCE = string.format("%s%x%x", string.match(tostring({}), "(%x+)$") or "", os.time(), math.floor((SysTime() % 2048) * 1048576))
local CLEAR = "__meowy_console_clear_" .. NONCE .. "__"
local clearsAsked = 0

-- Every command and setting, with its value, flags and help: the engine's own
-- `cvarlist`, run between two markers; its lines are read into the catalog and
-- left out of the window.
local BEGIN, FINISH = "__meowy_cvars_begin_" .. NONCE .. "__", "__meowy_cvars_end_" .. NONCE .. "__"
local catalog, catalogAt, collecting, asked, askedAt, collectFrom = {}, nil, nil, false, 0, 0
local function askCatalog()
	if not meowy.ConsoleRun then return end
	-- Asked and not read yet (the window was shut before its end came back,
	-- and that's long gone from the capture): asked again after a while.
	if asked and SysTime() - askedAt < 30 then return end
	if catalogAt and SysTime() - catalogAt < CATALOG_AGE then return end
	asked, askedAt, collecting = true, SysTime(), nil
	meowy.ConsoleRun("echo " .. BEGIN)
	meowy.ConsoleRun("cvarlist")
	meowy.ConsoleRun("echo " .. FINISH)
end

-- The lines since the last read (at most `max`, READ_MAX unless given) into
-- the window's list, with its own lines placed among them. Only while the
-- window is open. Returns how many were added.
local function pull(max)
	local got, last = meowy.ConsoleRead(lastId, max or READ_MAX)
	local added = 0
	local feedRing = overlayMode >= 1
	local hi = 1   -- hidden[hi]: the first range not behind this line
	if istable(got) then
		for _, l in ipairs(got) do
			local id = l.id or 0
			local before = #lines
			nextInsert = ConsoleLogic.placeDue(inserts, nextInsert, id, lines)
			added = added + #lines - before
			while hidden[hi] and hidden[hi].to < id do hi = hi + 1 end
			local x = string.match(l.x or "", "^%s*(.-)%s*$")   -- echo adds a space at the end
			if hidden[hi] and id >= hidden[hi].from then
				-- cvarlist's output, read into the command list before: left out again
			elseif x == CLEAR and clearsAsked > 0 then   -- a typed `clear` (run, below) got here
				clearsAsked = clearsAsked - 1
				clearLines(id)
				added = 0
			elseif x == BEGIN and asked and not collecting then
				collecting, collectFrom = {}, id
			elseif collecting and x == FINISH then
				hidden[#hidden + 1] = { from = collectFrom, to = id }
				if #hidden > 64 then table.remove(hidden, 1) end
				if #collecting > 0 then
					-- The pack's own settings are hidden from cvarlist (settings.lua): added here.
					local have = {}
					for _, e in ipairs(collecting) do have[string.lower(e.name)] = true end
					for _, s in ipairs(meowy.SettingsList and meowy.SettingsList() or {}) do
						if not have[s.name] then
							collecting[#collecting + 1] = { name = s.name, value = s.value, flags = { "pack" }, help = s.help, cmd = false }
						end
					end
					catalog, catalogAt = collecting, SysTime()
				end
				collecting, asked = nil, false
			elseif collecting then
				local e = ConsoleLogic.parseCvar(x)
				if e then collecting[#collecting + 1] = e end
			else
				lines[#lines + 1] = l
				added = added + 1
				if feedRing and id > ringId then ringAdd(l) end
			end
		end
	end
	if isnumber(last) then
		local before = #lines
		nextInsert = ConsoleLogic.placeDue(inserts, nextInsert, last + 1, lines)
		added = added + #lines - before
		lastId = last
		if last > ringId then ringId = last end
	end
	ConsoleLogic.trim(lines, ConsoleLogic.MAX_KEEP, ConsoleLogic.TRIM_SLACK)
	return added
end

-- Shut: the lines go (the capture still has them for next time). The
-- dividers and echoes stay in `inserts`, to be put back in place.
local function dropLines()
	lines, lastId, nextInsert = {}, floorId, 1
	collecting = nil   -- a command list half read is read again from its start
end

-- A typed command, echoed in the window after the lines read so far (at
-- once: it goes where the next read would put it).
local function echo(text)
	addInsert(lastId, { id = -1, t = os.time() * 1000, s = 0, c = 0x9A9EA8, x = "] " .. text })
	nextInsert = ConsoleLogic.placeDue(inserts, nextInsert, lastId + 1, lines)
end
local function run(line)
	if not isstring(line) or line:match("^%s*$") then return end
	-- A password or rcon line isn't kept (on disk, or for Up and Ctrl+R).
	if history[#history] ~= line and not ConsoleLogic.secret(line) then
		history[#history + 1] = line
		while #history > HISTORY_MAX do table.remove(history, 1) end
		saveList("history", history, HISTORY_MAX)
	end
	recallAt = 0
	-- Echoed like the game's console does, so the answer reads in context
	-- (without a password: the window can be on stream or saved).
	echo(ConsoleLogic.masked(line))
	local send, close = ConsoleLogic.prepare(line, keySetting and keySetting:GetBool(), CLEAR)
	if close and IsValid(holder) then holder:Remove() end
	if send == "" then return end
	if meowy.ConsoleRun and meowy.ConsoleRun(send) then
		local _, marks = string.gsub(send, CLEAR, "")
		clearsAsked = clearsAsked + marks
		return
	end
	-- Without the engine's own command line: each command through
	-- RunConsoleCommand, and `clear`'s mark empties the window here.
	for _, words in ipairs(ConsoleLogic.commands(send)) do
		if words[1] == "echo" and words[2] == CLEAR then clearLines() else RunConsoleCommand(unpack(words)) end
	end
end

local function pin(line)
	if not isstring(line) or line:match("^%s*$") or ConsoleLogic.secret(line) then return end
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
	-- Every line the capture still has (after a Clear), read once now; then
	-- only the new ones, a few times a second.
	dropLines()
	pull(ConsoleLogic.MAX_KEEP)
	fonts()
	local M, R, PAD = S(24), S(14), S(20)   -- shadow margin, corner radius, padding
	local state = { query = "", kinds = {}, counts = {}, find = false, pattern = false, collapse = true, view = {},
		offset = 0, follow = true, note = nil, noteUntil = 0, unseen = 0, cur = nil, selA = nil, selB = nil,
		clears = clears, filter = conFilterNow() }
	state.filterKey = state.filter and (state.filter.text .. "\0" .. state.filter.out) or ""
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

	-- Header: the two tabs (Console, Errors), how many lines (or a short note),
	-- live numbers on the right (frame rate; in game ping, loss and the server),
	-- close.
	state.tab, state.fps, state.frames, state.framesAt = "console", nil, 0, SysTime()
	local header = vgui.Create("DPanel", holder)
	local tabs = {}
	local setTab
	for i, t in ipairs({ { "console", "Console" }, { "errors", "Errors" } }) do
		local b = vgui.Create("DButton", header)
		b:SetText("")
		b.meowyTab = t[1]
		b.Paint = function(self, w, h)
			local on, hv = state.tab == t[1], UI.hover(self, self:IsHovered())
			if on then draw.RoundedBox(S(8), 0, 0, w, h, C.raised) elseif hv > 0.01 then draw.RoundedBox(S(8), 0, 0, w, h, Color(255, 255, 255, 10 * hv)) end
			local label = t[2]
			draw.SimpleText(label, "MeowyPack.Heading", S(12), h / 2, on and C.text or C.body, UI.LEFT, UI.CENTER)
			if t[1] == "errors" then
				local n = ConsoleLogic.errorCount(errorGroups)
				if n > 0 then
					local lw = UI.textW(label, "MeowyPack.Heading")
					local cw = UI.textW(tostring(n), "MeowyPack.Small") + S(12)
					draw.RoundedBox(S(8), S(12) + lw + S(8), h / 2 - S(9), cw, S(18), ERROR)
					draw.SimpleText(tostring(n), "MeowyPack.Small", S(12) + lw + S(8) + cw / 2, h / 2, C.white, UI.CENTER, UI.CENTER)
				end
			end
			return true
		end
		b.DoClick = function() setTab(t[1]) end
		local extra = t[1] == "errors" and S(44) or 0
		b:SetSize(UI.textW(t[2], "MeowyPack.Heading") + S(24) + extra, S(34))
		tabs[i] = b
	end
	header.PerformLayout = function(self, w, h)
		local x = 0
		for _, b in ipairs(tabs) do
			b:SetPos(x, math.floor((h - b:GetTall()) / 2))
			x = x + b:GetWide() + S(4)
		end
		self.textX = x + S(10)
	end
	header.Paint = function(self, w, h)
		local note = (state.note and SysTime() < state.noteUntil) and state.note
			or (state.tab == "errors" and (function()
				local n = ConsoleLogic.errorCount(errorGroups)
				return (n == 1 and "1 Lua error" or (n .. " Lua errors")) .. " since the game started"
			end)())
			or ((#state.view == #lines and (#lines .. (#lines == 1 and " line" or " lines"))
					or (#state.view .. " of " .. #lines .. (#lines == 1 and " line" or " lines")))
				.. (state.filter and "  \194\183  con_filter on" or ""))
		draw.SimpleText(note, "MeowyPack.Small", self.textX or S(200), h / 2 + S(1), C.dim, UI.LEFT, UI.CENTER)
		-- Live numbers.
		local facts = {}
		if state.fps then facts[#facts + 1] = state.fps .. " fps" end
		if IsInGame() and meowy.ServerNow then
			local address, rtt, loss = meowy.ServerNow()
			if isnumber(rtt) then facts[#facts + 1] = math.floor(rtt + 0.5) .. " ms" end
			if isnumber(loss) then facts[#facts + 1] = math.floor(loss * 100 + 0.5) .. "% loss" end
			if isstring(address) then facts[#facts + 1] = address end
		end
		draw.SimpleText(table.concat(facts, "  \194\183  "), "MeowyPack.Small", w - S(12), h / 2 + S(1), C.dim, UI.RIGHT, UI.CENTER)
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
	local clear = UI.Button(holder, "Clear", "ghost", function() clearLines() rebuild() end)
	-- Save: the lines shown, paths masked, to a file of its own (logs\); its
	-- full path goes on the clipboard.
	local save = UI.Button(holder, "Save", "ghost", function()
		local path = meowy.ConsoleSaveLog and meowy.ConsoleSaveLog(ConsoleLogic.copyText(state.view, ConsoleLogic.MAX_KEEP))
		if isstring(path) then
			SetClipboardText(path)
			state.note, state.noteUntil = "Saved " .. #state.view .. " lines (path copied): MeowyClientPack\\logs\\"
				.. (path:match("[^\\/]+$") or path), SysTime() + 4
		else
			state.note, state.noteUntil = "Couldn't save the log", SysTime() + 3
		end
	end)

	-- The Errors tab: Lua errors since the game started, by addon, newest first.
	local errorsPanel = vgui.Create("DScrollPanel", holder)
	errorsPanel:SetVisible(false)
	errorsPanel.Paint = function(self, w, h) UI.box(S(10), 0, 0, w, h, C.surface, C.border) end
	local bar = errorsPanel.GetVBar and errorsPanel:GetVBar()
	if bar then
		bar:SetWide(S(10))
		bar.Paint = function() end
		if bar.btnUp then bar.btnUp.Paint = function() end end
		if bar.btnDown then bar.btnDown.Paint = function() end end
		if bar.btnGrip then bar.btnGrip.Paint = function(self, w, h) draw.RoundedBox(S(3), S(3), 0, S(5), h, C.raised) end end
	end
	local updatedCache, builtFor = {}, -1
	local gameLine = "Garry's Mod " .. tostring(VERSIONSTR or "?") .. " (" .. tostring(BRANCH or "?") .. ")"
		.. ((meowy.Version and (", Meowy Client Pack " .. tostring(meowy.Version()))) or "")
	local function buildErrors()
		builtFor = errorsChanged
		if errorsPanel.Clear then errorsPanel:Clear() end
		local list = ConsoleLogic.errorList(errorGroups)
		if #list == 0 then
			local empty = errorsPanel:Add("DPanel")
			empty:Dock(TOP)
			empty:SetTall(S(120))
			empty.Paint = function(self, w, h)
				draw.SimpleText("No Lua errors since the game started", "MeowyPack.Body", w / 2, h / 2, C.dim, UI.CENTER, UI.CENTER)
			end
			return
		end
		for _, item in ipairs(list) do
			local g = item.group
			if g.id and not updatedCache[g.id] and steamworks and steamworks.FileInfo then
				updatedCache[g.id] = "?"
				steamworks.FileInfo(g.id, function(info)
					if istable(info) and tonumber(info.updated) then updatedCache[g.id] = os.date("%Y-%m-%d", tonumber(info.updated)) end
				end)
			end
			local head = errorsPanel:Add("DPanel")
			head:Dock(TOP)
			head:DockMargin(S(12), S(10), S(12), 0)
			head:SetTall(S(40))
			head.meowyErrorGroup = g.key
			head.Paint = function(self, w, h)
				draw.SimpleText(g.title or "An unknown addon", "MeowyPack.Heading", 0, S(12), C.text, UI.LEFT, UI.CENTER)
				local sub = (g.id and ("Workshop " .. g.id .. ((updatedCache[g.id] and updatedCache[g.id] ~= "?") and (", updated " .. updatedCache[g.id]) or "") .. "  \194\183  ") or "")
					.. g.count .. (g.count == 1 and " error" or " errors")
				draw.SimpleText(sub, "MeowyPack.Small", 0, S(31), C.dim, UI.LEFT, UI.CENTER)
			end
			if g.id then
				local open = UI.Button(head, "Open on Workshop", "ghost", function()
					gui.OpenURL("https://steamcommunity.com/sharedfiles/filedetails/?id=" .. g.id)
				end)
				open:Dock(RIGHT)
				if not IsInGame() and steamworks and steamworks.SetShouldMountAddon then
					local off = UI.Button(head, "Disable addon", "ghost", function()
						UI.Prompt({ title = "Disable " .. (g.title or g.id) .. "?",
							body = "It won't load at the next start or join. Turn it back on in Addons.",
							buttons = { { "Disable", "primary", function()
								steamworks.SetShouldMountAddon(g.id, false)
								steamworks.ApplyAddons()
								state.note, state.noteUntil = (g.title or g.id) .. " disabled", SysTime() + 3
							end }, { "Cancel", "secondary" } }, closable = true })
					end)
					off:Dock(RIGHT)
					off:DockMargin(0, 0, S(6), 0)
					off.meowyDisable = g.id
				end
			end
			for _, e in ipairs(item.errors) do
				local stack = ConsoleLogic.stackLines(e.stack, 6)
				local card = errorsPanel:Add("DPanel")
				card:Dock(TOP)
				card:DockMargin(S(12), S(6), S(12), 0)
				card:SetTall(S(20) + lh * (2 + #stack) + S(8))
				card.Paint = function(self, w, h)
					UI.box(S(8), 0, 0, w, h, C.window, C.border)
					local msg = e.message:gsub("[\r\n]+", "  ")
					if #msg > 160 then msg = msg:sub(1, 159) .. "\226\128\166" end
					draw.SimpleText(msg, "MeowyPack.Mono", S(12), S(10) + lh / 2, ERROR, UI.LEFT, UI.CENTER)
					local meta = "x" .. e.count .. "  \194\183  first " .. ConsoleLogic.clock(e.first * 1000) .. ", last " .. ConsoleLogic.clock(e.last * 1000)
						.. "  \194\183  " .. tostring(e.realm or "?") .. "  \194\183  " .. (e.server and ("on " .. e.server) or "at the main menu")
					draw.SimpleText(meta, "MeowyPack.Small", S(12), S(10) + lh * 1.5, C.dim, UI.LEFT, UI.CENTER)
					for i, s in ipairs(stack) do
						draw.SimpleText(s, "MeowyPack.Mono", S(24), S(10) + lh * (1.5 + i), C.body, UI.LEFT, UI.CENTER)
					end
				end
				local rep = UI.Button(card, "Copy report", "secondary", function()
					local text = ConsoleLogic.report(g, e, { updated = updatedCache[g.id] ~= "?" and updatedCache[g.id] or nil, game = gameLine })
					SetClipboardText(meowy.MaskPaths and meowy.MaskPaths(text) or text)
					state.note, state.noteUntil = "Report copied (paths hidden)", SysTime() + 2
				end)
				rep.meowyReport = e.message
				card.PerformLayout = function(self, w, h) rep:SetPos(w - rep:GetWide() - S(10), S(8)) end
			end
		end
	end

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
	-- The default as one argument, never as a console line: a server's Lua can
	-- give its convar any default (layoutPop offers the button only for a plain one).
	local reset = UI.Button(pop, "Reset to default", "secondary", function()
		if not (p.entry and ConsoleLogic.plainValue(p.default)) then return end
		echo(p.entry.name .. ' "' .. p.default .. '"')
		RunConsoleCommand(p.entry.name, p.default)
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
			and ConsoleLogic.plainValue(p.default)
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
			kinds = state.kinds, collapse = state.collapse, flood = state.collapse, conFilter = state.filter })
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
				local col = (s >= 2 or ConsoleLogic.kind(row.line) == "error") and ERROR or s == 1 and C.amber or Color(ConsoleLogic.color(row.line))
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

	-- The tabs: the console's own rows, or the Errors list in their place.
	setTab = function(tab)
		state.tab = tab
		local console = tab == "console"
		for _, b in ipairs(kindChips) do b:SetVisible(console) end
		for _, panel in ipairs({ collapse, findPill, patternPill, search.box, copy, clear, save, pinsRow, logPanel }) do
			panel:SetVisible(console)
		end
		errorsPanel:SetVisible(not console)
		if not console then
			buildErrors()
			p.mode = nil
			layoutPop()
		end
		holder:InvalidateLayout()
	end

	holder.PerformLayout = function(self, w, h)
		local x, y, iw = M + PAD, M + S(10), w - M * 2 - PAD * 2
		header:SetPos(x, y)
		header:SetSize(iw - S(40), S(46))
		close:SetPos(M + (w - M * 2) - PAD - close:GetWide() + S(6), y + S(8))
		y = y + S(52)
		local topY = y
		search.box:SetPos(x, y)
		search.box:SetSize(math.min(S(320), iw * 0.35), S(36))
		findPill:SetPos(x + search.box:GetWide() + S(12), y + S(3))
		patternPill:SetPos((findPill.x or 0) + findPill:GetWide() + S(8), y + S(3))
		clear:SetPos(x + iw - clear:GetWide(), y - S(1))
		copy:SetPos((clear.x or 0) - copy:GetWide() - S(8), y - S(1))
		save:SetPos((copy.x or 0) - save:GetWide() - S(4), y - S(1))
		errorsPanel:SetPos(x, topY)
		errorsPanel:SetSize(iw, h - M - PAD - S(40) - S(10) - topY)
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
	-- Shut: its lines go (read again from the capture when it opens).
	holder.OnRemove = function(self)
		if holder == self or not IsValid(holder) then dropLines() end
	end

	local nextPull = 0
	holder.Think = function()
		local now = SysTime()
		-- The frame rate, counted here (the menu state has no FrameTime).
		state.frames = state.frames + 1
		if now - state.framesAt >= 1 then
			state.fps = math.floor(state.frames / (now - state.framesAt) + 0.5)
			state.frames, state.framesAt = 0, now
		end
		if now < nextPull then return end
		nextPull = now + PULL
		if state.tab == "errors" and builtFor ~= errorsChanged then buildErrors() end
		local hadCatalog = catalogAt
		local added = pull()
		local filter = conFilterNow()
		local filterKey = filter and (filter.text .. "\0" .. filter.out) or ""
		if clears ~= state.clears then   -- emptied (Clear, or `clear` typed): back to the bottom
			state.clears, state.follow, state.unseen, state.cur, state.selA, state.selB = clears, true, 0, nil, nil, nil
			rebuild()
		elseif filterKey ~= state.filterKey then
			state.filterKey, state.filter = filterKey, filter
			rebuild()
		elseif added > 0 then
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
-- leave, so a session's lines are easy to find. Each is kept with the newest
-- line's id when it was made (one line read, not the join's thousands) and
-- goes in its place whenever the window reads past it, also if it's opened
-- much later.
local wasLoading, wasInGame = false, false
local function mark(text)
	local _, newest = meowy.ConsoleRead(0, 1)
	addInsert(isnumber(newest) and newest or lastId, { id = -2, t = os.time() * 1000, s = 0, c = -1, x = text, marker = true })
end
local joinSeenAt = -math.huge   -- SysTime() of the last frame a join was running
hook.Add("Think", "MeowyClientPack.ConsoleMarkers", function()
	local loading, ingame = IsInLoading(), IsInGame()
	if loading or ingame then joinSeenAt = SysTime() end
	if loading ~= wasLoading or ingame ~= wasInGame then
		local address = meowy.ServerNow and meowy.ServerNow() or nil
		local where = isstring(address) and address or "a server"
		if loading and not wasLoading then mark("Joining " .. where)
		elseif ingame and not wasInGame then mark("In game on " .. where)
		elseif wasInGame and not ingame and not loading then mark("Left the server") end
		wasLoading, wasInGame = loading, ingame
	end
end)

-- Lua errors, from the same hook the game's own notices use (OnLuaError: the
-- message, realm, stack, and the addon it came from), grouped by addon for the
-- Errors tab; with the server you were on.
hook.Add("OnLuaError", "MeowyClientPack.ConsoleErrors", function(message, realm, stack, title, id)
	local address = IsInGame() and meowy.ServerNow and meowy.ServerNow() or nil
	ConsoleLogic.recordError(errorGroups, message, realm, stack, title, id, os.time(), isstring(address) and address or nil)
	errorsChanged, lastErrorAt = errorsChanged + 1, SysTime()
end)

-- The badge: in place of the game's own error notices (its DrawOverlay hook
-- MenuDrawLuaErrors, put back when this is off), a small pill: how many errors,
-- from which addon, and which key shows them. The game's two other notices it
-- drew (a serious problem; the pause menu blocked) show as pills too.
local badgeSetting = meowy.Setting and meowy.Setting("meowy_console_badge", "1",
	"Meowy Client Pack: a small error badge in place of Garry's Mod's error notices (0 = Garry's Mod's)")
local notices = {}
hook.Add("OnProblemReceived", "MeowyClientPack.ConsoleNotices", function(problem)
	if istable(problem) and problem.severity == 2 and isstring(problem.text) then
		notices.problem = { text = string.match(problem.text, "([^\n]+)") or problem.text, at = SysTime() }
	end
end)
hook.Add("OnPauseMenuBlockedTooManyTimes", "MeowyClientPack.ConsoleNotices", function()
	notices.blocked = { text = "Something keeps blocking the pause menu: Shift+Esc opens it anyway", at = SysTime() }
end)
local stockNotices
local function badgeOn() return badgeSetting ~= nil and badgeSetting:GetBool() end
local BADGE_SHOW = 10   -- seconds a pill stays after its last error
local function pillAt(x, y, text, tone, age)
	local alpha = math.Clamp((BADGE_SHOW - age) / 1.5, 0, 1)
	if alpha <= 0 then return y end
	surface.SetFont("MeowyPack.Small")
	local tw = surface.GetTextSize(text)
	local h = S(28)
	draw.RoundedBox(h / 2, x, y, tw + S(34), h, Color(20, 21, 24, 225 * alpha))
	draw.RoundedBox(S(4), x + S(10), y + h / 2 - S(4), S(8), S(8), Color(tone.r, tone.g, tone.b, 255 * alpha))
	draw.SimpleText(text, "MeowyPack.Small", x + S(24), y + h / 2, Color(232, 233, 236, 255 * alpha), UI.LEFT, UI.CENTER)
	return y + h + S(6)
end

-- Overlay mode: while playing, the newest console lines in the top-left corner,
-- see-through and click-through, fading after a while (meowy_console_overlay:
-- 0 off, 1 warnings and errors, 2 everything).
local overlaySetting = meowy.Setting and meowy.Setting("meowy_console_overlay", "0",
	"Meowy Client Pack: show the newest console lines over the game (0 off, 1 warnings and errors, 2 everything)")
local OVERLAY_READ = OVERLAY_LINES * 16   -- lines per read with the window shut
local OVERLAY_BOX = Color(10, 10, 12, 150)
local overlayPull, settingsAt, badge = 0, 0, false
local drawHud                  -- the cl_drawhud convar, once found
local badgeFor, badgeLabel     -- the badge's text, and the errorsChanged it was made for

-- The overlay's lines since it last looked: through the window's own read
-- while that's open, otherwise a small read of the newest lines only.
local function overlayRead()
	if IsValid(holder) then pull() return end
	local got, last = meowy.ConsoleRead(ringId, OVERLAY_READ)
	if istable(got) then
		for _, l in ipairs(got) do if (l.id or 0) > ringId then ringAdd(l) end end
	end
	if isnumber(last) and last > ringId then ringId = last end
end

hook.Add("DrawOverlay", "MeowyClientPack.ConsoleOverlay", function()
	local now = SysTime()
	-- The two settings, a few times a second (each read asks the engine).
	if now >= settingsAt then
		settingsAt = now + PULL
		badge = badgeOn()
		overlayMode = overlaySetting and tonumber(overlaySetting:GetString()) or 0
	end
	-- The game's notices off while the badge is on (and back when it's off).
	local hooks = hook.GetTable and hook.GetTable().DrawOverlay
	if hooks then
		if badge and hooks.MenuDrawLuaErrors then
			stockNotices = hooks.MenuDrawLuaErrors
			hook.Remove("DrawOverlay", "MenuDrawLuaErrors")
		elseif not badge and stockNotices and not hooks.MenuDrawLuaErrors then
			hook.Add("DrawOverlay", "MenuDrawLuaErrors", stockNotices)
		end
	end
	if not drawHud and GetConVar then drawHud = GetConVar("cl_drawhud") end
	if drawHud and not drawHud:GetBool() then return end
	fonts()
	local y = S(32)
	if badge then
		-- The text only while its pill shows, and made again only after a new error.
		if lastErrorAt and now - lastErrorAt < BADGE_SHOW then
			if badgeFor ~= errorsChanged then
				badgeFor = errorsChanged
				local text = ConsoleLogic.badgeText(errorGroups)
				badgeLabel = text and (text .. "  \194\183  ` shows them") or nil
			end
			if badgeLabel then y = pillAt(S(32), y, badgeLabel, ERROR, now - lastErrorAt) end
		end
		for _, n in pairs(notices) do
			if now - n.at < BADGE_SHOW then y = pillAt(S(32), y, n.text, C.amber, now - n.at) end
		end
	end
	local mode = overlayMode
	-- Not while the window is on screen (in game it shows only with the game's menu).
	local windowShown = IsValid(holder) and holder:IsVisible() and gui and gui.IsGameUIVisible and gui.IsGameUIVisible()
	if mode == 0 or not IsInGame() or windowShown then return end
	if now > overlayPull then overlayPull = now + PULL overlayRead() end
	-- The lines kept for this mode, the ones from the last OVERLAY_SHOW seconds.
	local r, nowMs = ring[mode >= 2 and 2 or 1], os.time() * 1000
	local first = #r + 1
	while first > 1 and nowMs - (r[first - 1].t or 0) <= OVERLAY_SHOW * 1000 do first = first - 1 end
	if first > #r then return end
	local lh = S(18)
	local w = S(640)
	draw.RoundedBox(S(8), S(24), y, w, (#r - first + 1) * lh + S(12), OVERLAY_BOX)
	for i = first, #r do
		local l = r[i]
		if not l.oc then   -- its colour and its text cut short, once per line
			local cr, cg, cb = ConsoleLogic.color(l)
			if ConsoleLogic.kind(l) == "error" then cr, cg, cb = ERROR.r, ERROR.g, ERROR.b end
			local text = l.x or ""
			if #text > 110 then text = text:sub(1, 109) .. "\226\128\166" end
			l.oc, l.ox = Color(cr, cg, cb), text
		end
		local age = (nowMs - (l.t or 0)) / 1000
		l.oc.a = 255 * math.Clamp((OVERLAY_SHOW - age) / 3, 0.15, 1)
		draw.SimpleText(l.ox, "MeowyPack.Mono", S(34), y + S(6) + (i - first) * lh + lh / 2, l.oc, UI.LEFT, UI.CENTER)
	end
end)

-- The console key (` unless rebound) opens this window instead of the game's
-- console: the moment the game's console shows, it's hidden again and this
-- window opens, or closes if it's open, so the key toggles it. Off, the key
-- opens the game's console as before (and meowy_console still opens this).
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

-- The command opens the window only at the main menu with no join running or
-- just ended: a server's Lua can run any menu command (RunConsoleCommand), even
-- right after a `disconnect` it queued first. In game the console key opens it.
concommand.Add("meowy_console", function()
	if IsInLoading() or IsInGame() or SysTime() - joinSeenAt < 2 then
		log("console: meowy_console only opens it at the main menu; in game, the console key does")
		return
	end
	open()
end, nil, "Meowy Client Pack: the console, searchable, with warnings and errors only and repeats collapsed")
concommand.Add("meowy_console_status", function()
	log("console: %s", meowy.ConsoleState and meowy.ConsoleState() or "unavailable")
end, nil, "Meowy Client Pack: what the console capture is doing")
