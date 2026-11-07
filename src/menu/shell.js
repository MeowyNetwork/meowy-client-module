// Meowy home screen (meowy_menu, on by default): our home inside Garry's Mod's own
// menu page. Nothing is reloaded: the stock main screen's list and news are
// hidden by a style rule while this is on, our home takes their place, and the
// links are the page's own routes (#/servers/ and so on), so going there and
// back is instant. Sent by menu_page.lua every second with
// window.MeowyShell(on); safe to run again and again.
//
// Data: window.MeowyHomeData (home.lua's lists) and window.MeowyContinue
// (resilience.lua's Continue entry), each followed by a call to
// MeowyShellLists / MeowyShellContinue. Every server string is set as text.
// The only way back to Lua is meowy.post(kind, value) (menu_page.lua checks
// both), or the page's own routes.
(function () {
	if (window.MeowyShell) return;

	var MAIN = '.page[ng-controller="ControllerMain"]';
	var STYLE =
		// While on: the stock main screen's list, news and our stock-page Home card are hidden.
		'body.meowy-shell ' + MAIN + ' > .options, body.meowy-shell ' + MAIN + ' > .news, body.meowy-shell #meowy-home { display: none !important; }'
		+ '#meowy-shell { position: absolute; left: 0; top: 0; right: 0; bottom: 0; color: #fff; text-shadow: 1px 1px 2px rgba(0,0,0,0.8); }'
		+ '#meowy-shell .ms-nav { position: absolute; left: 48px; top: 48px; bottom: 24px; width: 380px; }'
		+ '#meowy-shell .ms-brand { font-size: 40px; font-weight: bold; letter-spacing: -1px; }'
		+ '#meowy-shell .ms-tag { font-size: 13px; opacity: 0.7; margin: 2px 0 24px 2px; }'
		+ '#meowy-shell .ms-link { display: block; font-size: 25px; font-weight: bold; padding: 4px 12px; margin: 0 0 2px -12px; border-radius: 6px; cursor: pointer; outline: none; color: #fff; text-decoration: none; }'
		+ '#meowy-shell .ms-link:hover, #meowy-shell .ms-link:focus { background: rgba(255,255,255,0.14); color: #ffa; }'
		+ '#meowy-shell .ms-gap { height: 16px; }'
		+ '#meowy-shell .ms-cont { display: none; }'
		+ '#meowy-shell .ms-cont.on { display: block; }'
		+ '#meowy-shell .ms-csub { display: block; font-size: 15px; font-weight: normal; opacity: 0.85; white-space: nowrap; }'
		+ '#meowy-shell .ms-cserver { display: inline-block; max-width: 280px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; vertical-align: bottom; }'
		+ '#meowy-shell .ms-foot { position: absolute; left: 48px; bottom: 12px; font-size: 13px; opacity: 0.7; cursor: pointer; text-decoration: underline; }'
		+ '#meowy-shell .ms-home { position: absolute; right: 40px; top: 48px; bottom: 16px; width: 520px; background: rgba(0,0,0,0.55); border-radius: 8px; padding: 10px; box-sizing: border-box; overflow-y: auto; }'
		+ '#meowy-shell .ms-search { display: block; width: 100%; box-sizing: border-box; padding: 8px 12px; font-size: 16px; border: 0; border-radius: 5px; background: rgba(255,255,255,0.12); color: #fff; outline: none; }'
		+ '#meowy-shell .ms-search:focus { background: rgba(255,255,255,0.2); }'
		+ '#meowy-shell .ms-search::-webkit-input-placeholder { color: rgba(255,255,255,0.55); }'
		+ '#meowy-shell .ms-title { font-size: 13px; font-weight: bold; letter-spacing: 1px; text-transform: uppercase; opacity: 0.7; margin: 14px 12px 4px 12px; }'
		+ '#meowy-shell .ms-note { font-size: 14px; opacity: 0.6; margin: 3px 12px 6px 12px; }'
		+ '#meowy-shell .ms-row { cursor: pointer; padding: 6px 12px; border-radius: 5px; overflow: hidden; }'
		+ '#meowy-shell .ms-row:hover, #meowy-shell .ms-row.ms-sel { background: rgba(255,255,255,0.16); }'
		+ '#meowy-shell .ms-row.ms-dim { opacity: 0.5; }'
		+ '#meowy-shell .ms-stats { float: right; font-size: 14px; margin-left: 12px; opacity: 0.85; line-height: 22px; }'
		+ '#meowy-shell .ms-name { font-size: 18px; font-weight: bold; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; line-height: 22px; }'
		+ '#meowy-shell .ms-detail { font-size: 14px; opacity: 0.75; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }'
		// Find servers (the browse view): in place of the lists panel, wider.
		+ '#meowy-shell .ms-browse { display: none; position: absolute; left: 460px; right: 40px; top: 48px; bottom: 16px; background: rgba(0,0,0,0.6); border-radius: 8px; padding: 10px; box-sizing: border-box; }'
		+ '#meowy-shell.ms-browsing .ms-browse { display: block; }'
		+ '#meowy-shell.ms-browsing .ms-home { display: none; }'
		+ '#meowy-shell .mb-bar { margin-bottom: 6px; white-space: nowrap; overflow: hidden; }'
		+ '#meowy-shell .mb-bar .ms-search { display: inline-block; width: 40%; vertical-align: middle; margin-right: 8px; }'
		+ '#meowy-shell .mb-chip { display: inline-block; vertical-align: middle; font-size: 14px; padding: 5px 10px; margin: 2px 4px 2px 0; border-radius: 12px; background: rgba(255,255,255,0.1); cursor: pointer; }'
		+ '#meowy-shell .mb-chip:hover { background: rgba(255,255,255,0.2); }'
		+ '#meowy-shell .mb-chip.on { background: rgba(255,255,140,0.35); }'
		+ '#meowy-shell .mb-status { font-size: 13px; opacity: 0.8; margin: 4px 4px 6px 4px; }'
		+ '#meowy-shell .mb-list { position: absolute; left: 10px; right: 10px; top: 104px; bottom: 10px; overflow-y: auto; }'
		+ '#meowy-shell .mb-row { cursor: pointer; padding: 5px 8px; border-radius: 4px; overflow: hidden; }'
		+ '#meowy-shell .mb-row:hover, #meowy-shell .mb-row.ms-sel { background: rgba(255,255,255,0.16); }'
		+ '#meowy-shell .mb-right { float: right; font-size: 14px; margin-left: 12px; text-align: right; line-height: 20px; opacity: 0.9; }'
		+ '#meowy-shell .mb-name { font-size: 16px; font-weight: bold; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; line-height: 20px; }'
		+ '#meowy-shell .mb-sub { font-size: 13px; opacity: 0.7; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }';
	// [label, kind, value]: "route" goes to the page's own screen; the rest are posts.
	var LINKS = [
		['Find servers', 'route', 'servers'], ['Start new game', 'route', 'newgame'], null,
		['Addons', 'route', 'addons'], ['Dupes', 'route', 'dupes'], ['Saves', 'route', 'saves'], ['Demos', 'route', 'demos'], null,
		['Options', 'options', ''], ['Meowy settings', 'settings', ''], null,
		['Quit', 'quit', '']
	];
	var MAX_MATCHES = 10;
	var MAX_MAPS = 8, MAX_NEWS = 3, MAX_ADDONS = 6;
	// Settings the search finds: [title, where it opens, words it answers to].
	var SETTINGS = [
		['Options', 'options', 'options video graphics resolution display audio sound volume voice microphone keyboard keys bindings mouse sensitivity controls multiplayer spray language'],
		['Meowy settings', 'settings', 'meowy settings pack stay connected reconnect start with the game loading screen continue home screen menu escape server card text size zoom scale pre-warm prewarm workshop find servers players fake counts']
	];

	var on = false, query = '', selected = 0, rows = [], focused = false;

	function post(kind, value) {
		if (window.meowy && typeof window.meowy.post === 'function') window.meowy.post(kind, value === undefined ? '' : value);
	}
	function node(tag, cls, text) {
		var e = document.createElement(tag);
		if (cls) e.className = cls;
		if (text) e.textContent = text;
		return e;
	}
	function shell() { return document.getElementById('meowy-shell'); }
	// Resting the pointer on a row for a moment: get that server ready (prewarm.h).
	function intentOn(el, kind, id) {
		var timer = null;
		el.onmouseenter = function () { timer = setTimeout(function () { post(kind, Number(id)); }, 400); };
		el.onmouseleave = function () { if (timer) clearTimeout(timer); timer = null; };
	}
	function intent(el, id) { intentOn(el, 'intent', id); }
	function setBodyClass(want) {
		var b = document.body;
		if (!b) return;
		var has = (' ' + b.className + ' ').indexOf(' meowy-shell ') >= 0;
		if (want && !has) b.className = (b.className ? b.className + ' ' : '') + 'meowy-shell';
		if (!want && has) b.className = (' ' + b.className + ' ').replace(' meowy-shell ', ' ').replace(/^\s+|\s+$/g, '');
	}

	function link(label, kind, value) {
		var a = node('a', 'ms-link', label);
		a.tabIndex = 0;
		var act = function () {
			if (kind === 'route') location.hash = '#/' + value + '/';
			else if (kind === 'browse') browseOpen(true);
			else post(kind, value);
		};
		a.onclick = act;
		a.onkeydown = function (e) { if (e.keyCode === 13) { act(); e.preventDefault(); } };
		return a;
	}

	function build() {
		var root = node('div');
		root.id = 'meowy-shell';
		var nav = node('div', 'ms-nav');
		nav.appendChild(node('div', 'ms-brand', "Garry's Mod"));
		nav.appendChild(node('div', 'ms-tag', 'Meowy home'));
		var cont = node('a', 'ms-link ms-cont');
		cont.tabIndex = 0;
		cont.appendChild(node('span', 'ms-ctitle', 'Continue'));
		var csub = node('span', 'ms-csub');
		csub.appendChild(node('span', 'ms-cserver'));
		csub.appendChild(node('span', 'ms-cwhen'));
		cont.appendChild(csub);
		cont.onclick = function () { post('continue'); };
		cont.onkeydown = function (e) { if (e.keyCode === 13) { post('continue'); e.preventDefault(); } };
		nav.appendChild(cont);
		for (var i = 0; i < LINKS.length; i++) {
			if (!LINKS[i]) { nav.appendChild(node('div', 'ms-gap')); continue; }
			nav.appendChild(link(LINKS[i][0], LINKS[i][1], LINKS[i][2]));
		}
		root.appendChild(nav);
		var foot = node('div', 'ms-foot', "Use Garry's Mod's menu");
		foot.onclick = function () { post('stock'); };
		root.appendChild(foot);
		var home = node('div', 'ms-home');
		var search = node('input', 'ms-search');
		search.type = 'text';
		search.maxLength = 64;
		search.placeholder = 'Search servers, maps, addons, settings   ( / )';
		search.value = query;
		search.oninput = function () { query = search.value; selected = 0; lists(); };
		search.onfocus = function () { focused = true; mark(); };
		search.onblur = function () { focused = false; mark(); };
		search.onkeydown = keys;
		home.appendChild(search);
		home.appendChild(node('div', 'ms-lists'));
		home.appendChild(node('div', 'ms-news'));
		root.appendChild(home);
		root.appendChild(buildBrowse());
		if (br.open) root.className = 'ms-browsing';
		return root;
	}

	// --- Find servers (the browse view) -------------------------------------------
	// One list straight from Steam (servers with players), sent by menu_page.lua in
	// batches with window.MeowyBrowse.add; filtered and sorted here, at most
	// BROWSE_SHOWN rows drawn. A row is joined by its number (the address stays in Lua).
	var BROWSE_SHOWN = 200, BROWSE_STALE = 120000;
	var CATS = [['rp', 'Roleplay'], ['pvp', 'PvP'], ['pve', 'PvE'], ['other', 'Other']];
	var br = { open: false, gen: 0, rows: [], done: false, ok: true, secs: 0, at: 0, q: '', notFull: false, noPass: false,
		cat: '', byPing: false, shown: [], sel: 0, timer: null };

	function chip(label, isOn, onClick) {
		var c = node('span', 'mb-chip' + (isOn ? ' on' : ''), label);
		c.onclick = onClick;
		return c;
	}
	function buildBrowse() {
		var panel = node('div', 'ms-browse');
		var bar = node('div', 'mb-bar');
		var filter = node('input', 'ms-search mb-filter');
		filter.type = 'text';
		filter.maxLength = 64;
		filter.placeholder = 'Filter by name, map or gamemode';
		filter.value = br.q;
		filter.oninput = function () { br.q = filter.value; br.sel = 0; browseRender(); };
		filter.onkeydown = browseKeys;
		bar.appendChild(filter);
		panel.appendChild(bar);
		panel.appendChild(node('div', 'mb-chips'));
		panel.appendChild(node('div', 'mb-status'));
		panel.appendChild(node('div', 'mb-list'));
		return panel;
	}
	function browseChips() {
		var s = shell();
		if (!s) return;
		var box = s.querySelector('.mb-chips');
		while (box.firstChild) box.removeChild(box.firstChild);
		box.appendChild(chip('Not full', br.notFull, function () { br.notFull = !br.notFull; browseRender(); }));
		box.appendChild(chip('No password', br.noPass, function () { br.noPass = !br.noPass; browseRender(); }));
		for (var i = 0; i < CATS.length; i++) {
			box.appendChild(chip(CATS[i][1], br.cat === CATS[i][0], (function (c) {
				return function () { br.cat = br.cat === c ? '' : c; browseRender(); };
			})(CATS[i][0])));
		}
		box.appendChild(chip(br.byPing ? 'Sorted by ping' : 'Sorted by players', false, function () { br.byPing = !br.byPing; browseRender(); }));
		box.appendChild(chip('Refresh', false, function () { post('browse'); }));
		box.appendChild(chip("Garry's Mod's list", false, function () { browseOpen(false); location.hash = '#/servers/'; }));
		box.appendChild(chip('Back', false, function () { browseOpen(false); }));
	}
	function browseOpen(open) {
		br.open = open;
		var s = shell();
		if (s) s.className = open ? 'ms-browsing' : '';
		if (open) {
			if (!br.rows.length || Date.now() - br.at > BROWSE_STALE) post('browse');
			browseRender();
			var f = s && s.querySelector('.mb-filter');
			if (f && f.focus) f.focus();
		} else {
			post('browse_stop');
		}
	}
	function browseRender() {
		var s = shell();
		if (!s || !br.open) return;
		browseChips();
		var q = br.q.toLowerCase(), list = s.querySelector('.mb-list'), status = s.querySelector('.mb-status');
		var picked = [];
		for (var i = 0; i < br.rows.length; i++) {
			var r = br.rows[i];
			if (br.notFull && r.p >= r.x) continue;
			if (br.noPass && r.pw) continue;
			if (br.cat && r.c !== br.cat) continue;
			if (q && (r.n + ' ' + r.m + ' ' + r.g).toLowerCase().indexOf(q) < 0) continue;
			picked.push(r);
		}
		picked.sort(br.byPing
			? function (a, b) { return a.ms - b.ms || b.p - a.p; }
			: function (a, b) { return b.p - a.p || a.ms - b.ms; });
		while (list.firstChild) list.removeChild(list.firstChild);
		br.shown = picked.slice(0, BROWSE_SHOWN);
		for (var k = 0; k < br.shown.length; k++) {
			var row = br.shown[k], el = node('div', 'mb-row');
			el.appendChild(node('span', 'mb-right', row.p + '/' + row.x + (row.pw ? ' · password' : '') + ' · ' + row.ms + ' ms'));
			el.appendChild(node('div', 'mb-name', row.n));
			el.appendChild(node('div', 'mb-sub', row.g + (row.m ? ' · ' + row.m : '')));
			el.title = row.n;
			el.onclick = (function (id) { return function () { post('browse_join', id); }; })(row.i);
			intentOn(el, 'browse_intent', row.i);
			list.appendChild(el);
		}
		if (br.sel >= br.shown.length) br.sel = br.shown.length - 1;
		if (br.sel < 0) br.sel = 0;
		browseMark();
		var text;
		if (!br.ok) text = "Steam's server list isn't available here: use Garry's Mod's list.";
		else if (br.done) text = br.rows.length + ' servers with players, complete in ' + br.secs + ' s';
		else text = br.rows.length + ' servers with players so far…';
		if (picked.length !== br.rows.length) text += ' · ' + picked.length + ' match';
		if (picked.length > br.shown.length) text += ', the first ' + br.shown.length + ' shown';
		status.textContent = text;
	}
	function browseMark() {
		var s = shell(), els = s ? s.querySelectorAll('.mb-row') : [];
		for (var i = 0; i < els.length; i++) {
			els[i].className = 'mb-row' + (i === br.sel ? ' ms-sel' : '');
			if (i === br.sel && els[i].scrollIntoView) els[i].scrollIntoView(false);
		}
	}
	// In the filter box: arrows move, Enter joins, Esc clears (then goes back).
	function browseKeys(e) {
		var k = e.keyCode, f = e.target;
		if (k === 40 || k === 38) {
			br.sel = Math.max(0, Math.min(br.shown.length - 1, br.sel + (k === 40 ? 1 : -1)));
			browseMark();
		} else if (k === 13) {
			if (br.shown[br.sel]) post('browse_join', br.shown[br.sel].i);
		} else if (k === 27) {
			if (f.value) { f.value = ''; br.q = ''; br.sel = 0; browseRender(); } else browseOpen(false);
		} else {
			return;
		}
		e.preventDefault();
		e.stopPropagation();
	}
	function browseSchedule() {
		if (br.timer) return;
		br.timer = setTimeout(function () { br.timer = null; browseRender(); }, 150);
	}
	// Find servers opens Garry's Mod's browser while this view is rebuilt; open() shows it.
	window.MeowyBrowse = {
		open: function (on) { browseOpen(on !== false); },
		reset: function (gen, ok) {
			br.gen = gen; br.rows = []; br.done = !ok; br.ok = !!ok; br.secs = 0; br.at = Date.now(); br.sel = 0;
			browseRender();
		},
		add: function (gen, rows, finished, secs) {
			if (gen !== br.gen || !rows) return;
			for (var i = 0; i < rows.length; i++) {
				var r = rows[i];
				if (r && typeof r.i === 'number' && typeof r.n === 'string') br.rows.push(r);
			}
			if (finished) br.done = true;
			br.secs = secs;
			browseSchedule();
		},
		flush: function () { if (br.timer) { clearTimeout(br.timer); br.timer = null; } browseRender(); }
	};

	// One row: a name, an optional detail and stats; act() is what Enter or a click does.
	function addRow(list, name, detail, stats, dim, act, intentId) {
		var cls = 'ms-row' + (dim ? ' ms-dim' : '');
		var el = node('div', cls);
		if (stats) el.appendChild(node('span', 'ms-stats', stats));
		el.appendChild(node('div', 'ms-name', name));
		if (detail) el.appendChild(node('div', 'ms-detail', detail));
		el.title = name;
		el.onclick = act;
		if (intentId !== undefined) intent(el, intentId);
		list.appendChild(el);
		rows.push({ el: el, cls: cls, act: act });
	}

	// The page's own map list (gScope.MapList: Garry's Mod's Lua sends it for New
	// game). A map is picked the way New game picks it (its root scope's Map and
	// LastCategory), then New game opens with it chosen.
	function pickMap(map, category) {
		var r = window.rootScope;
		if (!r) return;
		r.Map = map;
		r.LastCategory = category;
		if (typeof window.UpdateDigest === 'function') window.UpdateDigest(r, 50);
		location.hash = '#/newgame/';
	}
	function mapMatches(q) {
		var out = [], seen = {}, list = window.gScope && window.gScope.MapList;
		if (!q || !list || !list.length) return out;
		for (var i = 0; i < list.length && out.length < MAX_MAPS; i++) {
			var maps = list[i] && list[i].maps;
			if (!maps || list[i].category === 'Favourites') continue;   // also listed in their own category
			for (var j = 0; j < maps.length && out.length < MAX_MAPS; j++) {
				var m = maps[j];
				if (typeof m !== 'string' || seen[m] || m.toLowerCase().indexOf(q) < 0) continue;
				seen[m] = true;
				out.push([m, String(list[i].category)]);
			}
		}
		return out;
	}

	// Garry's Mod's news, as the stock main screen had it (rootScope.NewsList,
	// from its own feed); opened the way the stock page opens it. Titles as text.
	var newsShown = null;
	function newsAgo(date) {
		var t = Date.parse(date);
		if (isNaN(t)) return '';
		var days = Math.floor((Date.now() - t) / 86400000);
		if (days < 1) return 'today';
		if (days < 2) return 'yesterday';
		if (days < 60) return days + ' days ago';
		return new Date(t).toDateString().replace(/^\S+\s/, '');
	}
	function news() {
		var s = shell();
		if (!s) return;
		var box = s.querySelector('.ms-news'), items = (window.rootScope && window.rootScope.NewsList) || [];
		var picked = [];
		for (var i = 0; i < items.length && picked.length < MAX_NEWS; i++) {
			if (items[i] && typeof items[i].Title === 'string' && typeof items[i].Url === 'string') picked.push(items[i]);
		}
		var key = query ? '' : picked.map(function (n) { return n.Title + '|' + n.Date; }).join('\n');
		if (key === newsShown) return;
		newsShown = key;
		while (box.firstChild) box.removeChild(box.firstChild);
		if (!key) return;   // hidden while searching, or no news yet
		box.appendChild(node('div', 'ms-title', 'News'));
		for (var k = 0; k < picked.length; k++) {
			var item = node('div', 'ms-row');
			item.appendChild(node('span', 'ms-stats', newsAgo(picked[k].Date)));
			item.appendChild(node('div', 'ms-name', picked[k].Title));
			item.title = picked[k].Title;
			item.onclick = (function (url) {
				return function () { if (window.scope && typeof window.scope.OpenInSteam === 'function') window.scope.OpenInSteam(url); };
			})(picked[k].Url);
			box.appendChild(item);
		}
	}

	function mark() {
		for (var i = 0; i < rows.length; i++) {
			var sel = focused && i === selected;   // the row Enter joins, while the search box has the keyboard
			rows[i].el.className = rows[i].cls + (sel ? ' ms-sel' : '');
			if (sel && rows[i].el.scrollIntoView) rows[i].el.scrollIntoView(false);
		}
	}

	// The lists: each section's first rows; while searching, every row whose
	// name or map has the text (at most MAX_MATCHES a section), then the
	// installed maps whose name has it.
	function lists() {
		var s = shell();
		if (!s) return;
		var list = s.querySelector('.ms-lists');
		var data = window.MeowyHomeData, q = query.toLowerCase();
		while (list.firstChild) list.removeChild(list.firstChild);
		rows = [];
		var sections = (data && data.sections) || [];
		for (var i = 0; i < sections.length; i++) {
			var sec = sections[i], picked = [];
			for (var j = 0; j < sec.rows.length; j++) {
				var r = sec.rows[j];
				if (q && (r.name + ' ' + r.detail).toLowerCase().indexOf(q) < 0) continue;
				picked.push(r);
				if (picked.length >= (q ? MAX_MATCHES : Math.max(sec.show, 8))) break;
			}
			if (q && !picked.length) continue;
			list.appendChild(node('div', 'ms-title', sec.title));
			for (var k = 0; k < picked.length; k++) {
				var p = picked[k];
				addRow(list, p.name, p.detail, p.stats, p.dim,
					(function (id) { return function () { post('join', Number(id)); }; })(p.id), p.id);
			}
			if (!q && sec.note) list.appendChild(node('div', 'ms-note', sec.note));
		}
		var maps = mapMatches(q);
		if (maps.length) {
			list.appendChild(node('div', 'ms-title', 'Maps'));
			for (var m = 0; m < maps.length; m++) {
				addRow(list, maps[m][0], maps[m][1] + ' · start a new game', '', false,
					(function (map, cat) { return function () { pickMap(map, cat); }; })(maps[m][0], maps[m][1]));
			}
		}
		// Your subscribed addons (menu_page.lua sends their titles): Addons opens.
		var addons = q && window.MeowyAddons, shown = 0;
		for (var a = 0; addons && a < addons.length && shown < MAX_ADDONS; a++) {
			var ad = addons[a];
			if (!ad || typeof ad.t !== 'string' || ad.t.toLowerCase().indexOf(q) < 0) continue;
			if (!shown) list.appendChild(node('div', 'ms-title', 'Addons'));
			shown++;
			addRow(list, ad.t, ad.m ? 'Subscribed, on' : 'Subscribed, off', '', !ad.m,
				function () { location.hash = '#/addons/'; });
		}
		// Settings: by their names and what they cover.
		var setFirst = true;
		for (var st = 0; q && st < SETTINGS.length; st++) {
			if ((SETTINGS[st][0] + ' ' + SETTINGS[st][2]).toLowerCase().indexOf(q) < 0) continue;
			if (setFirst) { list.appendChild(node('div', 'ms-title', 'Settings')); setFirst = false; }
			addRow(list, SETTINGS[st][0], '', '', false, (function (kind) { return function () { post(kind); }; })(SETTINGS[st][1]));
		}
		if (q && !rows.length) list.appendChild(node('div', 'ms-note', 'Nothing matches'));
		if (selected >= rows.length) selected = rows.length - 1;
		if (selected < 0) selected = 0;
		mark();
		news();
	}

	function continueEntry() {
		var s = shell();
		if (!s) return;
		var c = s.querySelector('.ms-cont'), d = window.MeowyContinue;
		c.className = 'ms-link ms-cont' + (d ? ' on' : '');
		if (!d) return;
		c.querySelector('.ms-ctitle').textContent = d[0];
		var server = c.querySelector('.ms-cserver');
		server.textContent = d[1];
		server.title = d[1];
		c.querySelector('.ms-cwhen').textContent = d[2];
	}

	// In the search box: arrows move, Enter joins, Esc clears.
	function keys(e) {
		var k = e.keyCode, search = e.target;
		if (k === 40 || k === 38) {
			selected = Math.max(0, Math.min(rows.length - 1, selected + (k === 40 ? 1 : -1)));
			mark();
		} else if (k === 13) {
			if (rows[selected]) rows[selected].act();
		} else if (k === 27) {
			search.value = '';
			query = '';
			selected = 0;
			lists();
			search.blur();
		} else {
			return;
		}
		e.preventDefault();
		e.stopPropagation();
	}

	// Put in (or take out) our home on the stock main screen. Runs on every
	// change to the page, so it's back the moment the main screen is drawn again.
	function draw() {
		var page = document.querySelector(MAIN);
		var s = shell();
		if (!on) {
			if (s) s.parentNode.removeChild(s);
			setBodyClass(false);
			return;
		}
		if (!document.getElementById('meowy-shell-style')) {
			var css = node('style');
			css.id = 'meowy-shell-style';
			css.textContent = STYLE;
			(document.head || document.body).appendChild(css);
		}
		setBodyClass(true);
		if (!page) return;
		if (s && s.parentNode !== page) { s.parentNode.removeChild(s); s = null; }
		if (!s) {
			page.appendChild(build());
			lists();
			continueEntry();
		}
	}

	window.MeowyShell = function (v) {
		var want = !!v;
		if (on && want) news();   // the feed arrives after the page: picked up on the next send
		if (want === on && (!on || shell() || !document.querySelector(MAIN))) return;
		on = want;
		newsShown = null;
		draw();
	};
	window.MeowyShellLists = lists;
	window.MeowyShellContinue = continueEntry;

	// "/" anywhere on our home (not while typing elsewhere) goes to the search box.
	document.addEventListener('keydown', function (e) {
		var s = shell(), t = e.target && e.target.tagName;
		if (!s || t === 'INPUT' || t === 'TEXTAREA') return;
		if (e.key === '/' || e.keyCode === 191) { s.querySelector(br.open ? '.mb-filter' : '.ms-search').focus(); e.preventDefault(); }
	}, true);

	// Find servers: a Friends tab after Local Network, home screen on or off.
	// It's the game's own "friends" list (servers your Steam friends are on, no
	// names), switched to through the page's own SwitchType, like the other tabs.
	function friendsTab() {
		var lan = document.querySelector('a[ng-click="SwitchType( \'lan\' )"]');
		var li = lan && lan.parentNode;
		if (!li || !li.parentNode) return;
		var a = document.getElementById('meowy-friends-tab');
		if (!a) {
			var item = document.createElement('li');
			a = node('a', '', 'Friends');
			a.id = 'meowy-friends-tab';
			a.onclick = function () {
				var sc = window.Scope;
				if (!sc || typeof sc.SwitchType !== 'function') return;
				if (typeof sc.$apply === 'function') sc.$apply(function () { sc.SwitchType('friends'); });
				else sc.SwitchType('friends');
			};
			item.appendChild(a);
			li.parentNode.insertBefore(item, li.nextSibling);
		}
		var active = !!(window.RootScope && window.RootScope.ServerType === 'friends');
		if ((a.className === 'active') !== active) a.className = active ? 'active' : '';
	}
	window.MeowyFriendsTab = friendsTab;

	var MO = window.MutationObserver || window.WebKitMutationObserver;
	if (MO && document.body) new MO(function () { if (on) draw(); friendsTab(); }).observe(document.body, { childList: true, subtree: true, attributes: true, attributeFilter: ['class'] });
	friendsTab();
})();
