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
		+ '#meowy-home .mh-prep{font-size:12px;opacity:0.85;margin-top:2px;}'
		+ '#meowy-home .mh-prep-ask{display:inline-block;font-weight:bold;padding:1px 8px;border-radius:4px;'
		+ 'background:rgba(255,255,255,0.16);cursor:pointer;opacity:1;}'
		+ '#meowy-home .mh-prep-ask:hover{background:rgba(255,255,255,0.3);}'
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
				// The server's content (docs/CONTENT_PLATFORM.md): progress, or the
				// "Get ready" button, which doesn't join.
				if (p.prep) {
					var pr = el.appendChild(node('div', 'mh-prep'));
					var label = pr.appendChild(node('span', p.prepAsk ? 'mh-prep-ask' : '', p.prep));
					if (p.prepAsk) {
						label.title = 'Download this server\'s Workshop content now, so joining it is quick';
						label.onclick = (function (id) {
							return function (e) { e.stopPropagation(); lua.Run('MeowyClientPackHomePrepare(' + Number(id) + ')'); };
						})(p.id);
					}
				}
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

-- Workshop items, prepared before the join (prewarm.h, second part, and the
-- content platform, docs/CONTENT_PLATFORM.md). A server's list is what its
-- advert says (the `meowy_content` rule in its A2S_RULES answer, read with
-- meowy.ContentAsk) plus what its last join here mounted. Of those, only the
-- items that need it are handed to Steam (steamworks.DownloadUGC, the menu's
-- own, which shows Garry's Mod's download box), one at a time, so a missing
-- item, an update or an unpack happens now instead of on the loading screen.
-- An item needs it when Steam says so (meowy.WorkshopItemState: not installed,
-- an update known, mid-download), when the game's unpacked copy of a legacy
-- item is gone, or when the Workshop's version (steamworks.FileInfo) is newer
-- than the one installed (Content.needs / Content.newer).
--
-- Consent (Content.consent): automatic for our servers and servers joined
-- before here; any other server's row shows "Get ready · <size>" instead, and
-- nothing downloads until it's clicked. Over 4 GB always waits for the click.
--
-- One queue for every server, one item at a time (the menu's DownloadUGC does
-- one at a time). Nothing new is asked once a join starts: the join asks for
-- itself, and Steam carries on with anything downloading.
local WS_STALL = 120        -- seconds without a byte or an item: Steam isn't answering, stop asking
local WS_CHECK = 20         -- seconds to check a server's items before going on with what's known
local WS_TIME_SLACK = 256   -- seconds: FileInfo's float rounding (Content.newer)
local plans = {}            -- address -> what its content needs (below)
local allowed = {}          -- address -> true: "Get ready" clicked (this game start)
local run = { queue = {}, gen = 0, result = "idle" }
local adverts = {}          -- address -> { at, ids (nil: no advert) }
local asking = {}           -- address -> SysTime it was asked

local function planLine(address, p)
	return string.format("%s: %d Workshop items, %d needed an update, download or unpack (%s), %d done, %d failed, %.1f s",
		address, p.total, p.items or 0, Content.size(p.bytes), p.ok or 0, p.failed or 0, SysTime() - p.started)
end

local function planEnd(address, p, state)
	p.state = state
	run.result = (state == "stopped" and "stopped preparing " or "checked ") .. planLine(address, p)
	log("content: %s", run.result)
end

local function pump()
	if run.inFlight or IsInLoading() or IsInGame() then return end
	local job = table.remove(run.queue, 1)
	if not job then return end
	local p = plans[job.address]
	if not p or p.state ~= "going" then return pump() end
	run.inFlight, run.last, run.lastBytes = job, SysTime(), nil
	local gen = run.gen
	steamworks.DownloadUGC(job.id, function(path, f)
		if f and f.Close then f:Close() end
		if gen ~= run.gen then return end
		run.inFlight, run.last = nil, SysTime()
		local q = plans[job.address]
		if q == p and p.state == "going" then
			if path then
				p.ok = (p.ok or 0) + 1
				p.done = p.done + (p.need[job.id] or 0)
			else
				p.failed = (p.failed or 0) + 1
			end
			p.left = p.left - 1
			if p.left <= 0 then planEnd(job.address, p, "ready") end
		end
		pump()
	end)
end

