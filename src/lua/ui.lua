-- The pack's menu UI kit (menu state). module.cpp puts this first in the
-- resilience chunk, as the local `UI` that doctor.lua and resilience.lua use.
--
-- One look for every pack screen, matching Meowy's in-game UI: a dark window
-- with a soft shadow, 14px corners, one pink accent (#c2407a) used sparingly
-- (primary buttons, the active tab, switches), Roboto in a clear type scale.
-- Everything is drawn by hand (no DFrame chrome), sized in 1080p pixels and
-- scaled with the screen height (never below 0.85, so 720p stays readable).
-- Only menu-state globals are used: SysTime, never RealTime/FrameTime.

local UI = {}

-- draw.SimpleText alignments. The menu state has no TEXT_ALIGN_* globals, and
-- nil there means top-left, so every centred label would sit off-centre.
UI.LEFT, UI.CENTER, UI.RIGHT = 0, 1, 2

UI.C = {
	window = Color(20, 21, 24),        -- the window
	surface = Color(27, 28, 32),       -- cards, rows
	raised = Color(36, 37, 42),        -- hover, active tab, chips
	border = Color(44, 45, 51),
	divider = Color(36, 37, 42),
	accent = Color(194, 64, 122),      -- #c2407a
	accentHi = Color(212, 86, 144),
	accentSoft = Color(194, 64, 122, 40),
	green = Color(63, 185, 122),
	greenSoft = Color(63, 185, 122, 38),
	amber = Color(226, 168, 62),
	amberSoft = Color(226, 168, 62, 38),
	text = Color(232, 233, 236),       -- headings, values
	body = Color(178, 182, 190),       -- paragraphs
	dim = Color(124, 129, 138),        -- labels, hints
	white = Color(255, 255, 255),
	backdrop = Color(8, 8, 10, 175),
}
local C = UI.C

-- 1080p pixels to pixels on this screen.
function UI.S(n)
	local scale = math.min(math.max(ScrH() / 1080, 0.85), 2.4)
	return math.floor(n * scale + 0.5)
end
local S = UI.S

local fontsFor
function UI.fonts()
	local h = ScrH()
	if fontsFor == h then return end
	fontsFor = h
	local function font(name, size, weight)
		surface.CreateFont("MeowyPack." .. name, { font = "Roboto", size = S(size), weight = weight, antialias = true, extended = true })
	end
	font("Title", 22, 700)
	font("Heading", 16, 700)
	font("Body", 15, 500)
	font("Small", 13, 500)
	font("Label", 12, 700)
	font("Button", 15, 700)
end

local function lerp(a, b, t) return a + (b - a) * t end
local function mix(c1, c2, t)
	return Color(lerp(c1.r, c2.r, t), lerp(c1.g, c2.g, t), lerp(c1.b, c2.b, t), lerp(c1.a or 255, c2.a or 255, t))
end
UI.mix = mix

-- Seconds since this panel's `key` animation last stepped (SysTime; per panel,
-- clamped so a stall doesn't jump an animation).
local function stepTime(p, key)
	local now = SysTime()
	local before = p[key] or now
	p[key] = now
	return math.min(math.max(now - before, 0), 0.1)
end

-- Hover amount 0..1, eased, kept on the panel.
local function hover(p, on)
	p.meowyHover = lerp(p.meowyHover or 0, on and 1 or 0, math.min(stepTime(p, "meowyHoverT") * 14, 1))
	return p.meowyHover
end
UI.hover = hover

-- A rounded box with an optional 1px border.
function UI.box(r, x, y, w, h, fill, border)
	if border then
		draw.RoundedBox(r, x, y, w, h, border)
		draw.RoundedBox(math.max(r - 1, 0), x + 1, y + 1, w - 2, h - 2, fill)
	else
		draw.RoundedBox(r, x, y, w, h, fill)
	end
end

-- A soft drop shadow under a w x h box with corner radius r.
local SHADOW = { Color(0, 0, 0, 34), Color(0, 0, 0, 20), Color(0, 0, 0, 9) }
function UI.shadow(r, x, y, w, h)
	-- Three soft layers: enough depth, cheap enough for several open windows.
	for i = 1, 3 do
		local g = S(i * 5)
		draw.RoundedBox(r + g, x - g, y - g + S(4), w + g * 2, h + g * 2, SHADOW[i])
	end
end

function UI.fadeIn(p)
	if p.AlphaTo then
		p:SetAlpha(0)
		p:AlphaTo(255, 0.14, 0)
	end
end

-- Width of `text` in `font`.
function UI.textW(text, font)
	surface.SetFont(font)
	return (surface.GetTextSize(text))
end

-- Word-wrap `text` to `width` pixels in `font`: a list of lines.
function UI.wrap(text, font, width)
	surface.SetFont(font)
	local lines = {}
	for para in (tostring(text) .. "\n"):gmatch("(.-)\n") do
		local line = ""
		for word in para:gmatch("%S+") do
			local try = line == "" and word or (line .. " " .. word)
			if line ~= "" and surface.GetTextSize(try) > width then
				lines[#lines + 1] = line
				line = word
			else
				line = try
			end
		end
		lines[#lines + 1] = line
	end
	return lines
end

local function lineHeight(font)
	surface.SetFont(font)
	local _, h = surface.GetTextSize("Ag")
	return math.floor(h * 1.32 + 0.5)
end
UI.lineHeight = lineHeight

-- Wrapped text. :Fit(width) wraps it and returns its height.
function UI.Text(parent, text, font, color)
	local p = vgui.Create("DPanel", parent)
	p.text, p.font, p.color, p.h = text, font or "MeowyPack.Body", color or C.body, 0
	p:SetMouseInputEnabled(false)
	function p:Fit(width)
		self.lines = UI.wrap(self.text, self.font, width)
		self.lh = lineHeight(self.font)
		self.h = #self.lines * self.lh
		self:SetSize(width, self.h)
		return self.h
	end
	p.Paint = function(self)
		for i, line in ipairs(self.lines or {}) do
			draw.SimpleText(line, self.font, 0, (i - 1) * self.lh, self.color)
		end
		return true
	end
	return p
end

-- A small uppercase section label ("CONNECTION").
function UI.Label(parent, text)
	return UI.Text(parent, string.upper(text), "MeowyPack.Label", C.dim)
end

-- A pill-shaped status chip at x, y (or right-aligned to x when alignRight).
-- tone: "green", "amber", "accent" or nil (neutral). Returns width, height.
function UI.chip(text, x, y, tone, alignRight)
	surface.SetFont("MeowyPack.Label")
	local tw, th = surface.GetTextSize(text)
	local w, h = tw + S(16), th + S(8)
	if alignRight then x = x - w end
	local bg = tone == "green" and C.greenSoft or tone == "amber" and C.amberSoft or tone == "accent" and C.accentSoft or C.raised
	local fg = tone == "green" and C.green or tone == "amber" and C.amber or tone == "accent" and C.accentHi or C.dim
	draw.RoundedBox(math.floor(h / 2), x, y, w, h, bg)
	draw.SimpleText(text, "MeowyPack.Label", x + w / 2, y + h / 2, fg, UI.CENTER, UI.CENTER)
	return w, h
end

-- Icons, drawn (fonts may not have the glyphs). cx, cy: centre; r: radius.
function UI.iconCheck(cx, cy, r, color)
	surface.SetDrawColor(color.r, color.g, color.b, color.a or 255)
	for o = -1, 1 do
		surface.DrawLine(cx - r * 0.42, cy + o, cx - r * 0.1, cy + r * 0.32 + o)
		surface.DrawLine(cx - r * 0.1, cy + r * 0.32 + o, cx + r * 0.45, cy - r * 0.3 + o)
	end
end

function UI.iconAlert(cx, cy, r, color)
	local w = math.max(S(3), 2)
	draw.RoundedBox(w / 2, cx - w / 2, cy - r * 0.45, w, r * 0.6, color)
	draw.RoundedBox(w / 2, cx - w / 2, cy + r * 0.28, w, w, color)
end

-- Four rising bars: connection strength.
function UI.iconSignal(cx, cy, r, color)
	local bw, gap = math.max(S(3), 2), math.max(S(2), 1)
	local total = bw * 4 + gap * 3
	local x = cx - total / 2
	for i = 1, 4 do
		local h = r * (0.35 + 0.22 * (i - 1))
		draw.RoundedBox(1, x, cy + r * 0.5 - h, bw, h, color)
		x = x + bw + gap
	end
end

-- A badge: a soft circle with an icon. kind: "check", "alert", "signal".
function UI.badge(cx, cy, r, kind, tone)
	local soft = tone == "green" and C.greenSoft or tone == "amber" and C.amberSoft or C.accentSoft
	local fg = tone == "green" and C.green or tone == "amber" and C.amber or C.accentHi
	draw.RoundedBox(r, cx - r, cy - r, r * 2, r * 2, soft)
	if kind == "check" then UI.iconCheck(cx, cy, r * 0.62, fg)
	elseif kind == "alert" then UI.iconAlert(cx, cy, r * 0.75, fg)
	else UI.iconSignal(cx, cy, r * 0.62, fg) end
end

-- A spinner: dots around a circle, the bright one going round.
function UI.spinner(cx, cy, r, color)
	local n, t = 8, SysTime() * 1.4
	for i = 0, n - 1 do
		local a = i / n * math.pi * 2
		local phase = (t - i / n) % 1
		local d = math.max(S(4), 3)
		local alpha = 255 * (0.18 + 0.82 * (1 - phase) ^ 3)
		draw.RoundedBox(d / 2, cx + math.cos(a) * r - d / 2, cy + math.sin(a) * r - d / 2, d, d, Color(color.r, color.g, color.b, alpha))
	end
end

-- A button. kind: "primary" (filled accent), "secondary" (outlined), "ghost" (text only).
function UI.Button(parent, text, kind, onClick)
	UI.fonts()
	local b = vgui.Create("DButton", parent)
	b:SetText(text)
	b:SetFont("MeowyPack.Button")
	b.kind = kind or "secondary"
	b.w, b.h = UI.textW(text, "MeowyPack.Button") + S(b.kind == "ghost" and 24 or 36), S(38)
	b:SetSize(b.w, b.h)
	b.UpdateColours = function(self)
		if self.kind == "primary" then return self:SetTextStyleColor(C.white) end
		return self:SetTextStyleColor(mix(C.body, C.text, self.meowyHover or 0))
	end
	b.Paint = function(self, w, h)
		local t = hover(self, self:IsHovered())
		if self.kind == "primary" then
			UI.box(S(8), 0, 0, w, h, mix(C.accent, C.accentHi, t))
		elseif self.kind == "ghost" then
			if t > 0.01 then UI.box(S(8), 0, 0, w, h, Color(255, 255, 255, 12 * t)) end
		else
			UI.box(S(8), 0, 0, w, h, mix(C.surface, C.raised, t), mix(C.border, Color(70, 72, 80), t))
		end
	end
	b.DoClick = function() if onClick then onClick() end end
	return b
end

-- A small "x" for closing.
function UI.Close(parent, onClick)
	local b = vgui.Create("DButton", parent)
	b:SetText("")
	b.meowyClose = true
	b.w = S(30)
	b:SetSize(b.w, b.w)
	b.Paint = function(self, w, h)
		local t = hover(self, self:IsHovered())
		if t > 0.01 then draw.RoundedBox(S(7), 0, 0, w, h, Color(255, 255, 255, 14 * t)) end
		local c = mix(C.dim, C.text, t)
		surface.SetDrawColor(c.r, c.g, c.b, 255)
		local m, n = math.floor(w * 0.35), math.floor(w * 0.65)
		for o = 0, 1 do
			surface.DrawLine(m + o, m, n + o, n)
			surface.DrawLine(n + o, m, m + o, n)
		end
		return true
	end
	b.DoClick = function() if onClick then onClick() end end
	return b
end

-- A setting row: title and description on the left, a switch on the right,
-- on a surface card. The whole row toggles. :Fit(width) -> height.
function UI.Toggle(parent, label, description, on, onChange)
	local p = vgui.Create("DButton", parent)
	p:SetText("")
	p.on, p.knob = on and true or false, on and 1 or 0
	p.title = UI.Text(p, label, "MeowyPack.Heading", C.text)
	p.desc = description and UI.Text(p, description, "MeowyPack.Small", C.dim) or nil
	p.sw, p.sh, p.pad = S(42), S(24), S(18)
	function p:Fit(width)
		local textW = width - self.pad * 2 - self.sw - S(24)
		local h = self.title:Fit(textW)
		self.title:SetPos(self.pad, self.pad)
		if self.desc then
			self.desc:SetPos(self.pad, self.pad + h + S(4))
			h = h + S(4) + self.desc:Fit(textW)
		end
		self.h = math.max(h, self.sh) + self.pad * 2
		self:SetSize(width, self.h)
		return self.h
	end
	function p:SetOn(value)
		self.on = value and true or false
		if onChange then onChange(self.on) end
	end
	p.DoClick = function(self) self:SetOn(not self.on) end
	p.Paint = function(self, w, h)
		local t = hover(self, self:IsHovered())
		UI.box(S(10), 0, 0, w, h, mix(C.surface, Color(31, 32, 37), t), C.border)
		self.knob = lerp(self.knob, self.on and 1 or 0, math.min(stepTime(self, "meowyKnobT") * 16, 1))
		local x, y = w - self.pad - self.sw, math.floor((h - self.sh) / 2)
		draw.RoundedBox(self.sh / 2, x, y, self.sw, self.sh, mix(Color(58, 60, 68), C.accent, self.knob))
		local k = self.sh - S(6)
		draw.RoundedBox(k / 2, x + S(3) + (self.sw - k - S(6)) * self.knob, y + S(3), k, k, C.white)
		return true
	end
	return p
end

-- A setting row with a choice of a few values: title and description above a
-- row of segments, on a surface card. options: { { label, value }, ... }.
-- Clicking a segment picks it. :Fit(width) -> height.
function UI.Choice(parent, label, description, options, value, onChange)
	UI.fonts()
	local p = vgui.Create("DButton", parent)
	p:SetText("")
	p.options, p.value = options, value
	p.title = UI.Text(p, label, "MeowyPack.Heading", C.text)
	p.desc = description and UI.Text(p, description, "MeowyPack.Small", C.dim) or nil
	p.pad, p.segH = S(18), S(32)
	function p:Fit(width)
		local textW = width - self.pad * 2
		local h = self.title:Fit(textW)
		self.title:SetPos(self.pad, self.pad)
		if self.desc then
			self.desc:SetPos(self.pad, self.pad + h + S(4))
			h = h + S(4) + self.desc:Fit(textW)
		end
		self.segY = self.pad + h + S(12)
		self.h = self.segY + self.segH + self.pad
		self:SetSize(width, self.h)
		return self.h
	end
	-- The segment under x (panel-local), or nil.
	function p:SegmentAt(x, y)
		if not x or not self.segY then return nil end
		if y and (y < self.segY or y > self.segY + self.segH) then return nil end
		local w = self:GetWide() - self.pad * 2
		local i = math.floor((x - self.pad) / (w / #self.options)) + 1
		if i >= 1 and i <= #self.options then return i end
	end
	function p:SetValue(v)
		self.value = v
		if onChange then onChange(v) end
	end
	p.DoClick = function(self)
		local x, y = self:CursorPos()
		local i = self:SegmentAt(x, y)
		if i and self.options[i][2] ~= self.value then self:SetValue(self.options[i][2]) end
	end
	p.Paint = function(self, w, h)
		UI.box(S(10), 0, 0, w, h, C.surface, C.border)
		local n = #self.options
		local sw = (w - self.pad * 2) / n
		local mx, my = self:CursorPos()
		local over = self:IsHovered() and self:SegmentAt(mx, my)
		surface.SetFont("MeowyPack.Button")
		for i, o in ipairs(self.options) do
			local x = math.floor(self.pad + (i - 1) * sw)
			local selected = o[2] == self.value
			local fill = selected and C.accent or (over == i and C.raised or Color(36, 37, 42))
			draw.RoundedBox(S(6), x + 1, self.segY, math.floor(sw) - 2, self.segH, fill)
			draw.SimpleText(o[1], "MeowyPack.Button", x + sw / 2, self.segY + self.segH / 2, selected and C.white or C.body, UI.CENTER, UI.CENTER)
		end
		return true
	end
	return p
end

-- Lays panels out top to bottom at x, from y, `width` wide, with `gap`
-- between them. Each needs :Fit(width) or a .h. Returns the y below the last.
function UI.stack(panels, x, y, width, gap)
	for i, p in ipairs(panels) do
		if i > 1 then y = y + (p.gapBefore or gap or S(12)) end
		local h = p.Fit and p:Fit(width) or p.h or 0
		p:SetPos(x, y)
		y = y + h
	end
	return y
end

-- Buttons in a row, right-aligned in `width`; returns the row height.
function UI.row(buttons, x, y, width)
	local right, h = x + width, 0
	for i = #buttons, 1, -1 do
		local b = buttons[i]
		right = right - b.w
		b:SetPos(right, y)
		right = right - S(10)
		h = math.max(h, b.h)
	end
	return h
end

-- A card on the surface colour. Children go in with :Add(block, gapBefore);
-- :Fit(width) stacks them inside the padding and returns the height.
-- card.badge = { kind, tone } draws an icon badge at the top left.
function UI.Card(parent)
	local p = vgui.Create("DPanel", parent)
	p.blocks, p.pad = {}, S(18)
	function p:Add(block, gapBefore)
		block.gapBefore = gapBefore
		self.blocks[#self.blocks + 1] = block
		return block
	end
	function p:Fit(width)
		local x = self.pad + (self.badge and S(52) or 0)
		local y = UI.stack(self.blocks, x, self.pad, width - x - self.pad, S(6))
		self.h = math.max(y + self.pad, self.badge and S(38) + self.pad * 2 or 0)
		self:SetSize(width, self.h)
		return self.h
	end
	p.Paint = function(self, w, h)
		UI.box(S(10), 0, 0, w, h, C.surface, C.border)
		if self.badge then UI.badge(self.pad + S(19), self.pad + S(19), S(19), self.badge[1], self.badge[2]) end
		return true
	end
	return p
end

-- Key/value rows ("GPU   NVIDIA GeForce ..."), as one block. :Fit(width) -> height.
function UI.Details(parent, rows)
	local p = vgui.Create("DPanel", parent)
	p:SetMouseInputEnabled(false)
	p.rows = rows
	function p:Fit(width)
		self.keyW = 0
		for _, r in ipairs(self.rows) do self.keyW = math.max(self.keyW, UI.textW(r[1], "MeowyPack.Small")) end
		self.keyW = self.keyW + S(24)
		self.lh = lineHeight("MeowyPack.Small")
		self.items, self.h = {}, 0
		for _, r in ipairs(self.rows) do
			local lines = UI.wrap(r[2], "MeowyPack.Small", width - self.keyW)
			self.items[#self.items + 1] = { key = r[1], lines = lines, y = self.h }
			self.h = self.h + #lines * self.lh + S(10)
		end
		self.h = math.max(self.h - S(10), 0)
		self:SetSize(width, self.h)
		return self.h
	end
	p.Paint = function(self)
		for _, it in ipairs(self.items or {}) do
			draw.SimpleText(it.key, "MeowyPack.Small", 0, it.y, C.dim)
			for i, line in ipairs(it.lines) do
				draw.SimpleText(line, "MeowyPack.Small", self.keyW, it.y + (i - 1) * self.lh, C.text)
			end
		end
		return true
	end
	return p
end

-- A full-screen dim backdrop with a centred window. Returns backdrop, window.
-- Escape (and the "x", when there is one) call onDismiss.
function UI.Modal(width, onDismiss)
	UI.fonts()
	local back = vgui.Create("EditablePanel")
	back:SetSize(ScrW(), ScrH())
	back:SetPos(0, 0)
	local card = vgui.Create("EditablePanel", back)
	card.w, card.r = S(width), S(14)
	card.Paint = function(self, w, h) UI.box(self.r, 0, 0, w, h, C.window, C.border) end
	back.Paint = function(self, w, h)
		draw.RoundedBox(0, 0, 0, w, h, C.backdrop)
		if IsValid(card) then
			local x, y = card:GetPos()
			UI.shadow(card.r, x, y, card:GetWide(), card:GetTall())
		end
	end
	function back:Dismiss()
		if self.dismissed then return end
		self.dismissed = true
		if onDismiss then onDismiss() end
		self:Remove()
	end
	back.OnKeyCodePressed = function(self, key) if KEY_ESCAPE and key == KEY_ESCAPE then self:Dismiss() end end
	-- Size the window to its contents (call after adding them), centre it, show.
	function back:Open(height)
		card:SetSize(card.w, height)
		card:SetPos(math.floor((ScrW() - card.w) / 2), math.floor((ScrH() - height) / 2))
		self:MakePopup()
		self:SetKeyboardInputEnabled(true)
		self:RequestFocus()
		UI.fadeIn(self)
	end
	return back, card
end

-- A prompt: an icon badge, title, body, optional note, buttons
-- { { text, kind, fn } }. Buttons close it after running fn. closable adds
-- the "x" (it and Escape run onDismiss). icon = { kind, tone }.
function UI.Prompt(opts)
	local back, card = UI.Modal(opts.width or 480, opts.onDismiss)
	local pad = S(28)
	local inner = card.w - pad * 2
	local top = pad
	if opts.icon then
		local r, icon = S(22), opts.icon
		local badge = vgui.Create("DPanel", card)
		badge:SetMouseInputEnabled(false)
		badge:SetPos(pad, pad)
		badge:SetSize(r * 2, r * 2)
		badge.Paint = function() UI.badge(r, r, r, icon[1], icon[2]) return true end
		top = pad + r * 2 + S(18)
	end
	local blocks = { UI.Text(card, opts.title, "MeowyPack.Title", C.text) }
	local body = UI.Text(card, opts.body, "MeowyPack.Body", C.body)
	body.gapBefore = S(8)
	blocks[#blocks + 1] = body
	if opts.note then
		local note = UI.Text(card, opts.note, "MeowyPack.Small", C.dim)
		note.gapBefore = S(14)
		blocks[#blocks + 1] = note
	end
	local y = UI.stack(blocks, pad, top, inner)
	if opts.closable then
		local x = UI.Close(card, function() back:Dismiss() end)
		x:SetPos(card.w - S(16) - x.w, S(16))
	end
	local buttons = {}
	for _, spec in ipairs(opts.buttons or {}) do
		local fn = spec[3]
		buttons[#buttons + 1] = UI.Button(card, spec[1], spec[2], function()
			back.dismissed = true   -- answered: not a dismissal
			back:Remove()
			if fn then fn() end
		end)
	end
	y = y + S(26)
	y = y + UI.row(buttons, pad, y, inner) + pad
	back:Open(y)
	return back
end

return UI
