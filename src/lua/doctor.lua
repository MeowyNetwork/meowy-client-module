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

-- "GPU: NVIDIA ... (8017 MB), drawing the game" -> { "GPU", "NVIDIA ... drawing the game" }
local function splitLine(line)
	local key, value = tostring(line):match("^([^:]+):%s*(.+)$")
	if key and #key <= 18 then
		-- Sentence case, except a value that starts with a cvar ("gmod_mcore_test 1").
		if not value:match("^%S*_") then value = value:sub(1, 1):upper() .. value:sub(2) end
		return { key, value }
	end
	return { "", tostring(line) }
end

-- The PC doctor tab's contents, as blocks for the settings window to stack:
-- a verdict card, one card per tip (with its button), then what was checked.
function DoctorUI.build(tips, report)
	local C, S = UI.C, UI.S
	local blocks = {}

	local verdict = UI.Card(nil)
	if #tips == 0 then
		verdict.badge = { "check", "green" }
		verdict:Add(UI.Text(verdict, "Your PC is set up well", "MeowyPack.Heading", C.text))
		verdict:Add(UI.Text(verdict, "Garry's Mod runs on your strongest graphics card, multicore is on and nothing is holding it back.", "MeowyPack.Small", C.body), S(4))
	else
		verdict.badge = { "alert", "amber" }
		verdict:Add(UI.Text(verdict, #tips == 1 and "1 thing to look at" or (#tips .. " things to look at"), "MeowyPack.Heading", C.text))
		verdict:Add(UI.Text(verdict, "Fixing these can make the game noticeably smoother on this PC.", "MeowyPack.Small", C.body), S(4))
	end
	blocks[#blocks + 1] = verdict

	for _, tip in ipairs(tips) do
		local card = UI.Card(nil)
		card.badge = { "alert", tip.severity == "high" and "accent" or "amber" }
		card.meowyTip = tip.id
		card:Add(UI.Text(card, tip.title, "MeowyPack.Heading", C.text))
		card:Add(UI.Text(card, tip.text, "MeowyPack.Small", C.body), S(4))
		if tip.action then
			local action = tip.action
			card:Add(UI.Button(card, tip.action_label or "Open", "primary", function() meowy.DoctorOpen(action) end), S(14))
		end
		card.gapBefore = S(10)
		blocks[#blocks + 1] = card
	end

	local label = UI.Label(nil, "What was checked")
	label.gapBefore = S(22)
	blocks[#blocks + 1] = label
	local rows = {}
	for _, line in ipairs(report) do rows[#rows + 1] = splitLine(line) end
	local details = UI.Details(nil, rows)
	details.gapBefore = S(10)
	blocks[#blocks + 1] = details

	local note = UI.Text(nil, "Read-only: nothing is changed and nothing leaves your PC.", "MeowyPack.Small", C.dim)
	note.gapBefore = S(18)
	blocks[#blocks + 1] = note
	return blocks
end

if concommand then
	concommand.Add("meowy_doctor", function()
		local tips, report = DoctorUI.diagnose()
		Msg(Doctor.consoleText(tips, report))
	end, nil, "Meowy Client Pack: check this PC for common slowdowns (read-only)")
end
