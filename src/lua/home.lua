-- The main menu's Home card (first part of the new Home): on the main page,
-- out of game, our server's live status, then the player's favourites and
-- recent servers (the game's own lists, asked with the game's own query, not
-- the honest-count wrapper), each joined with one click. In game, the same
-- spot in the Escape menu has the server card instead (below).
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
local tickEsc                 -- the Escape menu's card, below
local sentText, sentAt, version = nil, 0, 0
local lastOurs = false        -- menuPage.ours when last sent

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
		+ '#meowy-home .mh-detail{font-size:13px;opacity:0.75;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}'
		+ '#meowy-home .mh-search{display:block;width:100%;box-sizing:border-box;margin:4px 0 2px 0;padding:6px 10px;'
		+ 'font-size:14px;border:0;border-radius:4px;background:rgba(255,255,255,0.12);color:#fff;outline:none;}'
		+ '#meowy-home .mh-search:focus{background:rgba(255,255,255,0.2);}'
		+ '#meowy-home .mh-search::-webkit-input-placeholder{color:rgba(255,255,255,0.55);}'
		+ '#meowy-home .mh-sel{background:rgba(255,255,255,0.2);}'
		+ '#meowy-home{overflow-y:auto;}';
	var MAX_MATCHES = 8;   // rows per section while searching
	var query = '', selected = 0, rows = [], focused = false;
	function node(tag, cls, text) {
		var e = document.createElement(tag);
		if (cls) e.className = cls;
		if (text) e.textContent = text;
		return e;
	}
	function join(id) { lua.Run('MeowyClientPackHomeJoin(' + Number(id) + ')'); }
	// Resting the pointer on a row for a moment: get that server ready (prewarm.h).
	function intent(el, id) {
		var timer = null;
		el.onmouseenter = function () {
			timer = setTimeout(function () { lua.Run('MeowyClientPackHomeIntent(' + Number(id) + ')'); }, 400);
		};
		el.onmouseleave = function () { if (timer) clearTimeout(timer); timer = null; };
	}
	function mark() {
		for (var i = 0; i < rows.length; i++) {
			var on = focused && i === selected;   // the row Enter joins, while the search box has the keyboard
			rows[i].el.className = rows[i].cls + (on ? ' mh-sel' : '');
			if (on && rows[i].el.scrollIntoView) rows[i].el.scrollIntoView(false);
		}
	}
	// The list: each section's first `show` rows; while searching, every row
	// whose name or map has the text, at most MAX_MATCHES a section.
	function render(list) {
		var data = window.MeowyHomeData, q = query.toLowerCase();
		while (list.firstChild) list.removeChild(list.firstChild);
		rows = [];
		for (var i = 0; i < data.sections.length; i++) {
			var sec = data.sections[i], picked = [];
			for (var j = 0; j < sec.rows.length; j++) {
				var r = sec.rows[j];
				if (q && (r.name + ' ' + r.detail).toLowerCase().indexOf(q) < 0) continue;
				picked.push(r);
				if (picked.length >= (q ? MAX_MATCHES : sec.show)) break;
			}
			if (q && !picked.length) continue;
			list.appendChild(node('div', 'mh-title', sec.title));
			for (var k = 0; k < picked.length; k++) {
				var p = picked[k], cls = 'mh-row' + (p.dim ? ' mh-dim' : '');
				var el = node('div', cls);
				if (p.stats) el.appendChild(node('span', 'mh-stats', p.stats));
				el.appendChild(node('div', 'mh-name', p.name));
				if (p.detail) el.appendChild(node('div', 'mh-detail', p.detail));
				el.title = p.name;
				el.onclick = (function (id) { return function () { join(id); }; })(p.id);
				intent(el, p.id);
				list.appendChild(el);
				rows.push({ id: p.id, el: el, cls: cls });
			}
			if (!q && sec.note) list.appendChild(node('div', 'mh-note', sec.note));
		}
		if (q && !rows.length) list.appendChild(node('div', 'mh-note', 'No server matches'));
		if (selected >= rows.length) selected = rows.length - 1;
		if (selected < 0) selected = 0;
		mark();
	}
	// In the search box: arrows move, Enter joins, Esc clears.
	function keys(e, input, list) {
		var k = e.keyCode;
		if (k === 40 || k === 38) {
			selected = Math.max(0, Math.min(rows.length - 1, selected + (k === 40 ? 1 : -1)));
			mark();
		} else if (k === 13) {
			if (rows[selected]) join(rows[selected].id);
		} else if (k === 27) {
			input.value = '';
			query = '';
			selected = 0;
			render(list);
			input.blur();
		} else {
			return;
		}
		e.preventDefault();
		e.stopPropagation();
	}
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
		var v = String(window.MeowyHomeVersion);
		if (box && box.getAttribute('data-v') === v) return;
		if (!box) {
			box = node('div');
			box.id = 'meowy-home';
			var input = node('input', 'mh-search');
			input.type = 'text';
			input.placeholder = 'Search your servers   ( / )';
			input.maxLength = 64;
			input.value = query;
			var list = node('div', 'mh-list');
			input.oninput = function () { query = input.value; selected = 0; render(list); };
			input.onkeydown = function (e) { keys(e, input, list); };
			input.onfocus = function () { focused = true; mark(); };
			input.onblur = function () { focused = false; mark(); };
			box.appendChild(input);
			box.appendChild(list);
			page.appendChild(box);
		}
		box.setAttribute('data-v', v);
		render(box.querySelector('.mh-list'));
	}
	window.MeowyHome = function (data, version) {
		window.MeowyHomeData = data;
		window.MeowyHomeVersion = version;
		draw();
	};
	// "/" anywhere on the main page (not while typing elsewhere) goes to the search box.
	document.addEventListener('keydown', function (e) {
		var box = document.getElementById('meowy-home');
		var t = e.target && e.target.tagName;
		if (!box || t === 'INPUT' || t === 'TEXTAREA') return;
		if (e.key === '/' || e.keyCode === 191) {
			box.querySelector('.mh-search').focus();
			e.preventDefault();
		}
	}, true);
	var MO = window.MutationObserver || window.WebKitMutationObserver;
	if (MO && document.body) new MO(draw).observe(document.body, { childList: true, subtree: true });
})();
]==]

