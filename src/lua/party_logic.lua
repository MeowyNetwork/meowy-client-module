-- Parties' rules (roadmap MR3, docs/server-index-and-parties.md), no game:
-- tests/party_logic_test.lua. party.lua has it as `PartyLogic`.
--
-- A party is a friends-only Steam lobby (party_steam.h). Its owner, the
-- leader, moves everyone by bumping the lobby's `go` ("<seq>|<address>");
-- each member's pack then counts down FOLLOW_SECONDS with a Stay button
-- before joining, and never follows more than once every MIN_GAP seconds, so
-- a party can't bounce anyone around. Members publish where they are (`at`,
-- `atname`) so everyone sees the party's servers.
local M = {}

M.FOLLOW_SECONDS = 10
M.MIN_GAP = 30
M.MAX_MEMBERS = 8

local function validAddress(a)
	if type(a) ~= "string" or #a > 21 then return false end
	local p1, p2, p3, p4, port = a:match("^(%d+)%.(%d+)%.(%d+)%.(%d+):(%d+)$")
	if not p1 then return false end
	for _, p in ipairs({ p1, p2, p3, p4 }) do if tonumber(p) > 255 then return false end end
	port = tonumber(port)
	return port >= 1 and port <= 65535
end
M.validAddress = validAddress

local function clean(s, max)
	if type(s) ~= "string" then return "" end
	s = s:gsub("[%c]", ""):gsub("^%s+", ""):gsub("%s+$", "")
	return #s > max and s:sub(1, max) or s
end
M.clean = clean

-- "<seq>|<address>" -> seq, address; nil for anything else.
function M.parseGo(text)
	if type(text) ~= "string" or #text > 40 then return nil end
	local seq, address = text:match("^(%d+)|(.+)$")
	seq = tonumber(seq)
	if not seq or seq < 1 or seq > 1e9 or not validAddress(address) then return nil end
	return seq, address
end

-- The leader's next `go` for `address`.
function M.nextGo(current, address)
	local seq = M.parseGo(current) or 0
	return (seq + 1) .. "|" .. address
end

-- Whether a member starts following. f = { go (the lobby's text), lastSeq
-- (the last one this member saw, nil before the first look), leader (this
-- member leads), mine (the server this member is on, "" in the menu), now,
-- lastFollowAt, allowed (address -> bool: public, not blacklisted) }.
-- Returns address or nil, and the seq to remember.
function M.shouldFollow(f)
	local seq, address = M.parseGo(f.go)
	if not seq then return nil, f.lastSeq end
	-- The first look at a party (just joined): what was asked before isn't for us.
	if f.lastSeq == nil then return nil, seq end
	if seq <= f.lastSeq then return nil, f.lastSeq end
	if f.leader or address == f.mine then return nil, seq end
	if f.allowed and not f.allowed(address) then return nil, seq end
	if f.now - (f.lastFollowAt or -math.huge) < M.MIN_GAP then return nil, seq end
	return address, seq
end

-- Seconds left on a countdown (never below 0).
function M.left(follow, now)
	return math.max(0, math.ceil(follow.at - now))
end

-- What the home screen's party card and the Escape menu's card show.
-- state = party_steam's (PartyState), friends = PartyFriends' list, follow =
-- { address, name, by, at } or nil. Everything here is text the page shows as text.
function M.model(state, follow, now, friends, notice)
	local out = { notice = clean(notice, 120) }
	if state then
		local leaderName, members = "", {}
		for i, m in ipairs(state.members or {}) do
			if i > M.MAX_MEMBERS * 2 then break end
			local lead = m.id == state.owner
			local name = clean(m.name, 32)
			if name == "" then name = "Someone" end
			if lead then leaderName = name end
			local where = validAddress(m.at) and (clean(m.atname, 64) ~= "" and clean(m.atname, 64) or m.at) or ""
			members[#members + 1] = { n = name, w = where, me = m.id == state.self, l = lead, k = m.pack ~= nil and m.pack ~= "",
				j = validAddress(m.at) and m.id ~= state.self }
		end
		local target = validAddress(state.srv) and (clean(state.srvname, 64) ~= "" and clean(state.srvname, 64) or state.srv) or ""
		out.party = { leader = state.owner == state.self, leaderName = leaderName, members = members, target = target }
		if follow then
			out.party.follow = { by = clean(follow.by, 32), to = clean(follow.name, 64) ~= "" and clean(follow.name, 64) or follow.address,
				left = M.left(follow, now) }
		end
	end
	out.friends = {}
	for _, f in ipairs(friends or {}) do
		if #out.friends >= 8 then break end
		if f.ingmod then out.friends[#out.friends + 1] = { n = clean(f.name, 32) ~= "" and clean(f.name, 32) or "Someone", p = f.pack == true } end
	end
	return out
end

-- The Escape menu's party line, or nil.
function M.escapeLine(model)
	local p = model and model.party
	if not p then return nil end
	if p.follow then
		return { text = "Following " .. p.follow.by .. " to " .. p.follow.to .. " in " .. p.follow.left .. " s", stay = true }
	end
	return { text = #p.members .. (#p.members == 1 and " member" or " members") .. (p.leader and " \194\183 you lead" or " \194\183 led by " .. p.leaderName), stay = false }
end

return M
