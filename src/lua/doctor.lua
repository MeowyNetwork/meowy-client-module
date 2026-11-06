-- PC doctor, menu state. Runs with `Doctor` (doctor_logic.lua) in scope:
-- module.cpp joins the files into one chunk, before resilience.lua, which
-- shows the doctor as a tab of the settings window through `DoctorUI`.
-- meowy_doctor prints the same findings to the console. Read-only and local:
-- nothing is changed and nothing is sent anywhere.
local meowy, Doctor = meowy, Doctor

local function cvarString(name)
	local cv = GetConVar and GetConVar(name)
	if cv then return cv:GetString() end
	if GetConVarString then
		local v = GetConVarString(name)
		if v ~= "" then return v end
	end
	return nil
end

local DoctorUI = {}

function DoctorUI.diagnose()
	local ok, facts = pcall(meowy.DoctorFacts)
	if not ok or type(facts) ~= "table" then facts = {} end
	return Doctor.diagnose(facts, { mcore = cvarString("gmod_mcore_test"), queue = cvarString("mat_queue_mode") })
end

function DoctorUI.tabName(tips)
	return "PC doctor (" .. Doctor.summary(tips) .. ")"
end

local FG, DIM = Color(30, 30, 35), Color(70, 70, 80)

-- The tab's contents: each tip (title, what to do, a button when there's a
-- page to open), then what was found.
function DoctorUI.fill(parent, tips, report)
	local scroll = vgui.Create("DScrollPanel", parent)
	scroll:Dock(FILL)
	local function label(text, font, color, top)
		local l = scroll:Add("DLabel")
		l:SetFont(font)
		l:SetTextColor(color)
		l:SetWrap(true)
		l:SetAutoStretchVertical(true)
		l:SetText(text)
		l:Dock(TOP)
		l:DockMargin(10, top or 2, 10, 0)
		return l
	end
	if #tips == 0 then label("Nothing to fix: this PC is set up well for Garry's Mod.", "DermaDefaultBold", FG, 10) end
	for _, tip in ipairs(tips) do
		label(tip.title, "DermaDefaultBold", FG, 10)
		label(tip.text, "DermaDefault", DIM)
		if tip.action then
			local b = scroll:Add("DButton")
			b:SetText(tip.action_label or "Open")
			b:SetTall(24)
			b:Dock(TOP)
			b:DockMargin(10, 4, 330, 0)
			local action = tip.action
			b.DoClick = function() meowy.DoctorOpen(action) end
		end
	end
	label("What the doctor found", "DermaDefaultBold", FG, 14)
	for _, line in ipairs(report) do label(line, "DermaDefault", DIM) end
	label("Nothing here leaves your PC, and the pack doesn't change these settings for you.", "DermaDefault", DIM, 10)
end

if concommand then
	concommand.Add("meowy_doctor", function()
		local tips, report = DoctorUI.diagnose()
		Msg(Doctor.consoleText(tips, report))
	end, nil, "Meowy Client Pack: check this PC for common slowdowns (read-only)")
end