-- Stop asking for anything new (a join started, or Steam stopped answering).
local function stopAll()
	run.gen = run.gen + 1
	run.queue, run.inFlight = {}, nil
	for address, p in pairs(plans) do
		if p.state == "going" or p.state == "checking" then planEnd(address, p, "stopped") end
	end
end

local function enqueue(address, p, first)
	-- A click goes before what was queued without one (after the item in flight).
	-- An item two servers share is asked for twice; the second ask finds it there.
	for i, id in ipairs(p.ids) do
		table.insert(run.queue, first and i or #run.queue + 1, { address = address, id = id })
	end
	pump()
end

local function planDecided(address, p)
	if plans[address] ~= p or p.state ~= "checking" then return end
	p.ids = {}
	for _, id in ipairs(p.order) do
		if p.need[id] then p.ids[#p.ids + 1] = id end
	end
	p.items, p.left = #p.ids, #p.ids
	if p.items == 0 then
		planEnd(address, p, "ready")
		return
	end
	if Content.consent({ ours = p.ours, known = p.known, bytes = p.bytes, asked = allowed[address] }) ~= "go" then
		p.state = "ask"
		log("content: %s needs %d of %d Workshop items (%s): waiting for Get ready", address, p.items, p.total, Content.size(p.bytes))
		return
	end
	p.state = "going"
	log("content: preparing %s: %d of %d Workshop items, %s", address, p.items, p.total, Content.size(p.bytes))
	enqueue(address, p, allowed[address])
end

local function decide(address, p, id, needs, size)
	if plans[address] ~= p or p.state ~= "checking" then return end
	p.pending = p.pending - 1
	if needs and not p.need[id] then
		p.need[id] = math.max(0, tonumber(size) or 0)
		p.bytes = p.bytes + p.need[id]
	end
	if p.pending <= 0 then planDecided(address, p) end
end

-- Check `address`'s items (the advert's `ids`, plus what its last join mounted).
local function planStart(address, ids)
	if not (prewarmWorkshopOn() and meowy.WorkshopItemState and steamworks and steamworks.DownloadUGC) then return end
	if IsInGame() or IsInLoading() then return end
	local learned = meowy.PrewarmWorkshop and meowy.PrewarmWorkshop(address)
	local order = Content.merge(ids, istable(learned) and learned or nil)
	if #order == 0 then return end
	local old = plans[address]
	if old and (old.state == "checking" or old.state == "going") then return end
	local p = {
		state = "checking", order = order, need = {}, bytes = 0, done = 0, total = #order, pending = #order,
		started = SysTime(),
		ours = meowy.IsOurServer and meowy.IsOurServer(address) == true,
		known = meowy.PrewarmRecorded and meowy.PrewarmRecorded(address) == true,
	}
	plans[address] = p
	for _, id in ipairs(order) do
		local installed, needsUpdate, busy, legacy, updated = meowy.WorkshopItemState(id)
		local unpacked = nil
		if legacy and file and file.Exists then unpacked = file.Exists("cache/workshop/" .. id .. ".gma", "MOD") end
		local needs = Content.needs(installed, needsUpdate, busy, legacy, unpacked)
		if needs == nil then
			decide(address, p, id, false)   -- Steam can't say: leave it to the join
		elseif steamworks.FileInfo then
			-- The size of what's needed, and for what's there, whether the Workshop has newer.
			steamworks.FileInfo(id, function(info)
				local remote = istable(info) and info or {}
				decide(address, p, id, needs or Content.newer(remote.updated, updated, WS_TIME_SLACK), remote.size)
			end)
		else
			decide(address, p, id, needs)
		end
	end
end

-- The advert: a recent answer, or ask (planStart runs when it comes, in Think).
local function contentFor(address)
	if not prewarmWorkshopOn() then return end
	local a = adverts[address]
	if a and SysTime() - a.at < Content.TTL then planStart(address, a.ids) return end
	if asking[address] then return end
	if meowy.ContentAsk and Home.publicAddress(address) and meowy.ContentAsk(address) then
		asking[address] = SysTime()
		return
	end
	planStart(address, nil)   -- not asked (not public, or too many in flight): what the last join learned
end

local function takeAdverts(now)
	if meowy.ContentTake then
		for _, a in ipairs(meowy.ContentTake() or {}) do
			local entry = { at = now }
			if a.value then
				local parsed, why = Content.parseAdvert(a.value)
				if parsed then entry.ids = parsed.ids else log("content: %s's advert isn't usable (%s)", a.address, why) end
			end
			adverts[a.address] = entry
			if asking[a.address] then
				asking[a.address] = nil
				planStart(a.address, entry.ids)
			end
		end
	end
	for address, at in pairs(asking) do   -- the worker always answers; this is only a backstop
		if now - at > 15 then
			asking[address] = nil
			adverts[address] = { at = now }
			planStart(address, nil)
		end
	end
end

local function warm(address, why)
	if not (Home.validAddress(address) and (prewarmOn() or prewarmWorkshopOn())) then return end
	if IsInGame() or IsInLoading() or address == pw.warmedFor then return end
	local recorded = meowy.PrewarmRecorded and meowy.PrewarmRecorded(address)
	-- A server never joined here: only its advert can say what it needs.
	if not recorded and not (prewarmWorkshopOn() and meowy.ContentAsk and Home.publicAddress(address)) then return end
	pw.warmedFor = address
	contentFor(address)
	if not (recorded and prewarmOn() and meowy.PrewarmStart) then return end
	local ok, reason = meowy.PrewarmStart(address)
	log("prewarm: %s %s%s", ok and "warming" or "not warming", address, ok and (" (" .. why .. ")") or (": " .. tostring(reason)))
end

-- The same for a server listed elsewhere on the home screen (Find servers,
-- menu_page.lua), by its address.
function MeowyClientPackWarm(address, why)
	if isstring(address) and isstring(why) then warm(address, why) end
end

-- "Get ready" clicked (the home card's row, or Find servers' picked server).
function MeowyClientPackPrepare(address)
	if not isstring(address) or not (Home.publicAddress(address) or (meowy.IsOurServer and meowy.IsOurServer(address))) then return end
	if IsInGame() or IsInLoading() or not prewarmWorkshopOn() then return end
	allowed[address] = true
	local p = plans[address]
	log("content: Get ready clicked for %s", address)
	if p and p.state == "ask" then
		p.state = "going"
		log("content: preparing %s: %d of %d Workshop items, %s", address, p.items, p.total, Content.size(p.bytes))
		enqueue(address, p, true)
	elseif not p or p.state == "stopped" then
		plans[address] = nil
		contentFor(address)
	end
end

-- What a server's row shows for its content (Content.label): text, isButton; nil for nothing.
function MeowyClientPackPrepLabel(address)
	local p = isstring(address) and plans[address]
	if not p then return nil end
	if p.state == "going" and run.inFlight and run.inFlight.address == address and meowy.WorkshopItemProgress then
		local done = meowy.WorkshopItemProgress(run.inFlight.id)
		local view = { state = p.state, items = p.items, bytes = p.bytes, done = p.done + math.min(tonumber(done) or 0, p.need[run.inFlight.id] or 0) }
		return Content.label(view)
	end
	return Content.label(p)
end

function MeowyClientPackHomeIntent(id)
	if not (homeOn() or menuPage.ours) then return end
	warm(addresses[math.floor(tonumber(id) or 0)], "pointer on its row")
end

-- Engine convars the pack changes for a while (fps_max_nofocus during a join,
-- fps_max_menu at a menu in the background): the player's own value is kept
-- with meowy.CvarHold until it's put back, so one a crash or a quit left
-- changed (the game saves config.cfg at quit) is put back at the next start.
local function numberText(v) return isstring(v) and v:match("^%d+%.?%d*$") ~= nil end
local function cvarText(name) return GetConVarString and GetConVarString(name) end
local function cvarChange(name, value, own)
	if not (meowy.ConsoleRun and numberText(own)) then return false end
	if meowy.CvarHold and not meowy.CvarHold(name, own) then return false end
	meowy.ConsoleRun(name .. " " .. value)
	return true
end
local function cvarPutBack(name, own)
	if meowy.ConsoleRun then meowy.ConsoleRun(name .. " " .. own) end
	if meowy.CvarRelease then meowy.CvarRelease(name) end
end
do
	for name, own in pairs(meowy.CvarHeld and meowy.CvarHeld() or {}) do
		cvarPutBack(name, own)
		log("%s put back to %s (the game closed while the pack had it changed)", name, own)
	end
end

-- The join priority guard: while a join loads, Windows' own QoS tag at full
-- speed (meowy.JoinBoost) and fps_max_nofocus lifted to fps_max, both put back
-- once in game or when the join ends.
local function boostOn()
	if not (joinBoostOn() and meowy.JoinBoost) then return end
	local nofocus, fpsMax = cvarText("fps_max_nofocus"), cvarText("fps_max")
	meowy.JoinBoost(true)
	pw.boosted = true
	if numberText(nofocus) and numberText(fpsMax) and tonumber(nofocus) > 0
		and cvarChange("fps_max_nofocus", fpsMax, nofocus) then
		pw.nofocus = nofocus
	end
end
local function boostOff()
	if not pw.boosted then return end
	pw.boosted = false
	if pw.nofocus then cvarPutBack("fps_max_nofocus", pw.nofocus) end
	pw.nofocus = nil
	if meowy.JoinBoost then meowy.JoinBoost(false) end
end

-- Idle savings (meowy_idle_savings).
-- The loading screen's page (WebView2: six processes, ~150 MB private, ~310 MB
-- working set) is kept loaded only while a join can come from the menu: from a
-- few seconds after start-up (not competing with it), while the game has focus
-- and for two minutes after it loses it. In game, and at a menu left in the
-- background longer, it's unloaded; a join starts it on its own, the native
-- drawing showing for the second that takes.
-- With fps_max_nofocus at 0 (no cap of its own in the background), a menu in
-- the background runs at fps_max_menu (60 by default): held to 15 fps there,
-- put back on focus or when a join starts.
local idleSetting = meowy.Setting and meowy.Setting("meowy_idle_savings", "1",
	"Meowy Client Pack: unload the loading screen's browser while you play, and slow the menu down while the game is in the background (0 = off)")
