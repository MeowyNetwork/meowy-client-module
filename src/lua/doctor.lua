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

-- The PC doctor tab's contents, as blocks for the settings window to stack:
-- a summary, one card per tip (coloured by how much it matters, with its
-- button), then what was found.
function DoctorUI.build(tips, report)
	local C, S = UI.C, UI.S
	local blocks = {}
	local head = UI.Text(nil, "PC doctor", "MeowyPack.Heading", C.white)
	blocks[#blocks + 1] = head
	local summary = #tips == 0 and "Checked just now. Nothing to fix: this PC is set up well for Garry's Mod."
		or ("Checked just now. %s for this PC."):format(Doctor.summary(tips))
	local sub = UI.Text(nil, summary, "MeowyPack.Small", C.dim)
	sub.gapBefore = S(4)
	blocks[#blocks + 1] = sub

	if #tips == 0 then
		local ok = UI.Card(nil, C.green)
		ok.icon, ok.indent = { color = C.green, check = true }, S(40)
		ok:Add(UI.Text(ok, "All good", "MeowyPack.Heading", C.white))
		ok:Add(UI.Text(ok, "The game is on your strongest GPU, multicore rendering is on and nothing is holding the PC back.", "MeowyPack.Body", C.text), S(4))
		ok.gapBefore = S(16)
		blocks[#blocks + 1] = ok
	end
	for i, tip in ipairs(tips) do
		local color = tip.severity == "high" and C.accent or C.amber
		local card = UI.Card(nil, color)
		card.icon, card.indent = { color = color, glyph = "!" }, S(40)
		card.meowyTip = tip.id
		card:Add(UI.Text(card, tip.title, "MeowyPack.Heading", C.white))
		card:Add(UI.Text(card, tip.text, "MeowyPack.Body", C.text), S(6))
		if tip.action then
			local action = tip.action
			card:Add(UI.Button(card, tip.action_label or "Open", "primary", function() meowy.DoctorOpen(action) end), S(14))
		end
		card.gapBefore = i == 1 and S(16) or S(12)
		blocks[#blocks + 1] = card
	end

	local found = UI.Card(nil)
	found:Add(UI.Text(found, "What the doctor found", "MeowyPack.Heading", C.white))
	for _, line in ipairs(report) do
		found:Add(UI.Text(found, line, "MeowyPack.Small", C.text), S(6))
	end
	found.gapBefore = S(20)
	blocks[#blocks + 1] = found
	local note = UI.Text(nil, "Nothing here leaves your PC, and the pack doesn't change these settings for you.", "MeowyPack.Small", C.dim)
	note.gapBefore = S(12)
	blocks[#blocks + 1] = note
	return blocks
end

if concommand then
	concommand.Add("meowy_doctor", function()
		local tips, report = DoctorUI.diagnose()
		Msg(Doctor.consoleText(tips, report))
	end, nil, "Meowy Client Pack: check this PC for common slowdowns (read-only)")
end
