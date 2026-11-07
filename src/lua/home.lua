-- The main menu's Home card (first part of the new Home): on the main page,
-- out of game, our server's live status, then the player's favourites and
-- recent servers (the game's own lists, asked with the game's own query, not
-- the honest-count wrapper), each joined with one click.
--
-- Runs in its own function at the end of resilience.lua's chunk (module.cpp),
-- with `Home` (home_logic.lua) and resilience.lua's locals in scope: join, log,
-- menuVisible, homeOn. Every server's text reaches the page as an escaped JS
-- string and is only set as textContent; a click sends back a row number, and
-- the address comes from this side.
local query = meowy.StockServerQuery or (serverlist and serverlist.Query)
local REFRESH = 60   -- seconds between asks while the card shows (at most one query per server per minute)
local STUCK = 30     -- a list not finished after this is asked again at the next refresh
local RESEND = 5     -- seconds: the same data again, in case the page was rebuilt

local st = { oursAddress = meowy.OurServerAddress and meowy.OurServerAddress() or nil }
local pending = {}            -- kind -> SysTime it was asked
local addresses = {}          -- row number -> address, from the model last sent
local nextLook, nextRefresh, shown = 0, 0, false
local sentText, sentAt, version = nil, 0, 0

local HOME_JS = [==[
(function () {
	if (window.MeowyHome) return;
	var STYLE = '#meowy-home{position:absolute;right:24px;bottom:16px;width:460px;'
		+ 'max-height:-webkit-calc(100% - 380px);max-height:calc(100% - 380px);overflow:hidden;'
		+ 'background:rgba(0,0,0,0.6);border-radius:6px;padding:6px 6px 10px 6px;box-sizing:border-box;'
		+ 'color:#fff;text-shadow:1px 1px 1px #000;z-index:5;}'
		+ '#meowy-home .mh-title{font-size:12px;font-weight:bold;letter-spacing:1px;text-transform:uppercase;opacity:0.7;margin:10px 10px 3px 10px;}'
		+ '#meowy-home .mh-note{font-size:13px;opacity:0.6;margin:2px 10px 4px 10px;}'
		+ '#meowy-home .mh-row{cursor:pointer;padding:5px 10px;border-radius:4px;overflow:hidden;}'
		+ '#meowy-home .mh-row:hover{background:rgba(255,255,255,0.14);}'
		+ '#meowy-home .mh-dim{opacity:0.5;}'
		+ '#meowy-home .mh-stats{float:right;font-size:13px;margin-left:12px;opacity:0.85;line-height:21px;}'
		+ '#meowy-home .mh-name{font-size:17px;font-weight:bold;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;line-height:21px;}'
		+ '#meowy-home .mh-detail{font-size:13px;opacity:0.75;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}';
	function node(tag, cls, text) {
		var e = document.createElement(tag);
		if (cls) e.className = cls;
		if (text) e.textContent = text;
		return e;
	}
	function join(id) { return function () { lua.Run('MeowyClientPackHomeJoin(' + Number(id) + ')'); }; }
	function draw() {
		var data = window.MeowyHomeData;
		var page = document.querySelector('.page[ng-controller="ControllerMain"]');
		var box = document.getElementById('meowy-home');
		if (!data || !page) { if (box) box.parentNode.removeChild(box); return; }
		if (!document.getElementById('meowy-home-style')) {
			var css = node('style');
			css.id = 'meowy-home-style';
			css.textContent = STYLE;
			(document.head || document.body).appendChild(css);
		}
		if (box && box.parentNode !== page) { box.parentNode.removeChild(box); box = null; }
		if (box && box.getAttribute('data-v') === String(window.MeowyHomeVersion)) return;
		if (!box) { box = node('div'); box.id = 'meowy-home'; page.appendChild(box); }
		box.setAttribute('data-v', String(window.MeowyHomeVersion));
		while (box.firstChild) box.removeChild(box.firstChild);
		for (var i = 0; i < data.sections.length; i++) {
			var sec = data.sections[i];
			box.appendChild(node('div', 'mh-title', sec.title));
			for (var j = 0; j < sec.rows.length; j++) {
				var r = sec.rows[j];
				var el = node('div', 'mh-row' + (r.dim ? ' mh-dim' : ''));
				if (r.stats) el.appendChild(node('span', 'mh-stats', r.stats));
				el.appendChild(node('div', 'mh-name', r.name));
				if (r.detail) el.appendChild(node('div', 'mh-detail', r.detail));
				el.title = r.name;
				el.onclick = join(r.id);
				box.appendChild(el);
			}
			if (sec.note) box.appendChild(node('div', 'mh-note', sec.note));
		}
	}
	window.MeowyHome = function (data, version) {
		window.MeowyHomeData = data;
		window.MeowyHomeVersion = version;
		draw();
	};
	var MO = window.MutationObserver || window.WebKitMutationObserver;
	if (MO && document.body) new MO(draw).observe(document.body, { childList: true, subtree: true });
})();
]==]

