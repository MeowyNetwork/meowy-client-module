-- PC doctor's rules (src/lua/doctor_logic.lua), no game:
--   lua tests/doctor_logic_test.lua        (from the repository root; Lua 5.1+)
local here = (arg and arg[0] or ""):match("^(.*)[/\\]") or "."
local M = dofile(here .. "/../src/lua/doctor_logic.lua")

local failures, count = 0, 0
local function check(name, got, want)
	count = count + 1
	if got ~= want then
		failures = failures + 1
		print(("FAIL %s: got %s, want %s"):format(name, tostring(got), tostring(want)))
	end
end
local function ids(tips)
	local t = {}
	for _, tip in ipairs(tips) do t[#t + 1] = tip.id end
	return table.concat(t, ",")
end

-- The hybrid laptop this was built on (doctor_smoke output), plugged in, game mode.
local RTX = { name = "NVIDIA GeForce RTX 3070 Ti Laptop GPU", vram_mb = 8017, perf_rank = 1, power_rank = 2 }
local IGPU = { name = "AMD Radeon(TM) Graphics", vram_mb = 485, perf_rank = 2, power_rank = 1 }
local function laptop(rtxShare, igpuShare, extra)
	local f = { arch = "x64", gpu_time_known = true, on_battery = false, battery_saver = false, power_mode = "game_mode",
		adapters = {
			{ name = RTX.name, vram_mb = RTX.vram_mb, perf_rank = 1, power_rank = 2, share = rtxShare },
			{ name = IGPU.name, vram_mb = IGPU.vram_mb, perf_rank = 2, power_rank = 1, share = igpuShare },
		} }
	for k, v in pairs(extra or {}) do f[k] = v end
	return f
end
local GOOD = { mcore = "1", queue = "-1" }

-- GPU ------------------------------------------------------------------------
local tips, report = M.diagnose(laptop(1, 0), GOOD)
check("on the dGPU: no tips", ids(tips), "")
check("summary none", M.summary(tips), "No tips")
check("report names the drawing GPU", report[1], "GPU: NVIDIA GeForce RTX 3070 Ti Laptop GPU (8017 MB), drawing the game (100% of its GPU time)")

tips = M.diagnose(laptop(0, 1), GOOD)
check("on the iGPU: gpu tip", ids(tips), "gpu")
check("gpu tip title", tips[1].title, "Garry's Mod is using AMD Radeon(TM) Graphics. You have NVIDIA GeForce RTX 3070 Ti Laptop GPU.")
check("gpu tip opens Graphics settings", tips[1].action, "graphics_settings")
check("gpu tip matters most", tips[1].severity, "high")
check("64-bit exe path", tips[1].text:find("bin\\win64\\gmod.exe", 1, true) ~= nil, true)
tips = M.diagnose(laptop(0, 1, { arch = "x86" }), GOOD)
check("32-bit exe path", tips[1].text:find("GarrysMod\\hl2.exe", 1, true) ~= nil, true)

tips, report = M.diagnose(laptop(0.3, 0.3), GOOD)
check("no clear winner: no gpu tip", ids(tips), "")
check("no clear winner: says so", report[3], "Couldn't tell which GPU draws the game.")

tips = M.diagnose(laptop(0, 1, { gpu_time_known = false }), GOOD)
check("GPU time unknown: no gpu tip", ids(tips), "")

-- Two discrete GPUs of similar strength, game on the second: not a tip
tips = M.diagnose({ arch = "x64", gpu_time_known = true, adapters = {
	{ name = "A", vram_mb = 8000, perf_rank = 1, power_rank = 1, share = 0 },
	{ name = "B", vram_mb = 6000, perf_rank = 2, power_rank = 2, share = 1 },
} }, GOOD)
check("similar discrete GPUs: no tip", ids(tips), "")

-- No DXGI 1.6 ranks: falls back to video memory
tips = M.diagnose({ arch = "x64", gpu_time_known = true, adapters = {
	{ name = "Intel(R) UHD Graphics 620", vram_mb = 128, perf_rank = 0, power_rank = 0, share = 0.97 },
	{ name = "NVIDIA GeForce MX150", vram_mb = 2048, perf_rank = 0, power_rank = 0, share = 0.03 },
} }, GOOD)
check("no ranks, iGPU by memory: tip", ids(tips), "gpu")

tips = M.diagnose({ arch = "x64", gpu_time_known = true, adapters = { { name = "Only", vram_mb = 4096, perf_rank = 1, power_rank = 1, share = 1 } } }, GOOD)
check("single GPU: no tip", ids(tips), "")

-- Power -------------------------------------------------------------------------
check("on battery", ids((M.diagnose(laptop(1, 0, { on_battery = true }), GOOD))), "battery")
check("battery is a medium tip", (M.diagnose(laptop(1, 0, { on_battery = true }), GOOD))[1].severity, "medium")
check("battery saver", ids((M.diagnose(laptop(1, 0, { battery_saver = true }), GOOD))), "battery_saver")
check("saver mode counts once", ids((M.diagnose(laptop(1, 0, { battery_saver = true, power_mode = "battery_saver" }), GOOD))), "battery_saver")
check("best power efficiency", ids((M.diagnose(laptop(1, 0, { power_mode = "better_battery" }), GOOD))), "power_mode")
check("balanced is fine", ids((M.diagnose(laptop(1, 0, { power_mode = "balanced" }), GOOD))), "")
_, report = M.diagnose(laptop(1, 0, { power_mode = "high_performance" }), GOOD)
local hasMode = false
for _, l in ipairs(report) do if l == "Power mode: high performance" then hasMode = true end end
check("power mode reported", hasMode, true)

-- Multicore -----------------------------------------------------------------------
check("mcore off", ids((M.diagnose(laptop(1, 0), { mcore = "0", queue = "0" }))), "mcore")
check("queue 0 with mcore on", ids((M.diagnose(laptop(1, 0), { mcore = "1", queue = "0" }))), "queue")
check("queue 2 fine", ids((M.diagnose(laptop(1, 0), { mcore = "1", queue = "2" }))), "")
_, report = M.diagnose(laptop(1, 0), {})
local mc = false
for _, l in ipairs(report) do if l == "Multicore: gmod_mcore_test unknown, mat_queue_mode unknown" then mc = true end end
do
	local _, r = M.diagnose({ arch = "x64", adapters = {} }, { mcore = "1", queue = "-1.00" })
	local plain = false
	for _, l in ipairs(r) do if l == "Multicore: gmod_mcore_test 1, mat_queue_mode -1" then plain = true end end
	check("cvars shown as typed", plain, true)
end
check("cvars unknown: reported, no tip", mc, true)

-- Branch ----------------------------------------------------------------------------
tips = M.diagnose(laptop(1, 0, { arch = "x86" }), GOOD)
check("32-bit: branch tip", ids(tips), "branch")
check("branch tip opens the guide", tips[1].action, "branch_guide")

-- Everything at once, and the summary
tips, report = M.diagnose(laptop(0, 1, { arch = "x86", on_battery = true, power_mode = "better_battery" }), { mcore = "0", queue = "0" })
check("all tips", ids(tips), "gpu,battery,power_mode,mcore,branch")
check("summary", M.summary(tips), "5 tips")
check("summary one", M.summary({ {} }), "1 tip")
local text = M.consoleText(tips, report)
check("console text header", text:match("^[^\n]+"), "[Meowy Client Pack] PC doctor: 5 tips")
check("console text has the settings page", text:find("ms-settings:display-advancedgraphics", 1, true) ~= nil, true)

-- Sound and the microphone ----------------------------------------------------------
local function withAudio(audio) return laptop(1, 0, { audio = audio }) end
local function has(lines, want) for _, l in ipairs(lines) do if l == want then return true end end return false end
tips, report = M.diagnose(withAudio({ output = "Speakers (Realtek)", input = "Mic (USB)", comms_input = "Mic (USB)", inputs = 1 }), GOOD)
check("sound: a normal setup, no tip", ids(tips), "")
check("sound: output reported", has(report, "Sound: Speakers (Realtek)"), true)
check("sound: mic reported", has(report, "Microphone: Mic (USB)"), true)
check("sound: one mic, no calls line", has(report, "Calls microphone: Mic (USB)"), false)
tips = M.diagnose(withAudio({ output = "Headphones (WH-1000XM4 Stereo)", input = "Headset (WH-1000XM4 Hands-Free AG Audio)",
	comms_input = "Headset (WH-1000XM4 Hands-Free AG Audio)", input_handsfree = true, inputs = 1 }), GOOD)
check("sound: a hands-free mic gets a tip", ids(tips), "mic_handsfree")
check("sound: it opens Sound settings", tips[1] and tips[1].action, "sound_settings")
tips, report = M.diagnose(withAudio({ output = "Speakers", input = "Mic (USB)", comms_input = "Webcam mic", inputs = 2 }), GOOD)
check("sound: two default mics get a tip", ids(tips), "mic_two")
check("sound: it names both", tips[1] ~= nil and tips[1].text:find("Mic (USB)", 1, true) ~= nil and tips[1].text:find("Webcam mic", 1, true) ~= nil, true)
check("sound: it opens Steam's voice settings", tips[1] and tips[1].action, "steam_voice")
check("sound: calls mic reported", has(report, "Calls microphone: Webcam mic"), true)
tips, report = M.diagnose(withAudio({ output = "", input = "", comms_input = "", inputs = 0 }), GOOD)
check("sound: no mic at all is reported, not a tip", ids(tips) .. "|" .. tostring(has(report, "Microphone: none found")), "|true")
check("sound: no output reported", has(report, "Sound: no output device"), true)
check("sound: console text points at Steam", M.consoleText((M.diagnose(withAudio({ input = "A", comms_input = "B" }), GOOD)), {}):find("Steam: Settings, Voice", 1, true) ~= nil, true)
check("sound: no audio facts, no sound lines", has(select(2, M.diagnose(laptop(1, 0), GOOD)), "Microphone: none found"), false)

-- Garbage in: no crash, no tips
check("nil facts", ids((M.diagnose(nil, nil))), "")
check("junk adapters", ids((M.diagnose({ gpu_time_known = true, adapters = { 1, "x", { share = "1" } } }, nil))), "")

print(("%d checks, %d failed"):format(count, failures))
os.exit(failures == 0 and 0 or 1)