local function idleOn() return idleSetting == nil or idleSetting:GetBool() end
local MENU_BACKGROUND_FPS, WEB_AFTER_START, WEB_AWAY_KEEP = 15, 5, 120
local idle = { started = SysTime(), next = 0 }
local function hasFocus() return not (system and system.HasFocus) or system.HasFocus() end
local function loadingScreenOn()
	local c = GetConVar and GetConVar("meowy_loadingscreen")
	return c ~= nil and c:GetBool() and not (system and system.IsWindowed and not system.IsWindowed())
end
local function menuCap(on)
	if on and not idle.capped then
		local own = cvarText("fps_max_menu")
		local n = tonumber(own)
		if n and (n == 0 or n > MENU_BACKGROUND_FPS) and cvarChange("fps_max_menu", MENU_BACKGROUND_FPS, own) then
			idle.capped = own
		end
	elseif not on and idle.capped then
		cvarPutBack("fps_max_menu", idle.capped)
		idle.capped = nil
	end
end
local function loadingPage(want)
	if want == idle.web then return end
	idle.web = want
	if want and meowy.LoadingPrewarm then meowy.LoadingPrewarm()
	elseif not want and meowy.LoadingPark then meowy.LoadingPark() end
end
hook.Add("Think", "MeowyClientPack.IdleSavings", function()
	local now = SysTime()
	if now < idle.next then return end
	idle.next = now + 0.25
	local loading, inGame, focus = IsInLoading(), IsInGame(), hasFocus()
	local atMenu = not loading and not inGame
	if focus or not atMenu then idle.awaySince = nil else idle.awaySince = idle.awaySince or now end
	local on = idleOn()
	menuCap(on and atMenu and not focus and cvarText("fps_max_nofocus") == "0")
	if loading then return end   -- a join keeps the page it has, or starts it
	if not loadingScreenOn() then loadingPage(false)
	elseif not on then loadingPage(true)
	else
		loadingPage(atMenu and now - idle.started >= WEB_AFTER_START
			and (idle.awaySince == nil or now - idle.awaySince < WEB_AWAY_KEEP))
	end
end)

