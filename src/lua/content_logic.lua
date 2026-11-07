-- The content platform's rules (docs/CONTENT_PLATFORM.md), as pure functions
-- (no GMod API), so tests/content_logic_test.lua can run them with a plain Lua
-- interpreter. Lua 5.1 compatible: Garry's Mod runs LuaJIT.
--
-- A server's advert is its `meowy_content` rule in A2S_RULES (content_advert.h):
--   "1;ws=<id>,<id>,~<id>"
-- the Workshop items its join will ask for ("~": deferred, downloaded after
-- spawn by the server's own Lua; prepared here like the rest). It's the
-- server's own text: only decimal IDs are taken from it, and all it can make
-- the pack do is download Workshop items, which the join then checks itself.
local M = {}

M.MAX_ITEMS = 200                       -- items taken from one advert
M.MAX_VALUE = 4096                      -- bytes of an advert (content_advert.h)
M.CAP = 4 * 1024 * 1024 * 1024          -- bytes a server may need before "Get ready" is always asked
M.TTL = 600                             -- seconds an advert's answer is reused

local UINT64_MAX = "18446744073709551615"

-- A Workshop ID as a decimal string: 1 to 20 digits, no leading zero, at most 2^64-1.
function M.validId(id)
	if type(id) ~= "string" or not string.match(id, "^[1-9]%d*$") or #id > 20 then return false end
	return #id < 20 or id <= UINT64_MAX
end

-- An advert's value -> { ids = { "<id>", ... }, deferred = { ["<id>"] = true } },
-- or nil, reason. Fields are ';'-separated "key=value"; unknown keys are left
-- for later versions, entries that aren't IDs are skipped, duplicates count once.
function M.parseAdvert(value)
	if type(value) ~= "string" or value == "" then return nil, "empty" end
	if #value > M.MAX_VALUE then return nil, "too long" end
	local version, rest = string.match(value, "^(%d+)(.*)$")
	if version ~= "1" then return nil, "not version 1" end
	if rest ~= "" and string.sub(rest, 1, 1) ~= ";" then return nil, "not version 1" end
	local out, seen = { ids = {}, deferred = {} }, {}
	for field in string.gmatch(rest, "[^;]+") do
		local key, list = string.match(field, "^(%a+)=(.*)$")
		if key == "ws" then
			for item in string.gmatch(list, "[^,]+") do
				local tilde, id = string.match(item, "^(~?)(%d+)$")
				if id and M.validId(id) and not seen[id] and #out.ids < M.MAX_ITEMS then
					seen[id] = true
					out.ids[#out.ids + 1] = id
					if tilde == "~" then out.deferred[id] = true end
				end
			end
		end
	end
	if #out.ids == 0 then return nil, "no Workshop items" end
	return out
end

-- The lists' union, each ID once: `a`'s order first, then what only `b` has.
function M.merge(a, b)
	local out, seen = {}, {}
	for _, list in ipairs({ a or {}, b or {} }) do
		for _, id in ipairs(list) do
			id = tostring(id)
			if M.validId(id) and not seen[id] then
				seen[id] = true
				out[#out + 1] = id
			end
		end
	end
	return out
end

-- Whether an item needs downloading or unpacking before a join, from what
-- Steam says (installed, needsUpdate, busy, legacy) and whether a legacy
-- item's unpacked copy (cache/workshop/<id>.gma) is there. nil: Steam can't
-- say (installed is nil), so it's left to the join.
function M.needs(installed, needsUpdate, busy, legacy, unpacked)
	if installed == nil then return nil end
	if not installed or needsUpdate or busy then return true end
	if legacy and unpacked == false then return true end
	return false
end

-- Whether the Workshop's version (steamworks.FileInfo's `updated`) is newer
-- than the installed one. FileInfo's time comes through a float: today's unix
-- times are rounded to 128 s either way, so newer means newer by more than `slack`.
function M.newer(remote, installed, slack)
	remote, installed = tonumber(remote), tonumber(installed) or 0
	return remote ~= nil and remote > installed + (tonumber(slack) or 256)
end

-- May a server's content be downloaded now? t = { ours, known (joined before),
-- bytes (what it needs), asked ("Get ready" clicked) }. "go" or "ask".
function M.consent(t)
	if t.asked then return "go" end
	if (tonumber(t.bytes) or 0) > M.CAP then return "ask" end
	if t.ours or t.known then return "go" end
	return "ask"
end

-- "8 MB", "412 MB", "1.3 GB" (1024-based, as Garry's Mod's own download box).
function M.size(bytes)
	bytes = tonumber(bytes) or 0
	if bytes ~= bytes or bytes <= 0 then return "0 MB" end
	local mb = bytes / 1048576
	if mb < 1000 then return math.max(1, math.ceil(mb)) .. " MB" end
	return string.format("%.1f GB", mb / 1024)
end

-- A server's row: what its content plan shows, and whether it's the button.
-- plan = { state = "checking" | "ask" | "going" | "ready" | "stopped",
--          bytes (needed), done (bytes so far), items (needing work) }.
-- nil when there's nothing worth a line (checking, nothing needed, stopped).
function M.label(plan)
	if type(plan) ~= "table" then return nil end
	local items = tonumber(plan.items) or 0
	if plan.state == "ask" and items > 0 then
		return "Get ready \194\183 " .. M.size(plan.bytes), true
	elseif plan.state == "going" then
		local bytes = tonumber(plan.bytes) or 0
		local done = math.min(tonumber(plan.done) or 0, bytes)
		return "Getting ready \194\183 " .. M.size(done) .. " of " .. M.size(bytes), false
	elseif plan.state == "ready" and items > 0 then
		return "Ready \194\183 content downloaded", false
	end
	return nil
end

return M