-- One of the game's lists ("favorite", "history"). The first time, rows fill
-- in as servers answer; after that the old list stays until the new one is done.
local function ask(kind, now)
	if not query then return end
	if pending[kind] and now - pending[kind] < STUCK then return end
	pending[kind] = now
	local list, n = {}, 0
	if st[kind] == nil then st[kind] = list end
	local function keep(s)
		if s and n < Home.KEEP then
			n = n + 1
			list[n] = s
		end
		return n < Home.KEEP
	end
	local ok, err = pcall(query, {
		Type = kind, GameDir = "garrysmod", AppID = 4000,
		Callback = function(...) return keep(Home.server(...)) end,
		CallbackFailed = function(address) return keep(Home.failed(address)) end,
		Finished = function()
			st[kind] = list
			pending[kind] = nil
		end,
	})
	if not ok then
		pending[kind] = nil
		log("home: couldn't ask for the %s list: %s", kind, tostring(err))
	end
end

-- Our server, through the game's own server ping (no answer: name is nil).
local function askOurs()
	local address = st.oursAddress
	if not (serverlist and serverlist.PingServer and Home.validAddress(address)) then return end
	local ok, err = pcall(serverlist.PingServer, address, function(ping, name, desc, map, players, maxplayers, botplayers, pass, lastplayed)
		if name then
			st.ours = Home.server(ping, name, desc, map, players, maxplayers, botplayers, pass, lastplayed, address)
		else
			st.ours = Home.failed(address)
		end
	end)
	if not ok then log("home: couldn't ask our server: %s", tostring(err)) end
end

-- A row, clicked on the page: its address from this side, checked, then joined.
function MeowyClientPackHomeJoin(id)
	if IsInGame() or IsInLoading() or not homeOn() then return end
	local address = addresses[math.floor(tonumber(id) or 0)]
	if not Home.validAddress(address) then return end
	log("home: joining %s", address)
	join(address)
end

-- The same from the console: meowy_home lists the rows, meowy_home_join <n> joins one.
local lastModel
if concommand then
	concommand.Add("meowy_home", function()
		if not lastModel then MsgN("[Meowy Client Pack] home: nothing listed yet (it fills in at the main menu)") return end
		for _, sec in ipairs(lastModel.sections) do
			MsgN("[Meowy Client Pack] home: " .. sec.title .. (sec.note and (" (" .. sec.note .. ")") or ""))
			for _, r in ipairs(sec.rows) do
				MsgN(string.format("  %d. %s  %s  %s", r.id, r.name, r.stats, r.detail))
			end
		end
	end, nil, "Meowy Client Pack: list the main menu's Home card, numbered")
	concommand.Add("meowy_home_join", function(_, _, args)
		MeowyClientPackHomeJoin(args and args[1])
	end, nil, "Meowy Client Pack: join row <n> of meowy_home")
end

local function send(text)
	if not (IsValid(pnlMainMenu) and pnlMainMenu.Call) then return end
	if text ~= sentText then version = version + 1 end
	sentText, sentAt = text, SysTime()
	pnlMainMenu:Call(HOME_JS .. "window.MeowyHome(" .. text .. "," .. version .. ");\n")
end

local function tick()
	local now = SysTime()
	if now < nextLook then return end
	nextLook = now + 0.5
	local visible = homeOn() and menuVisible() and not IsInGame() and not IsInLoading()
		and IsValid(pnlMainMenu) and pnlMainMenu.Call ~= nil
	if visible then
		if not shown or now >= nextRefresh then
			nextRefresh = now + REFRESH
			askOurs()
			ask("favorite", now)
			ask("history", now)
		end
		local model = Home.model(st, os.time())
		addresses, lastModel = model.addresses, model
		local text = Home.encode({ sections = model.sections })
		if text ~= sentText or now - sentAt >= RESEND then send(text) end
	elseif shown then
		send("null")   -- in game (the Escape menu is the same page) or turned off
	end
	shown = visible
end

local failed = false
hook.Add("Think", "MeowyClientPack.Home", function()
	local ok, err = pcall(tick)
	if not ok and not failed then
		failed = true   -- once, not twice a second
		ErrorNoHalt("[Meowy Client Pack] home: " .. tostring(err) .. "\n")
	end
end)