function MeowyClientPackHomePrepare(id)
	if not (homeOn() or menuPage.ours) then return end
	MeowyClientPackPrepare(addresses[math.floor(tonumber(id) or 0)])
end

hook.Add("Think", "MeowyClientPack.Prewarm", function()
	if not meowy.PrewarmBeginJoin then return end
	local now = SysTime()
	local loading = IsInLoading()
	takeAdverts(now)
	-- Steam answering means bytes moving: a big item may take minutes.
	if run.inFlight and meowy.WorkshopItemProgress then
		local bytes = meowy.WorkshopItemProgress(run.inFlight.id)
		if bytes and bytes ~= run.lastBytes then run.lastBytes, run.last = bytes, now end
	end
	if (run.inFlight and now - run.last > WS_STALL) or (loading and (run.inFlight or #run.queue > 0)) then stopAll() end
	for address, p in pairs(plans) do   -- FileInfo that never answered: go on with what's known
		if p.state == "checking" and now - p.started > WS_CHECK then p.pending = 0 planDecided(address, p) end
	end
	if loading and not pw.joining then
		pw.joining = true
		boostOn()
		stopAll()
		if pakCacheOn() and meowy.PakCache then
			local ok, why = meowy.PakCache(true)
			if not ok and not pw.pakWhyLogged then pw.pakWhyLogged = true log("pak cache: off (%s)", tostring(why)) end
		end
		meowy.PrewarmBeginJoin()   -- also stops any warming: the join's reads come first
	elseif pw.joining and IsInGame() and not loading then
		pw.joining = false
		boostOff()
		if meowy.PakCacheState and meowy.PakCache then
			log("pak cache: %s", meowy.PakCacheState())
			meowy.PakCache(false)
		end
		local kept = meowy.PrewarmEndJoin()
		pw.warmedFor = nil
		if kept > 0 then log("prewarm: kept %d files for this server", kept) end
	elseif not loading and not IsInGame() then
		if pw.joining and meowy.PakCache then meowy.PakCache(false) end
		if pw.joining then boostOff() end
		pw.joining = false   -- a join that didn't get in: nothing to keep
	end
	-- A few seconds at the main menu: the Continue server, once per visit (a
	-- row the pointer rests on afterwards takes over); and our server's
	-- content, which is prepared without asking (Content.consent).
	if menuVisible() and not IsInGame() and not loading then
		pw.menuSince = pw.menuSince or now
		if not pw.continueDone and now - pw.menuSince > 3 then
			pw.continueDone = true
			local address = meowy.LastServer and meowy.LastServer()
			if isstring(address) then warm(address, "your Continue server") end
			local ours = st.oursAddress
			if (homeOn() or menuPage.ours) and Home.validAddress(ours) and ours ~= address then contentFor(ours) end
		end
	else
		pw.menuSince, pw.continueDone = nil, false
	end
end)

if concommand then
	concommand.Add("meowy_prewarm_status", function()
		MsgN("[Meowy Client Pack] prewarm: " .. (meowy.PrewarmState and meowy.PrewarmState() or "unavailable"))
		MsgN("[Meowy Client Pack] prewarm workshop: " .. run.result .. string.format(" (%d queued%s)", #run.queue,
			run.inFlight and (", downloading " .. run.inFlight.id .. " for " .. run.inFlight.address) or ""))
		for address, p in pairs(plans) do
			MsgN(string.format("[Meowy Client Pack]   %s: %s, %s", address, p.state, (Content.label(p)) or planLine(address, p)))
		end
		MsgN("[Meowy Client Pack] content adverts: " .. (meowy.ContentState and meowy.ContentState() or "unavailable"))
		MsgN("[Meowy Client Pack] pak cache: " .. (meowy.PakCacheState and meowy.PakCacheState() or "unavailable"))
		MsgN("[Meowy Client Pack] join boost: " .. (joinBoostOn() and "on during joins" or "off") .. "; Windows has the game at " ..
			(meowy.JoinBoostState and (meowy.JoinBoostState() == "on" and "full speed" or "its own policy") or "unknown"))
		MsgN("[Meowy Client Pack] idle savings: " .. (idleOn() and "on" or "off") .. "; loading screen page " ..
			(idle.web and "kept loaded" or "unloaded") .. "; menu frame rate " .. (idle.capped and ("held to " .. MENU_BACKGROUND_FPS .. " in the background") or "as set"))
	end, nil, "Meowy Client Pack: what join pre-warm is doing")
	-- meowy_content_get_ready <address>: "Get ready" for a server, from the console.
	-- (The whole argument string: the console splits "a.b.c.d:port" at the colon.)
	concommand.Add("meowy_content_get_ready", function(_, _, _, argStr)
		MeowyClientPackPrepare(isstring(argStr) and (argStr:gsub("%s", "")) or nil)
	end, nil, "Meowy Client Pack: download a server's Workshop content now (its advert and last join's list)")
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
		-- Each row's content line (Content.label): "Get ready · 1.3 GB" (a button) or progress.
		for _, sec in ipairs(model.sections) do
			for _, r in ipairs(sec.rows) do
				local text, button = MeowyClientPackPrepLabel(model.addresses[r.id])
				if text then r.prep, r.prepAsk = text, button or nil end
			end
		end
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
