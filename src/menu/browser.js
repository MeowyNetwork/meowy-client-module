// Find servers, the pack's own (roadmap U6): everything Garry's Mod's server
// browser does, fed by the pack's faster Steam query. Gamemodes first, each with
// its players and servers, sorted and filtered by category; then a gamemode's
// servers in columns (name, map, players, ping, ranking), sorted by any of
// them, every row there to scroll to (only the rows on screen are drawn).
//
// It lives in the home screen (src/menu/shell.js builds it into its page and
// opens it); menu_page.lua sends the servers in batches through
// window.MeowyBrowse (reset, add, flush) and keeps the addresses: the page asks
// for a server by its number with meowy.post. Every server string is shown as
// text. The rules (no DOM) are window.MeowyBrowserLogic, for the tests.
//
// Plain ES5 and no flexbox: the 32-bit game's page is an old WebKit.
(function () {
	if (window.MeowyBrowser) return;

	var ROW_H = 26, BUFFER = 20, STALE = 120000, INTENT_MS = 400, RENDER_MS = 200;
	var CATS = ['rp', 'pvp', 'pve', 'other', 'none'];
	var CAT_NAMES = { rp: 'Roleplay', pvp: 'PvP', pve: 'PvE', other: 'Other', none: 'No category' };
	var ALL = '*';

	// --- The rules -----------------------------------------------------------------

	function str(v) { return typeof v === 'string' ? v : ''; }
	function int(v) { var n = Math.floor(Number(v)); return isFinite(n) ? n : 0; }

	// Garry's Mod's own ranking (control.Servers.js CalculateRank): lower is better.
	function rank(s) {
		var r = s.ping;
		if (s.players === 0) r += 75;
		if (s.pass || s.verC < 0) r += 300;
		if (s.anon) r += 1000;
		if (s.players >= 4) r -= 10;
		if (s.players >= 8) r -= 15;
		if (s.players >= 16) r -= 15;
		if (s.players >= 32) r -= 10;
		if (s.players >= 64) r -= 10;
		return r;
	}
	// Bars out of five, as Garry's Mod draws them.
	function rankBars(r) { return r < 50 ? 5 : r < 100 ? 4 : r < 200 ? 3 : r < 300 ? 2 : 1; }

	// 250107 -> "2025.01.07" (Garry's Mod's FormatVersion).
	function formatVersion(v) {
		if (!v) return 'unknown version';
		var y = Math.floor(v / 10000), m = Math.floor((v - y * 10000) / 100), d = v - y * 10000 - m * 100;
		var pad = function (n) { return (n < 10 ? '0' : '') + n; };
		return (y > 99 ? pad(y) : '20' + pad(y)) + '.' + pad(m) + '.' + pad(d);
	}
	// A server's sv_location as a flag name, or "".
	function flag(loc) {
		loc = str(loc).toLowerCase();
		if (!/^[a-z]+$/.test(loc)) return '';
		return loc === 'eu' ? 'europeanunion' : loc;
	}
	function datePart(n) { return (n < 10 ? '0' : '') + n; }

	// A row from menu_page.lua as a server. ours: this game's version number
	// (GMOD_VERSION_INT), haveMap(map): whether that map is installed.
	function server(r, ours, haveMap) {
		var s = {
			i: int(r.i), name: str(r.n), desc: str(r.d), map: str(r.m), address: str(r.a),
			players: Math.max(0, int(r.p)), max: Math.max(0, int(r.x)), bots: Math.max(0, int(r.b)),
			ping: Math.max(0, int(r.ms)), pass: r.pw === true, anon: r.an === true, fav: r.fv === true,
			cat: CATS.indexOf(r.c) >= 0 ? r.c : '', ws: /^\d{1,20}$/.test(str(r.ws)) && r.ws !== '0' ? r.ws : '',
			flag: flag(r.f), ver: Math.max(0, int(r.v)), lastplayed: Math.max(0, int(r.lp)) * 1000
		};
		s.gm = str(r.g) || s.desc || 'unknown';
		s.key = s.gm.toLowerCase();
		s.listen = s.desc.indexOf('[L] ') === 0;
		if (s.listen) s.desc = s.desc.substr(4);
		s.verC = (!s.ver || !ours) ? 0 : s.ver > ours ? 1 : s.ver === ours ? 0 : -1;
		s.hasmap = typeof haveMap === 'function' ? !!haveMap(s.map) : true;
		s.rank = rank(s);
		s.lname = s.name.toLowerCase();
		s.lmap = s.map.toLowerCase();
		if (s.lastplayed) {
			var t = new Date(s.lastplayed);
			s.lastText = datePart(t.getDate()) + '.' + datePart(t.getMonth() + 1) + '.' + t.getFullYear() + ' ' + datePart(t.getHours()) + ':' + datePart(t.getMinutes());
		}
		return s;
	}

	// The key with the highest count; a tie goes to the one seen first.
	function highest(counts, order) {
		var best = '', most = -1;
		for (var i = 0; i < order.length; i++) {
			if (counts[order[i]] > most) { most = counts[order[i]]; best = order[i]; }
		}
		return best;
	}

	// A gamemode as Garry's Mod's browser builds it from its servers: the title
	// most players see (the internal name only when nothing else), the most
	// common category (or "rp" from the title when none is given) and Workshop
	// ID, and the flags its servers have.
	function gamemode(key) {
		return { key: key, servers: [], count: 0, players: 0, sortPlayers: 0, title: key, titles: {}, titleOrder: [],
			cats: {}, catOrder: [], catSet: false, cat: '', wsids: {}, wsOrder: [], ws: '', flags: {}, flagOrder: [] };
	}
	function addServer(g, s) {
		g.servers.push(s);
		g.count++;
		g.players += s.players;
		if (!s.anon) g.sortPlayers += s.players;
		if (g.fixed) return;   // every gamemode: its own title, no category or Workshop item
		if (s.desc) {
			if (!(s.desc in g.titles)) { g.titles[s.desc] = 0; g.titleOrder.push(s.desc); }
			// Weighted by players (10 at most a server); the internal name itself only when there is nothing else.
			g.titles[s.desc] += (s.desc === s.gm ? 0 : 2 * Math.min(s.players, 10) + 1);
			g.title = highest(g.titles, g.titleOrder);
		}
		if (s.cat) {
			if (!(s.cat in g.cats)) { g.cats[s.cat] = 0; g.catOrder.push(s.cat); }
			g.cats[s.cat]++;
			g.cat = highest(g.cats, g.catOrder);
			g.catSet = true;
		}
		if (!g.catSet) g.cat = /roleplay/i.test(g.title) || / RP|RP |RP$/.test(g.title) ? 'rp' : '';
		if (s.ws) {
			if (!(s.ws in g.wsids)) { g.wsids[s.ws] = 0; g.wsOrder.push(s.ws); }
			g.wsids[s.ws]++;
			g.ws = highest(g.wsids, g.wsOrder);
		}
		if (s.flag && !g.flags[s.flag]) { g.flags[s.flag] = true; g.flagOrder.push(s.flag); }
	}

	// Which gamemodes show: the search (name or title), then the categories, each
	// "only" (any of these), "hide" or neither.
	function gamemodeShown(g, query, cats) {
		if (query) {
			var q = query.toLowerCase();
			if (g.key.indexOf(q) < 0 && g.title.toLowerCase().indexOf(q) < 0) return false;
		}
		var cat = g.cat || 'none', anyOnly = false;
		for (var c in cats) if (cats[c] === 'only') anyOnly = true;
		if (cats[cat] === 'hide') return false;
		if (anyOnly && cats[cat] !== 'only') return false;
		return true;
	}
	function sortGamemodes(list, by) {
		return list.sort(function (a, b) {
			if (by === 'servers' && a.count !== b.count) return b.count - a.count;
			if (by === 'name') {
				var an = a.title.toLowerCase(), bn = b.title.toLowerCase();
				if (an !== bn) return an < bn ? -1 : 1;
			}
			if (by === 'players' && a.sortPlayers !== b.sortPlayers) return b.sortPlayers - a.sortPlayers;
			return b.players - a.players || (a.key < b.key ? -1 : a.key > b.key ? 1 : 0);
		});
	}

	// Whether a server passes the filters: the search (name or map, any case, or
	// the address), players, full, password, ping, version and region flags
	// (each "prefer", "avoid" or neither: with any preferred, only those).
	function serverShown(s, f) {
		if (f.q) {
			var q = f.q.toLowerCase();
			if (s.lname.indexOf(q) < 0 && s.lmap.indexOf(q) < 0 && s.address.indexOf(f.q) < 0) return false;
		}
		if (f.hasPlayers && s.players < 1) return false;
		if (f.notFull && s.players >= s.max) return false;
		if (f.noPass && s.pass) return false;
		if (f.maxPing > 0 && s.ping > f.maxPing) return false;
		if (f.minPlayers > 0 && s.players < f.minPlayers) return false;
		if (f.maxPlayers > 0 && s.players > f.maxPlayers) return false;
		if (f.hideOutdated && s.verC < 0) return false;
		if (f.flags) {
			var mode = f.flags[s.flag], prefer = false;
			for (var k in f.flags) if (f.flags[k] === 'prefer') prefer = true;
			if (s.flag && mode === 'avoid') return false;
			if (prefer && mode !== 'prefer') return false;
		}
		return true;
	}
	// Sorted by a column ("rank", "name", "map", "players": most first, "ping"),
	// then ranking, ping and address; reverse turns it around.
	function sortServers(list, key, reverse) {
		var dir = reverse ? -1 : 1;
		return list.sort(function (a, b) {
			var c = 0;
			if (key === 'name') c = a.lname < b.lname ? -1 : a.lname > b.lname ? 1 : 0;
			else if (key === 'map') c = a.lmap < b.lmap ? -1 : a.lmap > b.lmap ? 1 : 0;
			else if (key === 'players') c = b.players - a.players;
			else if (key === 'ping') c = a.ping - b.ping;
			if (!c) c = a.rank - b.rank || a.ping - b.ping || (a.address < b.address ? -1 : a.address > b.address ? 1 : 0);
			return c * dir;
		});
	}
	// Clicking a mode: neither -> first -> second -> neither (categories: only,
	// hide; flags: prefer, avoid).
	function cycle(current, first, second) { return current === first ? second : current === second ? undefined : first; }

	window.MeowyBrowserLogic = {
		rank: rank, rankBars: rankBars, formatVersion: formatVersion, flag: flag, server: server,
		gamemode: gamemode, addServer: addServer, gamemodeShown: gamemodeShown, sortGamemodes: sortGamemodes,
		serverShown: serverShown, sortServers: sortServers, cycle: cycle, CATS: CATS
	};

	// --- The page --------------------------------------------------------------------

	var STYLE =
		'#meowy-browser { position: absolute; left: 0; top: 0; right: 0; bottom: 0; font-size: 14px; }'
		+ '#meowy-browser .mb-side { position: absolute; left: 0; top: 0; bottom: 0; width: 210px; overflow-y: auto; overflow-x: hidden; }'
		+ '#meowy-browser .mb-main { position: absolute; left: 226px; top: 0; right: 0; bottom: 0; }'
		+ '#meowy-browser .mb-title { font-size: 26px; font-weight: bold; letter-spacing: -0.5px; }'
		+ '#meowy-browser .mb-small { font-size: 13px; opacity: 0.75; margin: 2px 0; }'
		+ '#meowy-browser .mb-label { font-size: 12px; font-weight: bold; letter-spacing: 1px; text-transform: uppercase; opacity: 0.6; margin: 16px 0 4px 0; }'
		+ '#meowy-browser .mb-btn { display: inline-block; font-size: 14px; padding: 4px 11px; margin: 2px 4px 2px 0; border-radius: 12px; background: rgba(255,255,255,0.12); cursor: pointer; white-space: nowrap; }'
		+ '#meowy-browser .mb-btn:hover { background: rgba(255,255,255,0.24); }'
		+ '#meowy-browser .mb-btn.on { background: rgba(255,255,140,0.38); }'
		+ '#meowy-browser .mb-btn.off { background: rgba(255,90,90,0.3); text-decoration: line-through; }'
		+ '#meowy-browser .mb-link { display: block; font-size: 15px; font-weight: bold; padding: 3px 0; cursor: pointer; }'
		+ '#meowy-browser .mb-link:hover { color: #ffa; }'
		+ '#meowy-browser .mb-input { width: 100%; box-sizing: border-box; font-size: 15px; padding: 6px 9px; border-radius: 5px; border: 1px solid rgba(255,255,255,0.25); background: rgba(0,0,0,0.45); color: #fff; outline: none; }'
		+ '#meowy-browser .mb-head1 { height: 40px; line-height: 40px; white-space: nowrap; overflow: hidden; }'
		+ '#meowy-browser .mb-head1 .mb-small { display: inline; margin-left: 10px; }'
		+ '#meowy-browser .mb-grid { position: absolute; left: 0; right: 0; top: 46px; bottom: 0; overflow-y: auto; }'
		+ '#meowy-browser .mb-gm { display: inline-block; vertical-align: top; position: relative; width: 290px; height: 54px; margin: 0 8px 8px 0; padding: 7px 10px 0 44px; box-sizing: border-box; border-radius: 6px; background: rgba(0,0,0,0.45); cursor: pointer; overflow: hidden; }'
		+ '#meowy-browser .mb-gm:hover, #meowy-browser .mb-gm.sel { background: rgba(255,255,255,0.2); }'
		+ '#meowy-browser .mb-gm.noplayers { opacity: 0.55; }'
		+ '#meowy-browser .mb-gm img { position: absolute; left: 10px; top: 15px; width: 24px; height: 24px; }'
		+ '#meowy-browser .mb-gmname { font-size: 16px; font-weight: bold; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; padding-right: 4px; }'
		+ '#meowy-browser .mb-gmstats { font-size: 13px; opacity: 0.8; white-space: nowrap; }'
		+ '#meowy-browser .mb-tag { display: inline-block; font-size: 10px; font-weight: bold; text-transform: uppercase; padding: 1px 5px; margin-left: 6px; border-radius: 3px; background: rgba(255,255,255,0.2); vertical-align: 2px; }'
		+ '#meowy-browser .mb-tag.future { background: rgba(110,190,255,0.4); }'
		+ '#meowy-browser .mb-tag.old { background: rgba(255,110,110,0.4); }'
		+ '#meowy-browser .mb-install { position: absolute; right: 8px; top: 8px; font-size: 12px; padding: 2px 8px; border-radius: 10px; background: rgba(120,200,120,0.45); }'
		+ '#meowy-browser .mb-install:hover { background: rgba(120,220,120,0.7); }'
		+ '#meowy-browser .mb-head1 .mb-btn { vertical-align: 4px; margin-left: 10px; }'
		+ '#meowy-browser .mb-tools { position: absolute; left: 0; right: 0; top: 44px; height: 34px; }'
		+ '#meowy-browser .mb-tools .mb-input { width: 340px; }'
		+ '#meowy-browser .mb-table { position: absolute; left: 0; right: 0; top: 84px; bottom: 0; background: rgba(0,0,0,0.45); border-radius: 6px; overflow: hidden; }'
		+ '#meowy-browser .mb-thead { position: relative; height: 28px; line-height: 28px; font-size: 13px; font-weight: bold; border-bottom: 1px solid rgba(255,255,255,0.15); }'
		+ '#meowy-browser .mb-thead span { cursor: pointer; opacity: 0.8; }'
		+ '#meowy-browser .mb-thead span:hover { opacity: 1; color: #ffa; }'
		+ '#meowy-browser .mb-tbody { position: absolute; left: 0; right: 0; top: 29px; bottom: 0; overflow-y: auto; overflow-x: hidden; }'
		+ '#meowy-browser .mb-srow { position: relative; height: ' + ROW_H + 'px; line-height: ' + ROW_H + 'px; white-space: nowrap; overflow: hidden; cursor: pointer; }'
		+ '#meowy-browser .mb-srow.odd { background: rgba(255,255,255,0.04); }'
		+ '#meowy-browser .mb-srow:hover { background: rgba(255,255,255,0.13); }'
		+ '#meowy-browser .mb-srow.sel { background: rgba(255,255,140,0.28); }'
		+ '#meowy-browser .mb-srow.empty { opacity: 0.6; }'
		+ '#meowy-browser .mb-c-name { position: absolute; left: 8px; right: 410px; overflow: hidden; text-overflow: ellipsis; }'
		+ '#meowy-browser .mb-c-map { position: absolute; right: 230px; width: 170px; overflow: hidden; text-overflow: ellipsis; }'
		+ '#meowy-browser .mb-srow.nomap .mb-c-map { color: #ff9a9a; }'
		+ '#meowy-browser .mb-c-ply { position: absolute; right: 140px; width: 80px; text-align: right; }'
		+ '#meowy-browser .mb-c-ping { position: absolute; right: 74px; width: 56px; text-align: right; }'
		+ '#meowy-browser .mb-c-rank { position: absolute; right: 10px; width: 52px; }'
		+ '#meowy-browser .mb-bar { display: inline-block; height: 8px; border-radius: 2px; background: #9be27b; vertical-align: 1px; }'
		+ '#meowy-browser .mb-fav { display: inline-block; width: 18px; color: #ffd24d; opacity: 0.35; }'
		+ '#meowy-browser .mb-fav:hover, #meowy-browser .mb-fav.on { opacity: 1; }'
		+ '#meowy-browser .mb-flag { width: 16px; height: 11px; margin-right: 5px; vertical-align: -1px; }'
		+ '#meowy-browser .mb-lock { width: 12px; height: 12px; margin-right: 5px; vertical-align: -1px; }'
		+ '#meowy-browser .mb-last { float: right; font-size: 12px; opacity: 0.55; margin-left: 10px; }'
		+ '#meowy-browser .mb-empty { padding: 20px; font-size: 15px; opacity: 0.75; }';

	var st = {
		open: false, built: false, gen: 0, ok: true, done: false, stopped: false, stage: 1, secs: 0, at: 0,
		servers: [], byI: {}, gms: {}, gmOrder: [], all: null,
		gm: null, gmSort: 'players', gmQuery: '', cats: {},
		q: '', sortKey: 'rank', reverse: false, sel: null, view: [], viewFor: null, timer: null, first: -1, last: -1
	};
	function everyGamemode() { var g = gamemode(ALL); g.title = 'Every gamemode'; g.fixed = true; return g; }
	st.all = everyGamemode();
	var ui = {};

	function post(kind, value) {
		if (window.meowy && typeof window.meowy.post === 'function') window.meowy.post(kind, value === undefined ? '' : value);
	}
	function node(tag, cls, text) {
		var e = document.createElement(tag);
		if (cls) e.className = cls;
		if (text) e.textContent = text;
		return e;
	}
	function clear(e) { while (e.firstChild) e.removeChild(e.firstChild); }
	function button(label, cls, onClick) {
		var b = node('span', 'mb-btn' + (cls ? ' ' + cls : ''), label);
		b.onclick = onClick;
		return b;
	}
	function plural(n, one, many) { return n + ' ' + (n === 1 ? one : many); }
	function ours() { var v = window.GMOD_VERSION_INT; return typeof v === 'number' && v > 0 ? v : 0; }
	function haveMap(map) { return typeof window.DoWeHaveMap === 'function' ? window.DoWeHaveMap(map) : true; }
	function subscribed(ws) {
		var s = window.subscriptions;
		return !!(s && typeof s.Contains === 'function' && s.Contains(String(ws)));
	}
	function current() { return st.gm === ALL ? st.all : st.gm ? st.gms[st.gm] : null; }

	function gmIcon(g) {
		var img = node('img');
		img.onerror = function () { img.onerror = null; img.src = '../gamemodes/base/icon24.png'; };
		img.src = /^[a-z0-9_]+$/.test(g.key) ? '../gamemodes/' + g.key + '/icon24.png' : '../gamemodes/base/icon24.png';
		return img;
	}
	function installButton(g, cls) {
		if (!g.ws || subscribed(g.ws)) return null;
		var b = node('span', cls, 'Install');
		b.title = 'Subscribe to this gamemode on the Workshop';
		b.onclick = function (e) {
			post('browse_install', g.ws);
			b.textContent = 'Installing';
			b.onclick = null;
			if (e && e.stopPropagation) e.stopPropagation();
		};
		return b;
	}

	// The left column: counts, the list's state, Refresh or Stop, the way back,
	// and on the gamemodes page their sort, categories and search.
	function sideState() {
		ui.count.textContent = plural(st.servers.length, 'server', 'servers') + ' \u00b7 ' + plural(st.gmOrder.length, 'gamemode', 'gamemodes');
		var status;
		if (!st.ok) status = "Steam's server list isn't available here: use Garry's Mod's list.";
		else if (st.stopped) status = 'Stopped after ' + st.secs + ' s';
		else if (st.done) status = 'Complete in ' + st.secs + ' s';
		else status = (st.stage === 2 ? 'Listing empty servers\u2026 ' : 'Listing servers with players\u2026 ') + st.secs + ' s';
		ui.status.textContent = status;
		var listing = !st.done && st.ok;
		ui.action.textContent = listing ? 'Stop' : 'Refresh';
		ui.action.onclick = listing
			? function () { st.stopped = st.done = true; post('browse_stop'); sideState(); }
			: function () { post('browse'); };
	}
	function drawSide() {
		var side = ui.side;
		clear(side);
		ui.sideFor = st.gm || '';
		side.appendChild(node('div', 'mb-title', 'Find servers'));
		ui.count = side.appendChild(node('div', 'mb-small'));
		ui.status = side.appendChild(node('div', 'mb-small mb-status'));
		var row = node('div');
		ui.action = row.appendChild(button('Refresh', '', null));
		side.appendChild(row);
		sideState();
		var back = node('div');
		back.style.marginTop = '10px';
		if (st.gm) {
			var toGms = node('div', 'mb-link', '\u2039 All gamemodes');
			toGms.onclick = function () { pickGamemode(null); };
			back.appendChild(toGms);
		}
		var stock = node('div', 'mb-link', "Garry's Mod's list");
		stock.onclick = function () { window.MeowyBrowser.open(false); location.hash = '#/servers/'; };
		back.appendChild(stock);
		var home = node('div', 'mb-link', 'Back');
		home.onclick = function () { window.MeowyBrowser.open(false); };
		back.appendChild(home);
		side.appendChild(back);
		if (st.gm) return;
		side.appendChild(node('div', 'mb-label', 'Sort gamemodes by'));
		var sorts = [['players', 'Players'], ['servers', 'Servers'], ['name', 'Name']], box = node('div');
		for (var i = 0; i < sorts.length; i++) {
			box.appendChild(button(sorts[i][1], st.gmSort === sorts[i][0] ? 'on' : '', (function (by) {
				return function () { st.gmSort = by; drawSide(); drawGrid(); };
			})(sorts[i][0])));
		}
		side.appendChild(box);
		side.appendChild(node('div', 'mb-label', 'Categories'));
		var cats = node('div');
		for (var c = 0; c < CATS.length; c++) {
			var mode = st.cats[CATS[c]];
			var b = button(CAT_NAMES[CATS[c]], mode === 'only' ? 'on' : mode === 'hide' ? 'off' : '', (function (cat) {
				return function () { st.cats[cat] = cycle(st.cats[cat], 'only', 'hide'); drawSide(); drawGrid(); };
			})(CATS[c]));
			b.title = 'Click: only this, again: hide it, again: either';
			cats.appendChild(b);
		}
		side.appendChild(cats);
		side.appendChild(node('div', 'mb-label', 'Find a gamemode'));
		side.appendChild(ui.gmSearch);
	}

	function drawGamemodes() {
		var main = ui.main;
		clear(main);
		var head = node('div', 'mb-head1');
		head.appendChild(node('span', 'mb-title', 'Gamemodes'));
		head.appendChild(node('span', 'mb-small', 'pick one to see its servers'));
		main.appendChild(head);
		ui.grid = main.appendChild(node('div', 'mb-grid'));
		drawGrid();
	}
	function drawGrid() {
		var grid = ui.grid;
		if (!grid) return;
		var top = grid.scrollTop || 0, list = [], anyCat = false;
		for (var c in st.cats) if (st.cats[c]) anyCat = true;
		clear(grid);
		for (var i = 0; i < st.gmOrder.length; i++) {
			var g = st.gms[st.gmOrder[i]];
			if (gamemodeShown(g, st.gmQuery, st.cats)) list.push(g);
		}
		sortGamemodes(list, st.gmSort);
		if (list.length > 1 && !st.gmQuery && !anyCat) list.unshift(st.all);
		if (!list.length) grid.appendChild(node('div', 'mb-empty', st.servers.length ? 'No gamemode matches.' : (st.ok ? 'Asking Steam for servers\u2026' : '')));
		// Two gamemodes by the same title (DarkRP and a fork of it, say): each says its folder name.
		var titles = {};
		for (var t = 0; t < list.length; t++) titles[list[t].title] = (titles[list[t].title] || 0) + 1;
		for (var k = 0; k < list.length; k++) grid.appendChild(gamemodeTile(list[k], titles[list[k].title] > 1));
		grid.scrollTop = top;
	}
	function gamemodeTile(g, sameTitle) {
		var el = node('div', 'mb-gm' + (g.players === 0 ? ' noplayers' : ''));
		el.appendChild(gmIcon(g));
		var name = node('div', 'mb-gmname', g.title);
		if (g.cat) name.appendChild(node('span', 'mb-tag', CAT_NAMES[g.cat] || g.cat));
		el.appendChild(name);
		el.appendChild(node('div', 'mb-gmstats', plural(g.players, 'player', 'players') + ' on ' + plural(g.count, 'server', 'servers') + (sameTitle ? ' \u00b7 ' + g.key : '')));
		var install = installButton(g, 'mb-install');
		if (install) el.appendChild(install);
		el.title = g.key === ALL ? 'Every server, of every gamemode' : g.title + ' (' + g.key + ')';
		el.onclick = function () { pickGamemode(g.key); };
		return el;
	}

	// A gamemode's servers: the title (and Install), the search, the columns.
	function drawServerPage() {
		var g = current(), main = ui.main;
		clear(main);
		var head = node('div', 'mb-head1');
		head.appendChild(node('span', 'mb-title', g.title));
		head.appendChild(node('span', 'mb-small', plural(g.players, 'player', 'players') + ' on ' + plural(g.count, 'server', 'servers')));
		var install = installButton(g, 'mb-btn');
		if (install) head.appendChild(install);
		main.appendChild(head);
		var tools = node('div', 'mb-tools');
		tools.appendChild(ui.search);
		main.appendChild(tools);
		var table = node('div', 'mb-table');
		var thead = node('div', 'mb-thead');
		var cols = [['mb-c-name', 'name', 'Name'], ['mb-c-map', 'map', 'Map'], ['mb-c-ply', 'players', 'Players'],
			['mb-c-ping', 'ping', 'Ping'], ['mb-c-rank', 'rank', 'Ranking']];
		for (var i = 0; i < cols.length; i++) {
			var label = cols[i][2] + (st.sortKey === cols[i][1] ? (st.reverse ? ' \u25b4' : ' \u25be') : '');
			var h = node('span', cols[i][0], label);
			h.onclick = (function (key) {
				return function () {
					if (st.sortKey === key) st.reverse = !st.reverse; else { st.sortKey = key; st.reverse = false; }
					st.viewFor = null;
					drawServerPage();
				};
			})(cols[i][1]);
			thead.appendChild(h);
		}
		table.appendChild(thead);
		var body = node('div', 'mb-tbody');
		body.onscroll = function () { drawRows(false); };
		ui.body = body;
		ui.pre = node('div');
		ui.rows = node('div');
		ui.post = node('div');
		body.appendChild(ui.pre);
		body.appendChild(ui.rows);
		body.appendChild(ui.post);
		table.appendChild(body);
		main.appendChild(table);
		st.first = st.last = -1;
		drawRows(true);
	}

	// The servers that pass, sorted; worked out again only when something changed.
	function filters() { return { q: st.q }; }
	function updateView() {
		var g = current(), key = g.key + '|' + g.count + '|' + st.q + '|' + st.sortKey + '|' + st.reverse;
		if (st.viewFor === key) return;
		var f = filters(), out = [];
		for (var i = 0; i < g.servers.length; i++) if (serverShown(g.servers[i], f)) out.push(g.servers[i]);
		st.view = sortServers(out, st.sortKey, st.reverse);
		st.viewFor = key;
	}
	// Only the rows on screen (and BUFFER either side) are in the page; the rest
	// is two spacers, so the list scrolls as if they all were. Nothing scrolls by
	// itself: new servers land in their places.
	function drawRows(force) {
		if (!ui.body) return;
		updateView();
		var top = ui.body.scrollTop || 0, h = ui.body.clientHeight || 700;
		var first = Math.max(0, Math.floor(top / ROW_H) - BUFFER);
		var last = Math.min(st.view.length, Math.ceil((top + h) / ROW_H) + BUFFER);
		if (!force && first === st.first && last === st.last) return;
		st.first = first;
		st.last = last;
		ui.pre.style.height = (first * ROW_H) + 'px';
		ui.post.style.height = ((st.view.length - last) * ROW_H) + 'px';
		clear(ui.rows);
		if (!st.view.length) {
			ui.rows.appendChild(node('div', 'mb-empty', st.q ? 'No server matches.' : 'No servers yet.'));
			return;
		}
		for (var i = first; i < last; i++) ui.rows.appendChild(serverRow(st.view[i], i));
	}
	function serverRow(s, index) {
		var cls = 'mb-srow' + (index % 2 ? ' odd' : '') + (s.players === 0 ? ' empty' : '') + (s.hasmap ? '' : ' nomap') + (st.sel === s.i ? ' sel' : '');
		var el = node('div', cls);
		var name = node('span', 'mb-c-name');
		var fav = node('span', 'mb-fav' + (s.fav ? ' on' : ''), s.fav ? '\u2605' : '\u2606');
		fav.title = s.fav ? 'Remove from favourites' : 'Add to favourites';
		fav.onclick = function (e) {
			s.fav = !s.fav;
			post('browse_fav', s.i);
			drawRows(true);
			if (e && e.stopPropagation) e.stopPropagation();
		};
		name.appendChild(fav);
		if (s.lastText) name.appendChild(node('span', 'mb-last', 'last visit ' + s.lastText));
		if (s.flag) {
			var f = node('img', 'mb-flag');
			f.onerror = function () { f.onerror = null; f.src = 'img/unk_flag.png'; };
			f.src = 'asset://garrysmod/materials/flags16/' + s.flag + '.png';
			name.appendChild(f);
		}
		if (s.pass) {
			var lock = node('img', 'mb-lock');
			lock.src = 'img/server-passworded.png';
			lock.title = 'Needs a password';
			name.appendChild(lock);
		}
		name.appendChild(document.createTextNode(s.name));
		if (s.verC) name.appendChild(node('span', 'mb-tag ' + (s.verC > 0 ? 'future' : 'old'), (s.verC > 0 ? 'newer: ' : 'outdated: ') + formatVersion(s.ver)));
		el.appendChild(name);
		var map = node('span', 'mb-c-map', s.map);
		if (!s.hasmap) map.title = "You don't have this map: it downloads when you join";
		el.appendChild(map);
		el.appendChild(node('span', 'mb-c-ply', s.players + ' / ' + s.max));
		el.appendChild(node('span', 'mb-c-ping', String(s.ping)));
		var rk = node('span', 'mb-c-rank');
		var bar = node('span', 'mb-bar');
		bar.style.width = (rankBars(s.rank) * 10) + 'px';
		rk.appendChild(bar);
		el.appendChild(rk);
		el.title = s.name + '\n' + s.address + (s.desc ? '\n' + s.desc : '') + (s.bots ? '\n' + plural(s.bots, 'bot', 'bots') : '');
		el.onclick = function () { select(s.i); };
		el.ondblclick = function () { join(s); };
		el.oncontextmenu = function (e) {
			post('browse_copy', s.i);
			if (e && e.preventDefault) e.preventDefault();
			return false;
		};
		var timer = null;
		el.onmouseenter = function () { timer = setTimeout(function () { post('browse_intent', s.i); }, INTENT_MS); };
		el.onmouseleave = function () { if (timer) clearTimeout(timer); timer = null; };
		return el;
	}

	function select(i) {
		st.sel = i;
		drawRows(true);
	}
	function join(s) {
		if (!s) return;
		post('browse_join', s.i);
	}
	// Up and Down go through the list (kept in view), Enter joins.
	function moveSelection(dir) {
		updateView();
		if (!st.view.length) return;
		var at = -1;
		for (var i = 0; i < st.view.length; i++) if (st.view[i].i === st.sel) { at = i; break; }
		at = at < 0 ? (dir > 0 ? 0 : st.view.length - 1) : Math.max(0, Math.min(st.view.length - 1, at + dir));
		st.sel = st.view[at].i;
		var body = ui.body;
		if (!body) return;
		var top = at * ROW_H, h = body.clientHeight || 700;
		if (top < body.scrollTop) body.scrollTop = top;
		else if (top + ROW_H > body.scrollTop + h) body.scrollTop = top + ROW_H - h;
		drawRows(true);
	}
	function selected() {
		for (var i = 0; i < st.view.length; i++) if (st.view[i].i === st.sel) return st.view[i];
		return null;
	}

	function pickGamemode(key) {
		st.gm = key;
		st.q = '';
		ui.search.value = '';
		st.sel = null;
		st.viewFor = null;
		render();
		var f = key ? ui.search : ui.gmSearch;
		if (f.focus) f.focus();
	}

	// The page is drawn when it changes (the gamemodes, or a gamemode's servers);
	// otherwise only the counts, the tiles or the rows, so typing and scrolling
	// aren't disturbed while servers arrive.
	function render() {
		if (!st.built || !st.open) return;
		if (st.gm && !current()) st.gm = null;
		var page = st.gm || '';
		if (ui.sideFor !== page) drawSide(); else sideState();
		if (ui.mainFor === page) {
			if (st.gm) { drawServerPageHead(); drawRows(true); } else drawGrid();
			return;
		}
		ui.mainFor = page;
		if (st.gm) { ui.grid = null; drawServerPage(); } else { ui.body = null; drawGamemodes(); }
	}
	function drawServerPageHead() {
		var g = current(), head = ui.main.firstChild;
		if (!head || !head.children) return;
		var small = head.children[1];
		if (small) small.textContent = plural(g.players, 'player', 'players') + ' on ' + plural(g.count, 'server', 'servers');
	}

	function build() {
		if (!document.getElementById('meowy-browser-style')) {
			var css = document.createElement('style');
			css.id = 'meowy-browser-style';
			css.textContent = STYLE;
			(document.head || document.body).appendChild(css);
		}
		var root = node('div');
		root.id = 'meowy-browser';
		ui.side = node('div', 'mb-side');
		ui.main = node('div', 'mb-main');
		root.appendChild(ui.side);
		root.appendChild(ui.main);
		ui.root = root;

		ui.gmSearch = node('input', 'mb-input mb-gmsearch');
		ui.gmSearch.type = 'text';
		ui.gmSearch.maxLength = 64;
		ui.gmSearch.placeholder = 'Name of a gamemode';
		ui.gmSearch.value = st.gmQuery;
		ui.gmSearch.oninput = function () { st.gmQuery = ui.gmSearch.value; drawGrid(); };
		ui.gmSearch.onkeydown = function (e) {
			if (e.keyCode === 13) {
				var tile = ui.main.querySelector('.mb-gm');
				if (tile && tile.onclick) tile.onclick();
				e.preventDefault();
			} else if (e.keyCode === 27) {
				if (ui.gmSearch.value) { ui.gmSearch.value = ''; st.gmQuery = ''; drawGrid(); } else window.MeowyBrowser.open(false);
				e.preventDefault();
			}
		};

		ui.search = node('input', 'mb-input mb-search');
		ui.search.type = 'text';
		ui.search.maxLength = 64;
		ui.search.placeholder = 'Search names, maps or an address';
		ui.search.value = st.q;
		ui.search.oninput = function () { st.q = ui.search.value; st.viewFor = null; drawRows(true); };
		ui.search.onkeydown = function (e) {
			var k = e.keyCode;
			if (k === 40 || k === 38) { moveSelection(k === 40 ? 1 : -1); e.preventDefault(); }
			else if (k === 13) { join(selected() || st.view[0]); e.preventDefault(); }
			else if (k === 27) {
				if (ui.search.value) { ui.search.value = ''; st.q = ''; st.viewFor = null; drawRows(true); } else pickGamemode(null);
				e.preventDefault();
			}
		};
		st.built = true;
		return root;
	}

	// Data from menu_page.lua. A list is a generation: rows of an older one are
	// dropped. stage 1: servers with players, 2: the empty ones after them.
	function scheduleRender() {
		if (st.timer) return;
		st.timer = setTimeout(function () { st.timer = null; render(); }, RENDER_MS);
	}
	window.MeowyBrowse = {
		reset: function (gen, ok) {
			st.gen = gen; st.ok = !!ok; st.done = !ok; st.complete = false; st.stopped = false; st.stage = 1; st.secs = 0; st.at = Date.now();
			st.servers = []; st.byI = {}; st.gms = {}; st.gmOrder = []; st.all = everyGamemode();
			st.sel = null; st.viewFor = null;
			render();
		},
		add: function (gen, rows, finished, secs, stage) {
			if (gen !== st.gen || !rows) return;
			var v = ours();
			for (var i = 0; i < rows.length; i++) {
				var r = rows[i];
				if (!r || typeof r.i !== 'number' || typeof r.n !== 'string' || st.byI[r.i]) continue;
				var s = server(r, v, haveMap);
				if (s.max <= 1) continue;   // as Garry's Mod's list: not a server to join
				st.byI[s.i] = s;
				st.servers.push(s);
				if (!st.gms[s.key]) { st.gms[s.key] = gamemode(s.key); st.gmOrder.push(s.key); }
				addServer(st.gms[s.key], s);
				addServer(st.all, s);
			}
			if (finished) { st.done = st.complete = true; st.stopped = false; }
			if (stage) st.stage = stage;
			st.secs = secs;
			st.viewFor = null;
			scheduleRender();
		},
		flush: function () { if (st.timer) { clearTimeout(st.timer); st.timer = null; } render(); },
		// The tests' and shell's way in: open(false) closes.
		open: function (on) { window.MeowyBrowser.open(on !== false); }
	};

	window.MeowyBrowser = {
		// The browser's element, built once (the home screen puts it in its page).
		element: function () { return st.built ? ui.root : build(); },
		isOpen: function () { return st.open; },
		// Opens it (asking for a list if there's none or it's old) or closes it
		// (stopping the list). onToggle(open) lets the home screen show or hide it.
		open: function (on) {
			on = on !== false;
			if (on === st.open) { if (on) render(); return; }
			st.open = on;
			if (typeof window.MeowyBrowser.onToggle === 'function') window.MeowyBrowser.onToggle(on);
			if (on) {
				if (!st.built) build();
				if (!st.servers.length || !st.complete || Date.now() - st.at > STALE) post('browse');
				render();
				var f = st.gm ? ui.search : ui.gmSearch;
				if (f && f.focus) f.focus();
			} else {
				if (!st.done) post('browse_stop');
			}
		},
		focusSearch: function () { var f = st.gm ? ui.search : ui.gmSearch; if (f && f.focus) f.focus(); },
		onToggle: null
	};
})();
