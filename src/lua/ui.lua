-- The pack's menu UI kit (menu state). module.cpp puts this first in the
-- resilience chunk, as the local `UI` that doctor.lua and resilience.lua use.
--
-- Meowy's in-game look: dark cards (#18191c / #1f2023) with a 1px #2a2b2f
-- border, 12px corners, a pink accent (#c2407a), Roboto. Everything is drawn
-- by hand (no DFrame chrome) and scaled with the screen height, readable from
-- 1280x720 to 4K. Layout is computed in Lua when a panel is built (text is
-- wrapped by measuring it), so sizes never wait for a Derma layout pass.

local UI = {}

UI.C = {
	bg = Color(24, 25, 28),           -- #18191c
	card = Color(31, 32, 35),         -- #1f2023
	cardHi = Color(38, 39, 43),
	border = Color(42, 43, 47),       -- #2a2b2f
	accent = Color(194, 64, 122),     -- #c2407a
	accentHi = Color(214, 88, 146),
	amber = Color(217, 164, 65),
	green = Color(76, 175, 120),
	text = Color(195, 198, 204),      -- #c3c6cc
	dim = Color(139, 143, 151),       -- #8b8f97
	white = Color(255, 255, 255),
	backdrop = Color(0, 0, 0, 160),
}
local C = UI.C

-- Pixels at 1080p to pixels on this screen. Never below 0.85 (720p stays readable).
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
	surface.CreateFont("MeowyPack.Title", { font = "Roboto", size = S(26), weight = 700, antialias = true })
	surface.CreateFont("MeowyPack.Heading", { font = "Roboto", size = S(19), weight = 700, antialias = true })
	surface.CreateFont("MeowyPack.Body", { font = "Roboto", size = S(17), weight = 500, antialias = true })
	surface.CreateFont("MeowyPack.Small", { font = "Roboto", size = S(15), weight = 500, antialias = true })
	surface.CreateFont("MeowyPack.Button", { font = "Roboto", size = S(16), weight = 700, antialias = true })
end

local function lerp(a, b, t) return a + (b - a) * t end
local function mix(c1, c2, t)
	return Color(lerp(c1.r, c2.r, t), lerp(c1.g, c2.g, t), lerp(c1.b, c2.b, t), lerp(c1.a or 255, c2.a or 255, t))
end
local function frameTime() return RealFrameTime and RealFrameTime() or 0.016 end

-- A 1px-bordered rounded box: the border colour, then the fill inset by 1px.
function UI.box(r, x, y, w, h, fill, border)
	if border then
		draw.RoundedBox(r, x, y, w, h, border)
		draw.RoundedBox(math.max(r - 1, 0), x + 1, y + 1, w - 2, h - 2, fill)
	else
		draw.RoundedBox(r, x, y, w, h, fill)
	end
end

-- Hover amount 0..1, eased, kept on the panel.
local function hover(p, on)
	p.meowyHover = lerp(p.meowyHover or 0, on and 1 or 0, math.min(frameTime() * 14, 1))
	return p.meowyHover
end
UI.hover = hover

function UI.fadeIn(p)
	if p.AlphaTo then
		p:SetAlpha(0)
		p:AlphaTo(255, 0.15, 0)
	end
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
	return math.floor(h * 1.25 + 0.5)
end

-- Wrapped text. :Fit(width) wraps it and returns its height.
function UI.Text(parent, text, font, color)
	local p = vgui.Create("DPanel", parent)
	p.text, p.font, p.color, p.h = text, font or "MeowyPack.Body", color or C.text, 0
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

-- A button. kind: "primary" (filled accent), "secondary" (outline), "ghost".
function UI.Button(parent, text, kind, onClick)
	UI.fonts()
	local b = vgui.Create("DButton", parent)
	b:SetText(text)
	b:SetFont("MeowyPack.Button")
	b.kind = kind or "secondary"
	surface.SetFont("MeowyPack.Button")
	local tw = surface.GetTextSize(text)
	b.w, b.h = tw + S(36), S(40)
	b:SetSize(b.w, b.h)
	b.UpdateColours = function(self) return self:SetTextStyleColor(self.kind == "primary" and C.white or C.text) end
	b.Paint = function(self, w, h)
		local t = hover(self, self:IsHovered())
		if self.kind == "primary" then
			UI.box(S(8), 0, 0, w, h, mix(C.accent, C.accentHi, t))
		elseif self.kind == "ghost" then
			if t > 0.01 then UI.box(S(8), 0, 0, w, h, Color(255, 255, 255, 14 * t)) end
		else
			UI.box(S(8), 0, 0, w, h, mix(C.card, C.cardHi, t), mix(C.border, C.dim, t * 0.6))
		end
	end
	b.DoClick = function() if onClick then onClick() end end
	return b
end

-- A small round "x" for closing.
function UI.Close(parent, onClick)
	local b = vgui.Create("DButton", parent)
	b:SetText("")
	b.meowyClose = true
	b.w = S(32)
	b:SetSize(b.w, b.w)
	b.Paint = function(self, w, h)
		local t = hover(self, self:IsHovered())
		if t > 0.01 then draw.RoundedBox(S(8), 0, 0, w, h, Color(255, 255, 255, 18 * t)) end
		local c = mix(C.dim, C.white, t)
		surface.SetDrawColor(c.r, c.g, c.b, 255)
		local m, n = math.floor(w * 0.34), math.floor(w * 0.66)
		for o = 0, 1 do
			surface.DrawLine(m + o, m, n + o, n)
			surface.DrawLine(n + o, m, m + o, n)
		end
		return true
	end
	b.DoClick = function() if onClick then onClick() end end
	return b
end

-- A toggle switch with a label and a description. :Fit(width) -> height.
function UI.Toggle(parent, label, description, on, onChange)
	local p = vgui.Create("DButton", parent)
	p:SetText("")
	p.on, p.knob = on and true or false, on and 1 or 0
	p.title = UI.Text(p, label, "MeowyPack.Heading", C.white)
	p.desc = description and UI.Text(p, description, "MeowyPack.Small", C.dim) or nil
	p.sw, p.sh = S(46), S(26)
	function p:Fit(width)
		local textW = width - self.sw - S(20)
		local h = self.title:Fit(textW)
		self.title:SetPos(0, 0)
		if self.desc then
			self.desc:SetPos(0, h + S(4))
			h = h + S(4) + self.desc:Fit(textW)
		end
		self.h = math.max(h, self.sh)
		self:SetSize(width, self.h)
		return self.h
	end
	function p:SetOn(value)
		self.on = value and true or false
		if onChange then onChange(self.on) end
	end
	p.DoClick = function(self) self:SetOn(not self.on) end
	p.Paint = function(self, w)
		self.knob = lerp(self.knob, self.on and 1 or 0, math.min(frameTime() * 16, 1))
		local t = hover(self, self:IsHovered())
		local x, y = w - self.sw, 0
		local track = mix(mix(C.border, C.cardHi, t), C.accent, self.knob)
		draw.RoundedBox(self.sh / 2, x, y, self.sw, self.sh, track)
		local k = self.sh - S(6)
		draw.RoundedBox(k / 2, x + S(3) + (self.sw - k - S(6)) * self.knob, y + S(3), k, k, C.white)
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

-- A card: #1f2023 with a 1px border, optional coloured left bar. Children are
-- added to .blocks; :Fit(width) stacks them inside the padding -> height.
function UI.Card(parent, bar)
	local p = vgui.Create("DPanel", parent)
	p.blocks, p.bar, p.pad = {}, bar, S(18)
	p.indent = 0   -- room on the left for an icon
	function p:Add(block, gapBefore)
		block.gapBefore = gapBefore
		self.blocks[#self.blocks + 1] = block
		return block
	end
	function p:Fit(width)
		local x = self.pad + (self.bar and S(4) or 0) + self.indent
		local y = UI.stack(self.blocks, x, self.pad, width - x - self.pad, S(8))
		self.h = y + self.pad
		self:SetSize(width, self.h)
		return self.h
	end
	p.Paint = function(self, w, h)
		UI.box(S(10), 0, 0, w, h, C.card, C.border)
		if self.bar then draw.RoundedBoxEx(S(10), 0, 0, S(4), h, self.bar, true, false, true, false) end
		if self.icon then
			local r = S(13)
			local cx, cy = self.pad + S(4) + r, self.pad + r
			draw.RoundedBox(r, cx - r, cy - r, r * 2, r * 2, self.icon.color)
			if self.icon.check then   -- a tick, drawn (fonts may not have the glyph)
				surface.SetDrawColor(255, 255, 255, 255)
				for o = -1, 1 do
					surface.DrawLine(cx - r * 0.45, cy + o, cx - r * 0.1, cy + r * 0.35 + o)
					surface.DrawLine(cx - r * 0.1, cy + r * 0.35 + o, cx + r * 0.5, cy - r * 0.35 + o)
				end
			else
				draw.SimpleText(self.icon.glyph or "!", "MeowyPack.Button", cx, cy, C.white, TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER)
			end
		end
		return true
	end
	return p
end

-- A full-screen dim backdrop with a centred card. Returns backdrop, card.
-- Escape (and the "x", when closable) call onDismiss.
function UI.Modal(width, onDismiss)
	UI.fonts()
	local back = vgui.Create("EditablePanel")
	back:SetSize(ScrW(), ScrH())
	back:SetPos(0, 0)
	back.Paint = function(self, w, h) draw.RoundedBox(0, 0, 0, w, h, C.backdrop) end
	local card = vgui.Create("EditablePanel", back)
	card.w = S(width)
	card.Paint = function(self, w, h)
		UI.box(S(12), 0, 0, w, h, C.bg, C.border)
		draw.RoundedBoxEx(S(12), 0, 0, S(5), h, C.accent, true, false, true, false)
	end
	function back:Dismiss()
		if self.dismissed then return end
		self.dismissed = true
		if onDismiss then onDismiss() end
		self:Remove()
	end
	back.OnKeyCodePressed = function(self, key) if KEY_ESCAPE and key == KEY_ESCAPE then self:Dismiss() end end
	-- Size the card to its contents (call after adding them), centre it, show.
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

-- A prompt: title, body, optional note, buttons { { text, kind, fn } }.
-- Buttons close it after running fn. closable adds the "x" (it and Escape
-- run onDismiss).
function UI.Prompt(opts)
	local back, card = UI.Modal(opts.width or 520, opts.onDismiss)
	local pad = S(28)
	local inner = card.w - pad * 2
	local blocks = { UI.Text(card, opts.title, "MeowyPack.Title", C.white) }
	local body = UI.Text(card, opts.body, "MeowyPack.Body", C.text)
	body.gapBefore = S(10)
	blocks[#blocks + 1] = body
	if opts.note then
		local note = UI.Text(card, opts.note, "MeowyPack.Small", C.dim)
		note.gapBefore = S(12)
		blocks[#blocks + 1] = note
	end
	local y = UI.stack(blocks, pad, pad, opts.closable and inner - S(36) or inner)
	if opts.closable then
		local x = UI.Close(card, function() back:Dismiss() end)
		x:SetPos(card.w - S(14) - x.w, S(14))
	end
	local buttons = {}
	for _, spec in ipairs(opts.buttons or {}) do
		local fn = spec[3]
		local b = UI.Button(card, spec[1], spec[2], function()
			back.dismissed = true   -- answered: not a dismissal
			back:Remove()
			if fn then fn() end
		end)
		buttons[#buttons + 1] = b
	end
	y = y + S(24)
	y = y + UI.row(buttons, pad, y, inner) + pad
	back:Open(y)
	return back
end

return UI
