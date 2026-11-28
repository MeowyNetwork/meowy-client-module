-- The crash explainer (crash_logic.lua as `CrashLogic`): what the last crash
-- was, in plain words, what to try, and a text to copy when asking for help.
--
--   meowy.CrashLast()            the last crash's reading (crash_logic.lua's
--                                M.explain) with .time, or nil when there's none
--   meowy.CrashExplain(opts)     its window. opts.rejoin: a function for a
--                                "Rejoin" button; opts.after: run when it closes
--   meowy.CrashUnseen()          whether the last crash hasn't been shown yet
--   meowy_crash_status           the same in the console
--
-- Read from Garry's Mod's own crash report on this PC (meowy.LastCrash), on any
-- server or none. Nothing is sent: the text is for the player to copy. A crash
-- is offered once (crash_seen.txt keeps the last one shown); after that it's
-- in the PC doctor and the console command.
--
-- Runs in its own function at the end of resilience.lua's chunk (module.cpp),
-- with `CrashLogic` and resilience.lua's locals in scope: UI, log.
local Logic = CrashLogic
local S, C = UI.S, UI.C

local MAX_AGE = 14 * 86400   -- an older crash isn't news

local cached, cachedAt
local function last()
	-- Asking reads the crashes folder and a dump: at most every few seconds.
	local now = SysTime()
	if cachedAt and now - cachedAt < 5 then return cached or nil end
	cachedAt, cached = now, false
	if not meowy.LastCrash then return nil end
	local ok, r = pcall(meowy.LastCrash, MAX_AGE)
	if ok and istable(r) then cached = r end
	return cached or nil
end

local function facts(report)
	local hooksOn
	for _, e in ipairs(meowy.HookStatus and meowy.HookStatus() or {}) do
		if e.state == "on" or e.state == "waiting" then hooksOn = true elseif hooksOn == nil then hooksOn = false end
	end
	local server
	if meowy.ResilienceCrashedSession then
		local ok, address, name = pcall(meowy.ResilienceCrashedSession)
		if ok and isstring(address) and address ~= "" then
			server = (isstring(name) and name ~= "" and (name .. " | ") or "") .. address
		end
	end
	return {
		bits = report.bits == 64 and 64 or (report.bits == 32 and 32 or (jit and jit.arch == "x64" and 64 or 32)),
		pack = meowy.VERSION,
		game = meowy.GameBuild and meowy.GameBuild() or nil,
		multicore = meowy.MulticoreStatus and meowy.MulticoreStatus() == "managed" or false,
		hooks_on = hooksOn,
		server = server,
	}
end

function meowy.CrashLast()
	local r = last()
	if not r then return nil end
	local e = Logic.explain(r, facts(r))
	e.time = r.time
	return e
end

local function seenTime()
	return tonumber(meowy.MenuFileRead and meowy.MenuFileRead("crash_seen") or "") or 0
end
local function markSeen(t)
	if meowy.MenuFileWrite and (tonumber(t) or 0) > seenTime() then meowy.MenuFileWrite("crash_seen", tostring(math.floor(t))) end
end
-- maxAge (seconds): only a crash that recent counts (the start-up offer is
-- for the crash that just happened, not one from last week).
function meowy.CrashUnseen(maxAge)
	local r = last()
	if not r or (tonumber(r.time) or 0) <= seenTime() then return false end
	return maxAge == nil or os.time() - r.time <= maxAge
end
-- The last crash was offered some other way (the rejoin card's button).
function meowy.CrashMarkSeen()
	local r = last()
	if r then markSeen(r.time) end
end

function meowy.CrashExplain(opts)
	opts = opts or {}
	local e = meowy.CrashLast()
	if not e then
		if opts.after then opts.after() end
		return nil
	end
	markSeen(e.time)
	local finished = false
	local function finish()
		if finished then return end
		finished = true
		if opts.after then opts.after() end
	end
	local back, card = UI.Modal(640, finish)
	back.meowyCrash = true
	local pad = S(28)
	local inner = card.w - pad * 2
	local blocks = { UI.Text(card, e.title, "MeowyPack.Title", C.text) }
	local when = UI.Text(card, "The last crash, " .. Logic.ago(os.time(), e.time) .. ". This is a reading of Garry's Mod's own "
		.. "crash report on this PC; nothing is sent.", "MeowyPack.Small", C.dim)
	when.gapBefore = S(6)
	blocks[#blocks + 1] = when
	local what = UI.Text(card, e.what, "MeowyPack.Body", C.body)
	what.gapBefore = S(16)
	blocks[#blocks + 1] = what
	local cause = UI.Text(card, e.cause, "MeowyPack.Body", C.text)
	cause.gapBefore = S(10)
	cause.meowyCrashCause = true
	blocks[#blocks + 1] = cause
	if #e.advice > 0 then
		local label = UI.Label(card, "What to try")
		label.gapBefore = S(18)
		blocks[#blocks + 1] = label
		for i, line in ipairs(e.advice) do
			local t = UI.Text(card, i .. ". " .. line, "MeowyPack.Body", C.body)
			t.gapBefore = S(i == 1 and 8 or 6)
			blocks[#blocks + 1] = t
		end
	end
	local y = UI.stack(blocks, pad, pad, inner)
	local x = UI.Close(card, function() back:Dismiss() end)
	x:SetPos(card.w - S(16) - x.w, S(16))
	local function close(fn)
		back.dismissed = true   -- answered: not a dismissal
		back:Remove()
		if fn then fn() end
		finish()
	end
	local copy
	copy = UI.Button(card, "Copy report", "ghost", function()
		SetClipboardText(e.share)
		copy:SetText("Copied")
	end)
	copy.meowyCrashCopy = true
	local buttons = { copy }
	if opts.rejoin then
		buttons[#buttons + 1] = UI.Button(card, "Rejoin", "primary", function() close(opts.rejoin) end)
	else
		buttons[#buttons + 1] = UI.Button(card, "Close", "primary", function() close() end)
	end
	y = y + S(26)
	y = y + UI.row(buttons, pad, y, inner) + pad
	back:Open(y)
	return back
end

concommand.Add("meowy_crash_status", function()
	local e = meowy.CrashLast()
	if not e then
		print("[Meowy Client Pack] crash: Garry's Mod wrote no crash report in the last two weeks")
		return
	end
	print("[Meowy Client Pack] the last crash, " .. Logic.ago(os.time(), e.time) .. ":")
	print("  " .. e.what)
	print("  " .. e.cause)
	for i, line in ipairs(e.advice) do print("  " .. i .. ". " .. line) end
	print("  --- to copy when asking for help ---")
	for line in string.gmatch(e.share .. "\n", "([^\n]*)\n") do print("  " .. line) end
end)
