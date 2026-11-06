-- PC doctor: what to tell the player, as pure functions (no GMod API), so it
-- can be tested with a plain Lua interpreter (tests/doctor_logic_test.lua).
-- Lua 5.1 compatible: Garry's Mod runs LuaJIT.
--
-- facts: meowy.DoctorFacts() (doctor.cpp): { arch, gpu_time_known, on_battery,
--   battery_saver, power_mode, adapters = { { name, vram_mb, perf_rank,
--   power_rank, share } } }. Any field may be missing.
-- cvars: { mcore = gmod_mcore_test, queue = mat_queue_mode } as strings, or nil.
-- Everything stays on this PC. Nothing is changed: tips say what to do.

local M = {}

M.MIN_SHARE = 0.5   -- the adapter with at least this much of the game's GPU time draws it

-- How much each tip matters: "high" (a big, fixable slowdown or crashes) or "medium".
M.SEVERITY = { gpu = "high", mcore = "high", branch = "high", queue = "medium", battery = "medium", battery_saver = "medium", power_mode = "medium" }

local function str(v) return type(v) == "string" and v or "" end

-- The adapter drawing the game: most of this process's GPU time, if known.
function M.inUse(facts)
	if type(facts) ~= "table" or facts.gpu_time_known ~= true or type(facts.adapters) ~= "table" then return nil end
	local best
	for _, a in ipairs(facts.adapters) do
		if type(a) == "table" and type(a.share) == "number" and a.share >= M.MIN_SHARE and (not best or a.share > best.share) then
			best = a
		end
	end
	return best
end

-- The strongest adapter: Windows' first "high performance" choice, else the
-- one with the most video memory.
function M.strongest(facts)
	if type(facts) ~= "table" or type(facts.adapters) ~= "table" then return nil end
	local best
	for _, a in ipairs(facts.adapters) do
		if type(a) == "table" and a.perf_rank == 1 then return a end
		if type(a) == "table" and (not best or (tonumber(a.vram_mb) or 0) > (tonumber(best.vram_mb) or 0)) then best = a end
	end
	return best
end

local function gameExe(arch)
	if arch == "x86" then return "GarrysMod\\hl2.exe" end
	return "GarrysMod\\bin\\win64\\gmod.exe"
end

M.BRANCH_STEPS = "In Steam: Library, right-click Garry's Mod, Properties, Betas, and pick "
	.. "\"x86-64 - Chromium + 64-bit binaries\"."

-- Returns tips (each { id, title, text, action?, action_label? }) and report
-- lines (what was found, tip or not).
function M.diagnose(facts, cvars)
	facts = type(facts) == "table" and facts or {}
	cvars = type(cvars) == "table" and cvars or {}
	local tips, report = {}, {}

	-- GPU
	local adapters = type(facts.adapters) == "table" and facts.adapters or {}
	local used, strong = M.inUse(facts), M.strongest(facts)
	for _, a in ipairs(adapters) do
		if type(a) == "table" then
			local line = ("GPU: %s (%d MB)"):format(str(a.name), tonumber(a.vram_mb) or 0)
			if a == used then line = line .. (", drawing the game (%d%% of its GPU time)"):format(math.floor(a.share * 100 + 0.5)) end
			report[#report + 1] = line
		end
	end
	if #adapters > 1 and not used then
		report[#report + 1] = "Couldn't tell which GPU draws the game."
	end
	if used and strong and used ~= strong then
		local usedVram, strongVram = tonumber(used.vram_mb) or 0, tonumber(strong.vram_mb) or 0
		local weaker = used.power_rank == 1 or strongVram >= 2 * math.max(usedVram, 1)
		if weaker and strongVram > usedVram then
			tips[#tips + 1] = {
				id = "gpu",
				title = ("Garry's Mod is using %s. You have %s."):format(str(used.name), str(strong.name)),
				text = ("Open Windows' Graphics settings, add Garry's Mod (Browse: steamapps\\common\\%s), "
					.. "choose Options, pick High performance (%s), Save, and restart Garry's Mod."):format(gameExe(facts.arch), str(strong.name)),
				action = "graphics_settings",
				action_label = "Open Graphics settings",
			}
		end
	end

	-- Power
	local saverTip = false
	if facts.on_battery == true then
		tips[#tips + 1] = { id = "battery", title = "You're running on battery.",
			text = "Laptops slow down on battery. Plug in for full speed." }
		report[#report + 1] = "Power: on battery"
	elseif facts.on_battery == false then
		report[#report + 1] = "Power: plugged in"
	end
	if facts.battery_saver == true or facts.power_mode == "battery_saver" then
		saverTip = true
		tips[#tips + 1] = { id = "battery_saver", title = "Battery saver is on.",
			text = "It holds the game back. Turn it off in Windows' quick settings, or in Settings, System, Power & battery." }
	end
	if not saverTip and facts.power_mode == "better_battery" then
		tips[#tips + 1] = { id = "power_mode", title = "Windows is set to save power.",
			text = "In Settings, System, Power & battery, set Power mode to Best performance (or Balanced)." }
	end
	if type(facts.power_mode) == "string" then report[#report + 1] = "Power mode: " .. facts.power_mode:gsub("_", " ") end

	-- Multicore
	local mcore, queue = tonumber(cvars.mcore), tonumber(cvars.queue)
	report[#report + 1] = ("Multicore: gmod_mcore_test %s, mat_queue_mode %s"):format(cvars.mcore or "unknown", cvars.queue or "unknown")
	if mcore == 0 then
		tips[#tips + 1] = { id = "mcore", title = "Multicore rendering is off.",
			text = "Type gmod_mcore_test 1 in the console, then rejoin. It's much faster on most PCs. If the game "
				.. "gets unstable, set it back to 0." }
	elseif mcore == 1 and queue == 0 then
		tips[#tips + 1] = { id = "queue", title = "mat_queue_mode 0 turns multicore rendering off.",
			text = "Type mat_queue_mode -1 in the console to let the game decide." }
	end

	-- Branch
	if facts.arch == "x86" then
		tips[#tips + 1] = { id = "branch", title = "You're on the 32-bit game.",
			text = "It crashes when it runs out of memory on big maps. Switch to the 64-bit one: " .. M.BRANCH_STEPS,
			action = "branch_guide", action_label = "How to switch branches" }
		report[#report + 1] = "Game: 32-bit"
	elseif facts.arch == "x64" then
		report[#report + 1] = "Game: 64-bit"
	end

	for _, t in ipairs(tips) do t.severity = M.SEVERITY[t.id] or "medium" end
	return tips, report
end

function M.summary(tips)
	local n = type(tips) == "table" and #tips or 0
	if n == 0 then return "No tips" end
	return n == 1 and "1 tip" or (n .. " tips")
end

-- What meowy_doctor prints.
function M.consoleText(tips, report)
	local out = { "[Meowy Client Pack] PC doctor: " .. M.summary(tips) }
	for i, t in ipairs(tips or {}) do
		out[#out + 1] = ("  %d. %s %s"):format(i, t.title, t.text)
		if t.action == "graphics_settings" then out[#out + 1] = "     (Settings: ms-settings:display-advancedgraphics)" end
	end
	for _, line in ipairs(report or {}) do out[#out + 1] = "  - " .. line end
	return table.concat(out, "\n") .. "\n"
end

return M
