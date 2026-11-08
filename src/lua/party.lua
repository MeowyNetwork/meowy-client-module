-- Parties (roadmap MR3, docs/server-index-and-parties.md): friends who move
-- between servers together. Runs in its own function at the end of
-- resilience.lua's chunk (module.cpp), after menu_page.lua, with PartyLogic
-- (party_logic.lua) and resilience.lua's locals in scope: log, join, Home.
--
-- The party is a friends-only Steam lobby (party_steam.h): any friend can
-- join it from Steam's friends list or an invite, and Steam shows the members
-- together (Rich Presence steam_player_group). The leader's "Take the party
-- here" (Find servers) moves everyone: each member's pack counts down
-- PartyLogic.FOLLOW_SECONDS with Stay (the home screen's card, the Escape
-- menu's card, a banner in game, meowy_party_stay), then joins. Nothing is
-- followed on the first look at a party, more than once every MIN_GAP
-- seconds, or to a server Garry's Mod's blacklist names.
--
-- The page (shell.js, browser.js) sees window.MeowyParty, a model of text
-- only, and asks through meowy.post (menu_page.lua's handlers call
-- MeowyClientPackParty). Friends and members are picked by number.
local setting = meowy.Setting and meowy.Setting("meowy_party", "1",
	"Meowy Client Pack: parties with Steam friends that move between servers together (0 = off)")
local function partyOn() return meowy.PartyAvailable ~= nil and (setting == nil or setting:GetBool()) end

local LOOK = 0.5          -- seconds between looks at the party
local FRIENDS_LOOK = 10   -- seconds between looks at the friends list
local P = {
	state = nil, lastSeq = nil, follow = nil, lastFollowAt = -math.huge, notice = nil, noticeUntil = 0,
	friends = {}, friendsRaw = {}, friendsAt = -math.huge, mine = nil, presence = nil, sentModel = nil,
	nextLook = 0, warmed = nil, started = false, packSet = nil,
}

local function say(text, seconds)
	P.notice, P.noticeUntil = text, SysTime() + (seconds or 8)
	log("party: %s", text)
end

local function blacklisted(address)
	if not isfunction(IsServerBlacklisted) then return false end
	local ok, hit = pcall(IsServerBlacklisted, address, "", "", "", "")
	return ok and hit ~= nil
end
local function allowed(address) return Home.publicAddress(address) and not blacklisted(address) end

-- The server this player is on ("" in the menu or while loading), and its name.
local function whereAmI()
	if not IsInGame() or IsInLoading() or not meowy.ServerNow then return "", "" end
	local address = meowy.ServerNow()
	if not Home.validAddress(address) then return "", "" end
	return address, Home.clean(meowy.JoiningName or "")
end

local function pushModel(now)
	if not (IsValid(pnlMainMenu) and pnlMainMenu.Call) then return end
	local model = PartyLogic.model(P.state, P.follow, now, P.friendsRaw, now < P.noticeUntil and P.notice or nil)
	model.on = partyOn()
	local text = Home.encode(model)
	if text == P.sentModel then return end
	P.sentModel = text
	pnlMainMenu:Call("window.MeowyParty = " .. text .. ";\nif (window.MeowyShellParty) window.MeowyShellParty();\nif (window.MeowyBrowserParty) window.MeowyBrowserParty();\n")
end

local function leave(why)
	if meowy.PartyLeave then meowy.PartyLeave() end
	P.state, P.lastSeq, P.follow, P.mine, P.presence = nil, nil, nil, nil, nil
	if why then say(why) end
end

local function onEvent(e, now)
	if e == "created" then
		say("Party started: invite your friends")
		P.lastSeq, P.mine = nil, nil
	elseif e == "joined" then
		say("You joined the party")
		P.lastSeq, P.mine = nil, nil
	elseif e:sub(1, 7) == "failed:" then
		say(e:sub(9))
	elseif e:sub(1, 7) == "invite " then
		-- An invite accepted in Steam (or the game started from one): the player's own click.
		if not partyOn() then return log("party: invite ignored, parties are off") end
		if P.state then leave() end
		if not meowy.PartyJoin(e:sub(8)) then say("Couldn't join that party") end
	end
end

local function tick()
	local now = SysTime()
	if now < P.nextLook then return end
	P.nextLook = now + LOOK
	if not meowy.PartyPoll then return end
	if not P.started then
		P.started = true
		if meowy.PartyAvailable() then meowy.PartyStart() end
	end
	-- The game may clear Rich Presence (on a join, say): the pack's keys go back each minute.
	if now - (P.presenceAt or -math.huge) >= 60 then
		P.presenceAt, P.packSet, P.presence = now, nil, nil
	end
	-- Friends' packs see this one has the pack (Steam shows Rich Presence to friends only).
	local packWanted = partyOn()
	if packWanted ~= P.packSet and meowy.PartySet then
		P.packSet = packWanted
		meowy.PartySet("presence", "meowy_pack", packWanted and tostring(meowy.VERSION or "1") or "")
	end
	for _, e in ipairs(meowy.PartyPoll()) do onEvent(e, now) end
	if not partyOn() then
		if P.state then leave("Parties are switched off") end
		pushModel(now)
		return
	end

	local state = meowy.PartyState()
	if state and #state.members == 0 then
		-- No longer in it (the lobby closed, or Steam dropped us).
		leave("The party ended")
		state = nil
	end
	P.state = state
	if state then
		local leader = state.owner == state.self
		-- Where I am, for everyone's card.
		local mine, mineName = whereAmI()
		if mine ~= P.mine then
			P.mine = mine
			meowy.PartySet("member", "at", mine)
			meowy.PartySet("member", "atname", mineName:sub(1, 120))
			meowy.PartySet("member", "pack", tostring(meowy.VERSION or "1"))
		end
		local presence = state.lobby .. "|" .. #state.members
		if presence ~= P.presence then
			P.presence = presence
			meowy.PartySet("presence", "steam_player_group", state.lobby)
			meowy.PartySet("presence", "steam_player_group_size", tostring(#state.members))
		end
		-- The leader's target: content warmed ahead (prewarm), and a member follows.
		if Home.publicAddress(state.srv) and state.srv ~= P.warmed then
			P.warmed = state.srv
			if not leader and MeowyClientPackWarm then MeowyClientPackWarm(state.srv, "the party's next server") end
		end
		local leaderName = ""
		for _, m in ipairs(state.members) do if m.id == state.owner then leaderName = Home.clean(m.name) end end
		local to, seq = PartyLogic.shouldFollow({ go = state.go, lastSeq = P.lastSeq, leader = leader, mine = mine,
			now = now, lastFollowAt = P.lastFollowAt, allowed = allowed })
		P.lastSeq = seq
		if to then
			P.follow = { address = to, name = Home.clean(state.srvname), by = leaderName ~= "" and leaderName or "the leader",
				at = now + PartyLogic.FOLLOW_SECONDS }
			log("party: following %s to %s in %d s unless you stay", P.follow.by, to, PartyLogic.FOLLOW_SECONDS)
		end
		if P.follow and now >= P.follow.at then
			local address = P.follow.address
			P.follow, P.lastFollowAt = nil, now
			if address ~= mine and allowed(address) then
				log("party: joining %s with the party", address)
				join(address)
			end
		end
	else
		P.follow = nil
	end
	-- Friends playing Garry's Mod, for inviting: while our home or a party shows.
	if (menuPage.ours or state) and now - P.friendsAt >= FRIENDS_LOOK and meowy.PartyFriends then
		P.friendsAt = now
		local list, playing = meowy.PartyFriends(), {}
		for _, f in ipairs(list) do if f.ingmod then playing[#playing + 1] = f end end
		P.friendsRaw = playing
	end
	pushModel(now)
end

hook.Add("Think", "MeowyClientPack.Party", function()
	local ok, err = pcall(tick)
	if not ok and not P.failed then
		P.failed = true
		ErrorNoHalt("[Meowy Client Pack] party: " .. tostring(err) .. "\n")
	end
end)

-- In game: the countdown over the game, so a member sees it without the Escape menu.
local function banner()
	local f = P.follow
	local text = "Party: following " .. f.by .. " to " .. (f.name ~= "" and f.name or f.address) .. " in "
		.. PartyLogic.left(f, SysTime()) .. " s  \194\183  Esc, then Stay, to stay here"
	surface.SetFont("DermaDefaultBold")
	local w, h = surface.GetTextSize(text)
	local x, y = math.floor((ScrW() - w) / 2), 24
	surface.SetDrawColor(0, 0, 0, 200)
	surface.DrawRect(x - 14, y - 8, w + 28, h + 16)
	surface.SetTextColor(255, 255, 160, 255)
	surface.SetTextPos(x, y)
	surface.DrawText(text)
end
hook.Add("DrawOverlay", "MeowyClientPack.Party", function()
	if not P.follow or not IsInGame() or not surface or P.bannerFailed then return end
	local ok, err = pcall(banner)
	if not ok then
		P.bannerFailed = true
		ErrorNoHalt("[Meowy Client Pack] party banner: " .. tostring(err) .. "\n")
	end
end)

local function friendAt(i)
	i = math.floor(tonumber(i) or 0)
	return P.friendsRaw[i]
end

-- What the page (through menu_page.lua), the Escape card and the commands ask.
MeowyClientPackParty = {
	action = function(what)
		if what == "stay" then
			if P.follow then log("party: staying here") end
			P.follow = nil
		elseif not partyOn() then
			return
		elseif what == "create" then
			if P.state then return end
			if not meowy.PartyCreate() then say("Steam couldn't start a party right now") end
		elseif what == "leave" then
			if P.state then leave("You left the party") end
		elseif what == "invite" then
			if P.state and not meowy.PartyInviteDialog() then say("Steam's invite window couldn't open: invite from the list") end
		end
		P.nextLook = 0
	end,
	-- A friend from the list: invited to the party (with the pack), or to the party's server (without).
	inviteFriend = function(i)
		local f = friendAt(i)
		if not f or not partyOn() then return end
		if f.pack and P.state then
			if meowy.PartyInvite(f.id) then say("Invited " .. Home.clean(f.name)) else say("Couldn't invite " .. Home.clean(f.name)) end
			return
		end
		local target = P.state and Home.publicAddress(P.state.srv) and P.state.srv or (P.mine ~= "" and P.mine) or nil
		if not target then
			say(Home.clean(f.name) .. " has no Meowy Client Pack: start a party and pick a server first, then they get a Steam invite to it")
			return
		end
		if meowy.PartyInviteToServer(f.id, target) then say("Sent " .. Home.clean(f.name) .. " a Steam invite to the server") end
	end,
	-- A member's server: joined on the player's own click.
	joinMember = function(i)
		local s = P.state
		local m = s and s.members[math.floor(tonumber(i) or 0)]
		if not m or m.id == s.self or not allowed(m.at) then return end
		log("party: joining %s where %s is", m.at, Home.clean(m.name))
		join(m.at)
	end,
	-- The leader's "Take the party here": the party's server, then the leader goes.
	go = function(address, name)
		local s = P.state
		if not s or s.owner ~= s.self or not allowed(address) then return false end
		meowy.PartySet("lobby", "srv", address)
		meowy.PartySet("lobby", "srvname", Home.clean(name or ""):sub(1, 120))
		meowy.PartySet("lobby", "go", PartyLogic.nextGo(s.go, address))
		log("party: taking the party to %s", address)
		join(address)
		return true
	end,
	-- The Escape menu's line ({ text, stay }) or nil.
	escapeLine = function()
		return PartyLogic.escapeLine(PartyLogic.model(P.state, P.follow, SysTime(), {}, nil))
	end,
}

if concommand then
	concommand.Add("meowy_party_stay", function() MeowyClientPackParty.action("stay") end, nil,
		"Meowy Client Pack: stay on this server when your party moves")
	concommand.Add("meowy_party_status", function()
		local s = P.state
		log("%s; %s", meowy.PartyStatus and meowy.PartyStatus() or "parties: unavailable",
			s and string.format("%d members, leader %s, target %s, go %s%s", #s.members, s.owner == s.self and "you" or s.owner,
				s.srv ~= "" and s.srv or "none", s.go ~= "" and s.go or "none", P.follow and (", following to " .. P.follow.address) or "") or "no party")
	end, nil, "Meowy Client Pack: the party you're in")
end
