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
local ERROR = Color(238, 104, 112)
local lines, lastId = {}, 0
local history, recallAt = {}, 0
local holder

local function pull()
	local got, last = meowy.ConsoleRead(lastId, READ_MAX)
	local added = 0
	if istable(got) then
		for _, l in ipairs(got) do
			lines[#lines + 1] = l
			added = added + 1
		end
	end
	if isnumber(last) then lastId = last end
	ConsoleLogic.trim(lines, ConsoleLogic.MAX_KEEP)
	return added
end

local function run(line)
	if not isstring(line) or line:match("^%s*$") then return end
	if history[#history] ~= line then history[#history + 1] = line end
	while #history > 100 do table.remove(history, 1) end
	recallAt = 0
	-- Echoed like the game's console does, so the answer reads in context.
	lines[#lines + 1] = { id = -1, t = os.time() * 1000, s = 0, c = 0x9A9EA8, x = "] " .. line }
	if meowy.ConsoleRun and meowy.ConsoleRun(line) then return end
	for _, words in ipairs(ConsoleLogic.commands(line)) do RunConsoleCommand(unpack(words)) end
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
	local state = { query = "", badOnly = false, collapse = true, view = {}, offset = 0, follow = true, note = nil, noteUntil = 0 }
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

	-- Toolbar: search, the two filters, Copy and Clear.
	local search = field(holder, "Search")
	search:SetUpdateOnType(true)
	local rebuild
	search.OnValueChange = function(_, v) state.query = v or "" rebuild() end
	local bad = pill(holder, "Warnings and errors", false, function(v) state.badOnly = v rebuild() end)
	local collapse = pill(holder, "Collapse repeats", true, function(v) state.collapse = v rebuild() end)
	local copy = UI.Button(holder, "Copy", "secondary", function()
		SetClipboardText(ConsoleLogic.copyText(state.view))
		state.note, state.noteUntil = "Copied " .. math.min(#state.view, ConsoleLogic.MAX_COPY) .. " lines", SysTime() + 2
	end)
	local clear = UI.Button(holder, "Clear", "ghost", function()
		for i = #lines, 1, -1 do lines[i] = nil end
		rebuild()
	end)

	-- The log: a card, monospaced lines, a slim scrollbar.
	local logPanel = vgui.Create("DPanel", holder)
	local entry = field(holder, "Type a command and press Enter    (Up and Down: what you typed before)", S(30), "\226\128\186")
	entry:SetFont("MeowyPack.Mono")

	local function maxOffset() return math.max(0, #state.view * lh - (logPanel:GetTall() - S(12))) end
	rebuild = function()
		state.view = ConsoleLogic.view(lines, state.query, state.badOnly, state.collapse)
		if state.follow then state.offset = maxOffset() else state.offset = math.min(state.offset, maxOffset()) end
	end

	logPanel.Paint = function(self, w, h)
		UI.box(S(10), 0, 0, w, h, C.surface, C.border)
		local top = S(6)
		local first = math.floor(state.offset / lh) + 1
		local shift = state.offset % lh
		local tw = UI.textW("00:00:00", "MeowyPack.Mono") + S(14)
		local mx, my = self:CursorPos()
		local hoverRow = self:IsHovered() and (math.floor((my - top + shift) / lh) + first) or nil
		for i = first, math.min(#state.view, first + math.ceil(h / lh) + 1) do
			local row = state.view[i]
			local y = top + (i - first) * lh - shift
			if i == hoverRow then draw.RoundedBox(S(4), S(4), y, w - S(18), lh, Color(255, 255, 255, 6)) end
			draw.SimpleText(ConsoleLogic.clock(row.line.t), "MeowyPack.Mono", S(12), y + lh / 2, C.dim, UI.LEFT, UI.CENTER)
			local s = row.line.s or 0
			local col = s >= 2 and ERROR or s == 1 and C.amber or Color(ConsoleLogic.color(row.line))
			draw.SimpleText(row.line.x or "", "MeowyPack.Mono", S(12) + tw, y + lh / 2, col, UI.LEFT, UI.CENTER)
			if row.count > 1 then
				local label = "x" .. row.count
				local cwid = UI.textW(label, "MeowyPack.Small") + S(14)
				draw.RoundedBox(S(8), w - cwid - S(22), y + S(2), cwid, lh - S(4), C.raised)
				draw.SimpleText(label, "MeowyPack.Small", w - cwid / 2 - S(22), y + lh / 2, C.body, UI.CENTER, UI.CENTER)
			end
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
	logPanel.OnMousePressed = function(self, code)
		if code ~= MOUSE_LEFT then return end
		local mx, my = self:CursorPos()
		if mx >= self:GetWide() - S(16) then
			self.dragging = true
			self:MouseCapture(true)
			return
		end
		-- A click copies that line.
		local i = math.floor((my - S(6) + state.offset % lh) / lh) + math.floor(state.offset / lh) + 1
		local row = state.view[i]
		if row then
			SetClipboardText(row.line.x or "")
			state.note, state.noteUntil = "Line copied", SysTime() + 1.5
		end
	end
	logPanel.OnMouseReleased = function(self) self.dragging = false self:MouseCapture(false) end
	logPanel.Think = function(self)
		if not self.dragging then return end
		if not input.IsMouseDown(MOUSE_LEFT) then self.dragging = false self:MouseCapture(false) return end
		local _, my = self:CursorPos()
		state.offset = math.Clamp((my - S(8)) / math.max(1, self:GetTall() - S(16)) * maxOffset(), 0, maxOffset())
		state.follow = state.offset >= maxOffset() - 1
	end

	-- The command line: history on Up and Down (its prompt mark is the field's).
	entry.OnEnter = function(self)
		run(self:GetValue())
		self:SetText("")
		self:RequestFocus()
		state.follow = true
		rebuild()
	end
	entry.OnKeyCodeTyped = function(self, code)
		if code == KEY_UP or code == KEY_DOWN then
			local text
			recallAt, text = ConsoleLogic.recall(history, recallAt, code == KEY_UP and 1 or -1)
			self:SetText(text)
			self:SetCaretPos(#text)
			return true
		end
		if code == KEY_ESCAPE then holder:Remove() return true end
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
		bad:SetPos(x + search.box:GetWide() + S(12), y + S(3))
		collapse:SetPos(bad.x + bad:GetWide() + S(8), y + S(3))
		clear:SetPos(x + iw - clear:GetWide(), y - S(1))
		copy:SetPos(clear.x - copy:GetWide() - S(8), y - S(1))
		y = y + S(48)
		local entryH = S(40)
		logPanel:SetPos(x, y)
		logPanel:SetSize(iw, h - M - PAD - entryH - S(10) - y)
		entry.box:SetPos(x, h - M - PAD - entryH)
		entry.box:SetSize(iw, entryH)
		grip:SetPos(w - M - S(20), h - M - S(20))
		grip:SetSize(S(18), S(18))
		rebuild()
	end
	holder.OnKeyCodePressed = function(self, code) if code == KEY_ESCAPE then self:Remove() end end

	local nextPull = 0
	holder.Think = function()
		local now = SysTime()
		if now < nextPull then return end
		nextPull = now + PULL
		if pull() > 0 then rebuild() end
	end
	holder:MakePopup()
	holder:SetKeyboardInputEnabled(true)
	UI.fadeIn(holder)
	rebuild()
	entry:RequestFocus()
end

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
