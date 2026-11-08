-- Parties (roadmap MR3, docs/server-index-and-parties.md): friends who move
-- between servers together. Runs in its own function at the end of
-- resilience.lua's chunk (module.cpp), after menu_page.lua, with PartyLogic
-- (party_logic.lua) and resilience.lua's locals in scope: log, join, Home.
--
-- The party is a friends-only Steam lobby (party_steam.h): any friend can
-- join it from Steam's friends list or an invite, and Steam shows the members
-- together (Rich Presence steam_player_group). The leader's "Take the party
-- here" (Find servers) moves everyone: each member's pack counts down
-- PartyLogic.FOLLOW_SECONDS with Stay, then joins. The countdown is a card
-- with Stay wherever the menu is up (any screen of it, Garry's Mod's own
-- menu, Find servers, the Escape menu in game), and a banner over the game
-- while playing; it only runs while one of them is on screen (not while a
-- join loads), so nobody is moved without having seen it. Nothing is followed
-- on the first look at a party, sooner than MIN_GAP seconds after the last
-- follow (the countdown waits), or to a server Garry's Mod's blacklist names.
-- The party's next server isn't contacted before the member joins it.
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
		-- The leader's target: a member follows. (Not warmed ahead: the leader picks
		-- the address, and nothing is sent to it before the member agreed to go.)
		local leaderName = ""
		for _, m in ipairs(state.members) do if m.id == state.owner then leaderName = Home.clean(m.name) end end
		local to, seq, notBefore = PartyLogic.shouldFollow({ go = state.go, lastSeq = P.lastSeq, leader = leader, mine = mine,
			now = now, lastFollowAt = P.lastFollowAt, allowed = allowed })
		P.lastSeq = seq
		if to then
			P.follow = PartyLogic.newFollow(to, Home.clean(state.srvname), leaderName ~= "" and leaderName or "the leader", notBefore)
			log("party: following %s to %s in %d s unless you stay", P.follow.by, to, PartyLogic.left(P.follow, now))
		end
		-- The countdown runs while it's on screen: the card at the menu (any screen,
		-- the Escape menu too), the banner in game; not while a join loads.
		if P.follow and P.lastTick and not IsInLoading() then
			P.follow.shown = P.follow.shown + math.min(now - P.lastTick, 1)
		end
		if P.follow and PartyLogic.due(P.follow, now) then
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
	P.lastTick = now
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
	if not P.follow or not IsInGame() or IsInLoading() or not surface or P.bannerFailed then return end
	local ok, err = pcall(banner)
	if not ok then
		P.bannerFailed = true
		ErrorNoHalt("[Meowy Client Pack] party banner: " .. tostring(err) .. "\n")
	end
end)

-- Wherever the menu is up (any of its screens, Garry's Mod's own menu, Find
-- servers, the Escape menu in game): the countdown as a card, top centre,
-- with Stay and Go now. Clicks only; it never takes the keyboard.
local card
local function hideCard()
	if IsValid(card) then card:Remove() end
	card = nil
end
local function drawCard(now)
	local f = P.follow
	if not (f and not IsInLoading() and menuVisible()) then return hideCard() end
	if not IsValid(card) then
		UI.fonts()
		card = vgui.Create("DPanel")
		card.w = S(460)
		card.Paint = function(self, w, h)
			UI.shadow(S(12), 0, 0, w, h)
			UI.box(S(12), 0, 0, w, h, C.window, C.border)
			return true
		end
		card.title = UI.Text(card, "", "MeowyPack.Heading", C.text)
		card.sub = UI.Text(card, "", "MeowyPack.Small", C.body)
		card.buttons = {
			UI.Button(card, "Stay", "secondary", function() MeowyClientPackParty.action("stay") end),
			UI.Button(card, "Go now", "primary", function() MeowyClientPackParty.action("go_now") end),
		}
		card:MakePopup()
		card:SetKeyboardInputEnabled(false)
		UI.fadeIn(card)
	end
	local title = "Your party is going to " .. (f.name ~= "" and f.name or f.address)
	local sub = f.by .. " moved the party. You follow in " .. PartyLogic.left(f, now) .. " s, unless you stay here."
	if card.key ~= title .. "|" .. sub then
		card.key = title .. "|" .. sub
		card.title.text, card.sub.text = title, sub
		local pad, buttonsW = S(18), 0
		for _, b in ipairs(card.buttons) do buttonsW = buttonsW + b.w + S(10) end
		local y = UI.stack({ card.title, card.sub }, pad, pad, card.w - pad * 2 - buttonsW, S(3))
		local h = math.max(y + pad, S(38) + pad * 2)
		card:SetSize(card.w, h)
		UI.row(card.buttons, 0, math.floor((h - S(38)) / 2), card.w - pad)
	end
	-- Below the reconnect card when that shows too.
	local top = IsValid(overlay) and select(2, overlay:GetPos()) + overlay:GetTall() + S(12) or S(28)
	card:SetPos(math.floor((ScrW() - card.w) / 2), top)
	card:MoveToFront()
end
hook.Add("Think", "MeowyClientPack.PartyCard", function()
	if P.cardFailed then return end
	local ok, err = pcall(drawCard, SysTime())
	if not ok then
		P.cardFailed = true
		hideCard()
		ErrorNoHalt("[Meowy Client Pack] party card: " .. tostring(err) .. "\n")
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
		elseif what == "go_now" then
			-- The player's own click: no countdown and no gap to wait for.
			local f = P.follow
			P.follow = nil
			if f and not IsInLoading() and allowed(f.address) and f.address ~= P.mine then
				P.lastFollowAt = SysTime()
				log("party: joining %s with the party (Go now)", f.address)
				join(f.address)
			end
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