-- One of the game's lists ("friends": servers Steam friends are on, "favorite",
-- "history"). The first time, rows fill
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
	if IsInGame() or IsInLoading() or not (homeOn() or menuPage.ours) then return end
	local address = addresses[math.floor(tonumber(id) or 0)]
	if not Home.validAddress(address) then return end
	log("home: joining %s", address)
	join(address)
end

-- Join pre-warm (prewarm.h): read a server's recorded files into memory when
-- the player is about to join it. Recorded at every join (any server; native
-- keeps ours or a public address only); warmed for the Continue server after a
-- few seconds at the main menu, or for a row the pointer rests on.
local pw = { joining = false, warmedFor = nil, menuSince = nil }

-- Workshop items (prewarm.h, second part): of the items the server's last join
-- mounted, only those that need it are handed to Steam (steamworks.DownloadUGC,
-- the menu's own, which shows Garry's Mod's download box), one at a time, so an
-- update or a missing item downloads (or unpacks) now instead of on the loading
-- screen. An item needs it when Steam says so (meowy.WorkshopItemState: not
-- installed, an update known, mid-download), when the game's unpacked copy of
-- a legacy item is gone, or when the Workshop's version (steamworks.FileInfo)
-- is newer than the one installed. Nothing new is asked once a join starts:
-- the join asks for itself, and Steam carries on with anything downloading.
local WS_GIVE_UP = 120   -- seconds: Steam never answered
local WS_TIME_SLACK = 256   -- seconds: FileInfo's float rounding (below)
local ws = { gen = 0, result = "idle" }

local function wsFinish(stopped)
	if not ws.address then return end
	ws.result = string.format("%s %s: %d Workshop items, %d needed an update or unpack, %d done, %d failed, %.1f s",
		stopped and "stopped checking" or "checked", ws.address, ws.total, ws.stale, ws.done, ws.failed, SysTime() - ws.started)
	log("prewarm: %s", ws.result)
	ws.address, ws.queue = nil, nil
end

local function wsPump()
	if not ws.queue then return end
	if IsInLoading() or IsInGame() then wsFinish(true) return end
	if not ws.inFlight and #ws.queue > 0 then
		local id = table.remove(ws.queue, 1)
		local gen = ws.gen
		ws.inFlight = id
		steamworks.DownloadUGC(id, function(path, f)
			if f and f.Close then f:Close() end
			if gen ~= ws.gen then return end
			ws.inFlight = nil
			if path then ws.done = ws.done + 1 else ws.failed = ws.failed + 1 end
			wsPump()
		end)
	end
	if not ws.inFlight and #ws.queue == 0 and ws.pending == 0 then wsFinish(false) end
end

local function wsDecide(gen, id, needs)
	if gen ~= ws.gen or not ws.queue then return end
	ws.pending = ws.pending - 1
	if needs then
		ws.stale = ws.stale + 1
		ws.queue[#ws.queue + 1] = id
	end
	wsPump()
end

local function wsStart(address)
	if not (prewarmWorkshopOn() and meowy.PrewarmWorkshop and meowy.WorkshopItemState and steamworks and steamworks.DownloadUGC) then return end
	if ws.address == address then return end
	local ids = meowy.PrewarmWorkshop(address)
	if not istable(ids) or #ids == 0 then return end
	ws.gen = ws.gen + 1
	local gen = ws.gen
	ws.address, ws.queue, ws.total, ws.pending = address, {}, #ids, #ids
	ws.inFlight, ws.stale, ws.done, ws.failed, ws.started = nil, 0, 0, 0, SysTime()
	for _, id in ipairs(ids) do
		local installed, needsUpdate, busy, legacy, updated = meowy.WorkshopItemState(id)
		if installed == nil then
			wsDecide(gen, id, false)   -- Steam can't say: leave it to the join
		elseif not installed or needsUpdate or busy
			or (legacy and file and file.Exists and not file.Exists("cache/workshop/" .. id .. ".gma", "MOD")) then
			wsDecide(gen, id, true)
		elseif steamworks.FileInfo then
			steamworks.FileInfo(id, function(info)
				-- FileInfo's time comes through a float: today's unix times are
				-- rounded to 128 s, either way. Newer means newer by more than that.
				local now = istable(info) and tonumber(info.updated)
				wsDecide(gen, id, now ~= nil and now > (tonumber(updated) or 0) + WS_TIME_SLACK)
			end)
		else
			wsDecide(gen, id, false)
		end
	end
end

local function warm(address, why)
	if not (Home.validAddress(address) and (prewarmOn() or prewarmWorkshopOn())) then return end
	if IsInGame() or IsInLoading() or address == pw.warmedFor then return end
	if not (meowy.PrewarmRecorded and meowy.PrewarmRecorded(address)) then return end
	pw.warmedFor = address
	wsStart(address)
	if not (prewarmOn() and meowy.PrewarmStart) then return end
	local ok, reason = meowy.PrewarmStart(address)
	log("prewarm: %s %s%s", ok and "warming" or "not warming", address, ok and (" (" .. why .. ")") or (": " .. tostring(reason)))
end

-- The same for a server listed elsewhere on the home screen (Find servers,
-- menu_page.lua), by its address.
function MeowyClientPackWarm(address, why)
	if isstring(address) and isstring(why) then warm(address, why) end
end

function MeowyClientPackHomeIntent(id)
	if not (homeOn() or menuPage.ours) then return end
	warm(addresses[math.floor(tonumber(id) or 0)], "pointer on its row")
end

hook.Add("Think", "MeowyClientPack.Prewarm", function()
	if not meowy.PrewarmBeginJoin then return end
	local loading = IsInLoading()
	if ws.queue and (loading or SysTime() - ws.started > WS_GIVE_UP) then wsFinish(true) end
	if loading and not pw.joining then
		pw.joining = true
		if pakCacheOn() and meowy.PakCache then
			local ok, why = meowy.PakCache(true)
			if not ok and not pw.pakWhyLogged then pw.pakWhyLogged = true log("pak cache: off (%s)", tostring(why)) end
		end
		meowy.PrewarmBeginJoin()   -- also stops any warming: the join's reads come first
	elseif pw.joining and IsInGame() and not loading then
		pw.joining = false
		if meowy.PakCacheState and meowy.PakCache then
			log("pak cache: %s", meowy.PakCacheState())
			meowy.PakCache(false)
		end
		local kept = meowy.PrewarmEndJoin()
		pw.warmedFor = nil
		if kept > 0 then log("prewarm: kept %d files for this server", kept) end
	elseif not loading and not IsInGame() then
		if pw.joining and meowy.PakCache then meowy.PakCache(false) end
		pw.joining = false   -- a join that didn't get in: nothing to keep
	end
	-- A few seconds at the main menu: the Continue server, once per visit (a
	-- row the pointer rests on afterwards takes over).
	if menuVisible() and not IsInGame() and not loading then
		pw.menuSince = pw.menuSince or SysTime()
		if not pw.continueDone and SysTime() - pw.menuSince > 3 and meowy.LastServer then
			pw.continueDone = true
			local address = meowy.LastServer()
			if isstring(address) then warm(address, "your Continue server") end
		end
	else
		pw.menuSince, pw.continueDone = nil, false
	end
end)

if concommand then
	concommand.Add("meowy_prewarm_status", function()
		MsgN("[Meowy Client Pack] prewarm: " .. (meowy.PrewarmState and meowy.PrewarmState() or "unavailable"))
		local wsLine = ws.address and string.format("checking %s: %d Workshop items, %d still to check, %d needed an update or unpack, %d done",
			ws.address, ws.total, ws.pending, ws.stale, ws.done) or ws.result
		MsgN("[Meowy Client Pack] prewarm workshop: " .. wsLine)
		MsgN("[Meowy Client Pack] pak cache: " .. (meowy.PakCacheState and meowy.PakCacheState() or "unavailable"))
	end, nil, "Meowy Client Pack: what join pre-warm is doing")
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
	pnlMainMenu:Call(HOME_JS .. "window.MeowyHome(" .. text .. "," .. version .. ");\n"
		.. "if (window.MeowyShellLists) window.MeowyShellLists();\n")   -- the Meowy home screen's lists (shell.js)
end

local function tick()
	local now = SysTime()
	if now < nextLook then return end
	nextLook = now + 0.5
	-- Our own menu page (menu_page.lua) shows the same lists, whatever the card's setting.
	if menuPage.ours ~= lastOurs then
		lastOurs, sentText, sentAt = menuPage.ours, nil, 0   -- a new page: send it everything now
	end
	local visible = (homeOn() or menuPage.ours) and menuVisible() and not IsInGame() and not IsInLoading()
		and IsValid(pnlMainMenu) and pnlMainMenu.Call ~= nil
	if visible then
		if not shown or now >= nextRefresh then
			nextRefresh = now + REFRESH
			askOurs()
			ask("friends", now)
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
	tickEsc(now)
end

-- The Escape menu's card ------------------------------------------------------------
-- In game, the Escape menu is the same page with "Resume Game": the server
-- you're on (its name and map as it answers, the engine's own ping, loss and
-- time connected), Reconnect, Copy address, and three quick sliders.
local ESC_JS = [==[
(function () {
	if (window.MeowyEsc) return;
	var STYLE = '#meowy-esc{position:absolute;right:24px;bottom:16px;width:460px;overflow:hidden;'
		+ 'background:rgba(0,0,0,0.6);border-radius:6px;padding:6px 16px 12px 16px;box-sizing:border-box;'
		+ 'color:#fff;text-shadow:1px 1px 1px #000;z-index:5;}'
		+ '#meowy-esc .me-title{font-size:12px;font-weight:bold;letter-spacing:1px;text-transform:uppercase;opacity:0.7;margin:10px 0 4px 0;}'
		+ '#meowy-esc .me-name{font-size:17px;font-weight:bold;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;line-height:24px;}'
		+ '#meowy-esc .me-line{font-size:13px;opacity:0.8;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;line-height:18px;}'
		+ '#meowy-esc .me-btn{float:right;margin-left:8px;font-size:13px;font-weight:bold;padding:2px 10px;border-radius:4px;'
		+ 'background:rgba(255,255,255,0.16);cursor:pointer;line-height:20px;}'
		+ '#meowy-esc .me-btn:hover{background:rgba(255,255,255,0.3);}'
		+ '#meowy-esc .me-set{overflow:hidden;font-size:13px;line-height:24px;}'
		+ '#meowy-esc .me-set span{float:left;width:120px;}'
		+ '#meowy-esc .me-set b{float:right;width:44px;text-align:right;font-weight:normal;opacity:0.85;}'
		+ '#meowy-esc input{width:240px;margin:6px 0 0 8px;float:left;}';
	function node(tag, cls, text) {
		var e = document.createElement(tag);
		if (cls) e.className = cls;
		if (text) e.textContent = text;
		return e;
	}
	function shown(s, v) { return s.percent ? Math.round(v * 100) + '%' : String(Math.round(v)); }
	var dragging = false, lastSent = 0;
	function send(id, v) { lua.Run('MeowyClientPackEscSet(' + Number(id) + ',' + Number(v) + ')'); }
	function slider(s) {
		var row = node('div', 'me-set');
		row.appendChild(node('span', null, s.label));
		var val = node('b', null, shown(s, s.value));
		var input = node('input');
		input.type = 'range';
		input.min = s.min; input.max = s.max; input.step = s.step; input.value = s.value;
		input.setAttribute('data-id', s.id);
		input.onmousedown = function () { dragging = true; };
		input.onmouseup = function () { dragging = false; };
		input.oninput = function () {
			val.textContent = shown(s, Number(input.value));
			var now = Date.now();
			if (now - lastSent > 100) { lastSent = now; send(s.id, input.value); }
		};
		input.onchange = function () { dragging = false; val.textContent = shown(s, Number(input.value)); send(s.id, input.value); };
		row.appendChild(input);
		row.appendChild(val);
		return row;
	}
	function draw() {
		var data = window.MeowyEscData;
		var page = document.querySelector('.page[ng-controller="ControllerMain"]');
		var box = document.getElementById('meowy-esc');
		if (!data || !page) { if (box) box.parentNode.removeChild(box); return; }
		if (!document.getElementById('meowy-esc-style')) {
			var css = node('style');
			css.id = 'meowy-esc-style';
			css.textContent = STYLE;
			(document.head || document.body).appendChild(css);
		}
		if (box && box.parentNode !== page) { box.parentNode.removeChild(box); box = null; }
		var v = String(window.MeowyEscVersion);
		if (box && box.getAttribute('data-v') === v) return;
		if (box && dragging) return;   // not while a slider is held; the next push catches up
		if (!box) { box = node('div'); box.id = 'meowy-esc'; page.appendChild(box); }
		box.setAttribute('data-v', v);
		while (box.firstChild) box.removeChild(box.firstChild);
		if (data.server) {
			box.appendChild(node('div', 'me-title', 'Server'));
			var name = node('div', 'me-name');
			var re = node('span', 'me-btn', 'Reconnect');
			re.onclick = function () { lua.Run('MeowyClientPackEscAction("reconnect")'); };
			name.appendChild(re);
			name.appendChild(document.createTextNode(data.server.name));
			name.title = data.server.name;
			box.appendChild(name);
			if (data.server.line1) box.appendChild(node('div', 'me-line', data.server.line1));
			if (data.server.line2) box.appendChild(node('div', 'me-line', data.server.line2));
			var addr = node('div', 'me-line');
			var copy = node('span', 'me-btn', 'Copy');
			copy.onclick = function () {
				lua.Run('MeowyClientPackEscAction("copy")');
				copy.textContent = 'Copied';
				setTimeout(function () { copy.textContent = 'Copy'; }, 1500);
			};
			addr.appendChild(copy);
			addr.appendChild(document.createTextNode(data.server.address));
			addr.style.lineHeight = '24px';
			box.appendChild(addr);
		}
		if (data.party) {
			box.appendChild(node('div', 'me-title', 'Party'));
			var pl = node('div', 'me-name');
			pl.style.fontSize = '15px';
			if (data.party.stay) {
				var stay = node('span', 'me-btn', 'Stay');
				stay.onclick = function () { lua.Run('MeowyClientPackEscAction("party_stay")'); };
				pl.appendChild(stay);
			}
			pl.appendChild(document.createTextNode(data.party.text));
			box.appendChild(pl);
		}
		if (data.settings.length) {
			box.appendChild(node('div', 'me-title', 'Quick settings'));
			for (var i = 0; i < data.settings.length; i++) box.appendChild(slider(data.settings[i]));
		}
	}
	window.MeowyEsc = function (data, version) {
		window.MeowyEscData = data;
		window.MeowyEscVersion = version;
		draw();
	};
	document.addEventListener('mouseup', function () { dragging = false; }, true);
	var MO = window.MutationObserver || window.WebKitMutationObserver;
	if (MO && document.body) new MO(draw).observe(document.body, { childList: true, subtree: true });
})();
]==]

local esc = { info = {}, nextAsk = 0, sentText = nil, sentAt = 0, version = 0, shown = false, askedFor = nil }

local function convarNumber(name)
	local cv = GetConVar and GetConVar(name)
	if cv and cv.GetFloat then return cv:GetFloat() end
	return tonumber(GetConVarString and GetConVarString(name))
end

-- The card's buttons: only these two, only in game.
function MeowyClientPackEscAction(what)
	if not IsInGame() or not escapeOn() then return end
	if what == "reconnect" then
		log("escape menu: reconnect")
		RunConsoleCommand("retry")
	elseif what == "party_stay" then
		if MeowyClientPackParty then MeowyClientPackParty.action("stay") end
	elseif what == "copy" then
		local address = meowy.ServerNow and meowy.ServerNow()
		if Home.validAddress(address) and SetClipboardText then SetClipboardText(address) end
	end
end

-- A slider: its row number and a number; the convar and range are this side's.
function MeowyClientPackEscSet(id, value)
	if not IsInGame() or not escapeOn() then return end
	local key, text = Home.settingValue(id, value)
	if not key then return end
	RunConsoleCommand(key, text)
	esc.info.values = esc.info.values or {}
	esc.info.values[key] = tonumber(text)
end

local function sendEsc(text)
	if not (IsValid(pnlMainMenu) and pnlMainMenu.Call) then return end
	if text ~= esc.sentText then esc.version = esc.version + 1 end
	esc.sentText, esc.sentAt = text, SysTime()
	pnlMainMenu:Call(ESC_JS .. "window.MeowyEsc(" .. text .. "," .. esc.version .. ");\n")
end

function tickEsc(now)
	local visible = escapeOn() and IsInGame() and not IsInLoading() and menuVisible()
		and IsValid(pnlMainMenu) and pnlMainMenu.Call ~= nil and meowy.ServerNow ~= nil
	if not visible then
		if esc.shown then sendEsc("null") end
		esc.shown = false
		return
	end
	local info = esc.info
	local address, latency, loss, connected = meowy.ServerNow()
	if address ~= esc.askedFor then   -- another server: what we knew is about the last one
		esc.askedFor, esc.nextAsk = address, 0
		info.map, info.players, info.max, info.answerName = nil, nil, nil, nil
	end
	info.address, info.latency, info.loss, info.connected = address, latency, loss, connected
	info.updaterate = convarNumber("cl_updaterate")
	info.name = info.answerName or meowy.JoiningName
	info.values = {}
	for _, s in ipairs(Home.SETTINGS) do info.values[s.key] = convarNumber(s.key) end
	info.party = MeowyClientPackParty and MeowyClientPackParty.escapeLine() or nil
	-- The server's own answer (map, players) at most every 30 s while the card shows.
	if Home.validAddress(address) and now >= esc.nextAsk and serverlist and serverlist.PingServer then
		esc.nextAsk = now + 30
		pcall(serverlist.PingServer, address, function(ping, name, desc, map, players, maxplayers, botplayers)
			if not name or address ~= esc.askedFor then return end
			local s = Home.server(ping, name, desc, map, players, maxplayers, botplayers, false, 0, address)
			if s then info.answerName, info.map, info.players, info.max = s.name, s.map, s.players, s.max end
		end)
	end
	local text = Home.encode(Home.escapeModel(info))
	if text ~= esc.sentText or now - esc.sentAt >= RESEND then sendEsc(text) end
	esc.shown = true
end

if concommand then
	-- The engine's reading of the connection, to check the card's numbers against net_graph.
	concommand.Add("meowy_server_now", function()
		if not meowy.ServerNow then return end
		local address, latency, loss, connected = meowy.ServerNow()
		MsgN(string.format("[Meowy Client Pack] server now: address=%s, ping=%s ms, loss=%s, connected=%s s",
			tostring(address), tostring(latency), tostring(loss), tostring(connected)))
	end, nil, "Meowy Client Pack: the connection as the Escape menu's card reads it")
end

local failed = false
hook.Add("Think", "MeowyClientPack.Home", function()
	local ok, err = pcall(tick)
	if not ok and not failed then
		failed = true   -- once, not twice a second
		ErrorNoHalt("[Meowy Client Pack] home: " .. tostring(err) .. "\n")
	end
end)
