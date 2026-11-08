-- Parties' rules (src/lua/party_logic.lua), no game:
--   lua tests/party_logic_test.lua        (from the repository root; Lua 5.1+)
local here = (arg and arg[0] or ""):match("^(.*)[/\\]") or "."
local P = dofile(here .. "/../src/lua/party_logic.lua")

local failures, count = 0, 0
local function check(name, got, want)
	count = count + 1
	if got ~= want then
		failures = failures + 1
		print(("FAIL %s: got %s, want %s"):format(name, tostring(got), tostring(want)))
	end
end

-- go
local seq, address = P.parseGo("3|45.10.1.7:27015")
check("go: seq", seq, 3)
check("go: address", address, "45.10.1.7:27015")
check("go: nonsense", P.parseGo("x|1.2.3.4:1"), nil)
check("go: bad address", P.parseGo("1|1.2.3.400:1"), nil)
check("go: zero", P.parseGo("0|1.2.3.4:1"), nil)
check("go: not a string", P.parseGo(nil), nil)
check("next go: first", P.nextGo("", "1.2.3.4:27015"), "1|1.2.3.4:27015")
check("next go: after 3", P.nextGo("3|5.5.5.5:1", "1.2.3.4:27015"), "4|1.2.3.4:27015")

-- following
local base = { go = "4|45.10.1.7:27015", lastSeq = 3, leader = false, mine = "", now = 100, lastFollowAt = -1e9 }
local function with(t) local o = {} for k, v in pairs(base) do o[k] = v end for k, v in pairs(t) do o[k] = v end return o end
local to, remember = P.shouldFollow(base)
check("follow: a new go", to, "45.10.1.7:27015")
check("follow: remembered", remember, 4)
local first = with({})
first.lastSeq = nil
to, remember = P.shouldFollow(first)
check("follow: not on the first look", to, nil)
check("follow: first look remembered", remember, 4)
check("follow: seen already", (P.shouldFollow(with({ lastSeq = 4 }))), nil)
check("follow: not the leader", (P.shouldFollow(with({ leader = true }))), nil)
check("follow: already there", (P.shouldFollow(with({ mine = "45.10.1.7:27015" }))), nil)
-- A move soon after the last follow isn't dropped: it waits for the gap.
local soonTo, soonSeq, notBefore = P.shouldFollow(with({ lastFollowAt = 80 }))
check("follow: soon after the last, still followed", soonTo, "45.10.1.7:27015")
check("follow: soon after the last, seq taken", soonSeq, 4)
check("follow: soon after the last, not before the gap", notBefore, 80 + P.MIN_GAP)
check("follow: after the gap", (P.shouldFollow(with({ lastFollowAt = 70 }))), "45.10.1.7:27015")
check("follow: not where it isn't allowed", (P.shouldFollow(with({ allowed = function() return false end }))), nil)
check("follow: a broken go", (P.shouldFollow(with({ go = "garbage" }))), nil)

-- The countdown runs only while it's on screen (shown), and never ends before notBefore.
local fl = P.newFollow("45.10.1.7:27015", "Big RP", "Ann", -1e9)
check("left: the whole countdown at first", P.left(fl, 100), P.FOLLOW_SECONDS)
check("due: not before it was shown", P.due(fl, 1e6), false)
fl.shown = 7.2
check("left: what's still to show", P.left(fl, 100), 3)
fl.shown = P.FOLLOW_SECONDS
check("due: shown long enough", P.due(fl, 100), true)
check("left: never negative", P.left(fl, 100), 0)
local gap = P.newFollow("45.10.1.7:27015", "", "Ann", 125)
gap.shown = P.FOLLOW_SECONDS
check("due: shown, but the gap isn't over", P.due(gap, 110), false)
check("left: the gap's longer", P.left(gap, 110), 15)
check("due: once the gap's over", P.due(gap, 125), true)

-- model
local state = {
	lobby = "1", owner = "11", self = "22", srv = "45.10.1.7:27015", srvname = "Big \1RP",
	members = {
		{ id = "11", name = "Ann", at = "45.10.1.7:27015", atname = "Big RP", pack = "1.9.0" },
		{ id = "22", name = "", at = "", atname = "", pack = "1.9.0" },
		{ id = "33", name = "Cat", at = "nonsense", atname = "Evil", pack = "" },
	},
}
local m = P.model(state, nil, 100, { { name = "Dee", ingmod = true, pack = true }, { name = "Eve", ingmod = false } }, "hello")
check("model: party", m.party ~= nil, true)
check("model: leader name", m.party.leaderName, "Ann")
check("model: not leader", m.party.leader, false)
check("model: target cleaned", m.party.target, "Big RP")
check("model: leader where", m.party.members[1].w, "Big RP")
check("model: leader joinable", m.party.members[1].j, true)
check("model: unnamed", m.party.members[2].n, "Someone")
check("model: me", m.party.members[2].me, true)
check("model: bad address, nowhere", m.party.members[3].w, "")
check("model: no pack", m.party.members[3].k, false)
check("model: friends in GMod only", #m.friends, 1)
check("model: friend has the pack", m.friends[1].p, true)
check("model: notice", m.notice, "hello")
check("model: no party", P.model(nil, nil, 0, {}, nil).party, nil)
local f = P.model(state, { address = "45.10.1.7:27015", name = "", by = "Ann", notBefore = -1e9, shown = 5 }, 100, {}, nil)
check("model: following", f.party.follow.to .. "/" .. f.party.follow.by .. "/" .. f.party.follow.left, "45.10.1.7:27015/Ann/5")
check("escape line: following", P.escapeLine(f).text, "Following Ann to 45.10.1.7:27015 in 5 s")
check("escape line: stay", P.escapeLine(f).stay, true)
check("escape line: members", P.escapeLine(m).text, "3 members \194\183 led by Ann")
check("escape line: none", P.escapeLine(P.model(nil, nil, 0, {}, nil)), nil)

print(("%d checks, %d failed"):format(count, failures))
if failures > 0 then os.exit(1) end
