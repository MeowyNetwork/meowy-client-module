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
		// A ping Steam hasn't measured yet (a row from Meowy's index): 1 this PC's
		// last ping to that network, 2 estimated from the distance, 3 unknown.
		s.hint = r.ph === 1 || r.ph === 2 || r.ph === 3 ? r.ph : 0;
		s.gm = str(r.g) || s.desc || 'unknown';
		s.key = s.gm.toLowerCase();
		s.listen = s.desc.indexOf('[L] ') === 0;
		if (s.listen) s.desc = s.desc.substr(4);
		s.verC = (!s.ver || !ours) ? 0 : s.ver > ours ? 1 : s.ver === ours ? 0 : -1;
		s.hasmap = typeof haveMap === 'function' ? !!haveMap(s.map) : true;
		// The honest server list's tag (VERIFIED, FAKE - 12 real, ...); unverified ranks a little lower.
		s.honest = str(r.h);
		s.penalty = Math.max(0, int(r.hp));
		s.rank = rank(s) + s.penalty;
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
		if (s.flag && !g.flags[s.flag]) { g.flags[s.flag] = true; g.flagOrder.push(s.flag); }
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
	// Sorted by a column ("rank", "name", "map", "players": most first, "ping",
	// "last": last visit, newest first, "gm": gamemode),
	// then ranking, ping and address; reverse turns it around.
	function sortServers(list, key, reverse) {
		var dir = reverse ? -1 : 1;
		return list.sort(function (a, b) {
			var c = 0;
			if (key === 'name') c = a.lname < b.lname ? -1 : a.lname > b.lname ? 1 : 0;
			else if (key === 'map') c = a.lmap < b.lmap ? -1 : a.lmap > b.lmap ? 1 : 0;
			else if (key === 'players') c = b.players - a.players;
			else if (key === 'ping') c = a.ping - b.ping;
			else if (key === 'last') c = b.lastplayed - a.lastplayed;
			else if (key === 'gm') c = a.gmTitle < b.gmTitle ? -1 : a.gmTitle > b.gmTitle ? 1 : 0;
			if (!c) c = a.rank - b.rank || a.ping - b.ping || (a.address < b.address ? -1 : a.address > b.address ? 1 : 0);
			return c * dir;
		});
	}
	// Clicking a mode: neither -> first -> second -> neither (categories: only,
	// hide; flags: prefer, avoid).
	function cycle(current, first, second) { return current === first ? second : current === second ? undefined : first; }

	// The ping column: Steam's measurement, "~45" for a hint, "..." for none.
	function pingText(s) { return s.hint === 3 ? '…' : (s.hint ? '~' : '') + s.ping; }
	var HINT_TITLES = { 1: 'Your ping there last time: Steam measures it again in a moment',
		2: 'Estimated from the distance (IP Geolocation by DB-IP): Steam measures it in a moment',
		3: 'Not measured yet: Steam measures it in a moment' };

	// A server sent again (same number) with newer numbers: copied onto the one
	// the list has, so its place in gamemodes, the selection and the star stay.
	function updateServer(s, n) {
		var keys = ['name', 'desc', 'map', 'players', 'max', 'bots', 'ping', 'hint', 'pass', 'anon', 'ver', 'verC', 'hasmap',
			'honest', 'penalty', 'rank', 'lname', 'lmap', 'listen', 'flag', 'cat', 'ws'];
		for (var k = 0; k < keys.length; k++) s[keys[k]] = n[keys[k]];
		if (n.desc && !s.desc) s.desc = n.desc;
	}

	window.MeowyBrowserLogic = {
		rank: rank, rankBars: rankBars, formatVersion: formatVersion, flag: flag, server: server,
		gamemode: gamemode, addServer: addServer, gamemodeShown: gamemodeShown, sortGamemodes: sortGamemodes,
		serverShown: serverShown, sortServers: sortServers, cycle: cycle, pingText: pingText, updateServer: updateServer, CATS: CATS
	};

	// --- The page --------------------------------------------------------------------

	var TABS = [['internet', 'Internet'], ['favorite', 'Favourites'], ['history', 'History'], ['lan', 'Local network'], ['friends', 'Friends']];
	var TAB_NAMES = {};
	for (var tn = 0; tn < TABS.length; tn++) TAB_NAMES[TABS[tn][0]] = TABS[tn][1];
	var DETAIL_W = 340, REFRESH_MS = 10000, WAIT_MS = 5000, SELECT_MS = 250, PREFS_MS = 800, PREFS_MAX = 600, PLAYERS_MAX = 256;
	var INDEX_REFRESH_MS = 60000;   // a newer index while the Internet list from it stays open
	var TOGGLES = [['notFull', 'Not full'], ['hasPlayers', 'Has players'], ['noPass', 'No password'], ['hideOutdated', 'Not outdated']];

	var STYLE =
		'#meowy-browser { position: absolute; left: 0; top: 0; right: 0; bottom: 0; font-size: 14px; }'
		+ '#meowy-browser .mb-side { position: absolute; left: 0; top: 0; bottom: 0; width: 220px; overflow-y: auto; overflow-x: hidden; padding-right: 4px; box-sizing: border-box; }'
		+ '#meowy-browser .mb-main { position: absolute; left: 236px; top: 0; right: 0; bottom: 0; }'
		+ '#meowy-browser .mb-title { font-size: 26px; font-weight: bold; letter-spacing: -0.5px; }'
		+ '#meowy-browser .mb-small { font-size: 13px; opacity: 0.75; margin: 2px 0; }'
		+ '#meowy-browser .mb-label { font-size: 12px; font-weight: bold; letter-spacing: 1px; text-transform: uppercase; opacity: 0.6; margin: 14px 0 4px 0; }'
		+ '#meowy-browser .mb-btn { display: inline-block; font-size: 14px; padding: 4px 11px; margin: 2px 4px 2px 0; border-radius: 12px; background: rgba(255,255,255,0.12); cursor: pointer; white-space: nowrap; }'
		+ '#meowy-browser .mb-btn:hover { background: rgba(255,255,255,0.24); }'
		+ '#meowy-browser .mb-btn.on { background: rgba(255,255,140,0.38); }'
		+ '#meowy-browser .mb-btn.off { background: rgba(255,90,90,0.3); text-decoration: line-through; }'
		+ '#meowy-browser .mb-link { display: block; font-size: 15px; font-weight: bold; padding: 3px 0; cursor: pointer; }'
		+ '#meowy-browser .mb-link:hover, #meowy-browser .mb-link.on { color: #ffa; }'
		+ '#meowy-browser .mb-tabs { margin: 10px 0 8px 0; }'
		+ '#meowy-browser .mb-tabs .mb-link { font-size: 17px; padding: 2px 0; }'
		+ '#meowy-browser .mb-input { width: 100%; box-sizing: border-box; font-size: 15px; padding: 6px 9px; border-radius: 5px; border: 1px solid rgba(255,255,255,0.25); background: rgba(0,0,0,0.45); color: #fff; outline: none; }'
		+ '#meowy-browser .mb-num { width: 70px; display: inline-block; padding: 4px 6px; font-size: 14px; }'
		+ '#meowy-browser .mb-head1 { height: 40px; line-height: 40px; white-space: nowrap; overflow: hidden; }'
		+ '#meowy-browser .mb-head1 .mb-small { display: inline; margin-left: 10px; }'
		+ '#meowy-browser .mb-head1 .mb-btn { vertical-align: 4px; margin-left: 10px; }'
		+ '#meowy-browser .mb-grid { position: absolute; left: 0; right: 0; top: 46px; bottom: 0; overflow-y: auto; }'
		+ '#meowy-browser .mb-gm { display: inline-block; vertical-align: top; position: relative; width: 290px; height: 54px; margin: 0 8px 8px 0; padding: 7px 10px 0 44px; box-sizing: border-box; border-radius: 6px; background: rgba(0,0,0,0.45); cursor: pointer; overflow: hidden; }'
		+ '#meowy-browser .mb-gm:hover { background: rgba(255,255,255,0.2); }'
		+ '#meowy-browser .mb-gm.noplayers { opacity: 0.55; }'
		+ '#meowy-browser .mb-gm img { position: absolute; left: 10px; top: 15px; width: 24px; height: 24px; }'
		+ '#meowy-browser .mb-gmname { font-size: 16px; font-weight: bold; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; padding-right: 4px; }'
		+ '#meowy-browser .mb-gmstats { font-size: 13px; opacity: 0.8; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }'
		+ '#meowy-browser .mb-tag { display: inline-block; font-size: 10px; font-weight: bold; text-transform: uppercase; padding: 1px 5px; margin-left: 6px; border-radius: 3px; background: rgba(255,255,255,0.2); vertical-align: 2px; line-height: 14px; }'
		+ '#meowy-browser .mb-tag.future { background: rgba(110,190,255,0.4); }'
		+ '#meowy-browser .mb-tag.old, #meowy-browser .mb-tag.bad { background: rgba(255,90,90,0.5); }'
		+ '#meowy-browser .mb-tag.good { background: rgba(110,220,130,0.45); }'
		+ '#meowy-browser .mb-install { position: absolute; right: 8px; top: 8px; font-size: 12px; padding: 2px 8px; border-radius: 10px; background: rgba(120,200,120,0.45); }'
		+ '#meowy-browser .mb-install:hover { background: rgba(120,220,120,0.7); }'
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
		+ '#meowy-browser .mb-table.wide .mb-c-name { right: 570px; }'
		+ '#meowy-browser .mb-c-gm { position: absolute; right: 410px; width: 150px; overflow: hidden; text-overflow: ellipsis; opacity: 0.85; }'
		+ '#meowy-browser .mb-c-map { position: absolute; right: 230px; width: 170px; overflow: hidden; text-overflow: ellipsis; }'
		+ '#meowy-browser .mb-srow.nomap .mb-c-map { color: #ff9a9a; }'
		+ '#meowy-browser .mb-c-ply { position: absolute; right: 140px; width: 80px; text-align: right; }'
		+ '#meowy-browser .mb-c-ping { position: absolute; right: 74px; width: 56px; text-align: right; }'
		+ '#meowy-browser .mb-c-rank { position: absolute; right: 10px; width: 52px; }'
		+ '#meowy-browser .mb-hint { opacity: 0.5; font-style: italic; }'
		+ '#meowy-browser .mb-bar { display: inline-block; height: 8px; border-radius: 2px; background: #9be27b; vertical-align: 1px; }'
		+ '#meowy-browser .mb-fav { display: inline-block; width: 18px; color: #ffd24d; opacity: 0.35; cursor: pointer; }'
		+ '#meowy-browser .mb-fav:hover, #meowy-browser .mb-fav.on { opacity: 1; }'
		+ '#meowy-browser .mb-flag { width: 16px; height: 11px; margin-right: 5px; vertical-align: -1px; }'
		+ '#meowy-browser .mb-lock { width: 12px; height: 12px; margin-right: 5px; vertical-align: -1px; }'
		+ '#meowy-browser .mb-last { float: right; font-size: 12px; opacity: 0.55; margin-left: 10px; }'
		+ '#meowy-browser .mb-lasthead { position: absolute; right: 418px; }'
		+ '#meowy-browser .mb-table.wide .mb-lasthead { right: 578px; }'
		+ '#meowy-browser .mb-empty { padding: 20px; font-size: 15px; opacity: 0.75; }'
		+ '#meowy-browser .mb-flagchip { display: inline-block; font-size: 12px; padding: 3px 7px; margin: 2px 4px 2px 0; border-radius: 10px; background: rgba(255,255,255,0.12); cursor: pointer; text-transform: uppercase; }'
		+ '#meowy-browser .mb-flagchip img { width: 16px; height: 11px; margin-right: 4px; vertical-align: -1px; }'
		+ '#meowy-browser .mb-flagchip.on { background: rgba(255,255,140,0.38); }'
		+ '#meowy-browser .mb-flagchip.off { background: rgba(255,90,90,0.3); text-decoration: line-through; }'
		+ '#meowy-browser .mb-detail { display: none; position: absolute; right: 0; top: 84px; bottom: 0; width: ' + DETAIL_W + 'px; background: rgba(0,0,0,0.55); border-radius: 6px; padding: 12px 14px; box-sizing: border-box; overflow-y: auto; overflow-x: hidden; }'
		+ '#meowy-browser .mb-x { position: absolute; right: 12px; top: 4px; font-size: 24px; cursor: pointer; opacity: 0.7; }'
		+ '#meowy-browser .mb-x:hover { opacity: 1; }'
		+ '#meowy-browser .mb-dname { font-size: 18px; font-weight: bold; margin-right: 22px; word-wrap: break-word; }'
		+ '#meowy-browser .mb-fact { font-size: 13px; margin: 3px 0; }'
		+ '#meowy-browser .mb-fact span { display: inline-block; width: 74px; opacity: 0.6; }'
		+ '#meowy-browser .mb-thumb { display: block; width: 100%; height: 150px; object-fit: cover; border-radius: 4px; margin: 8px 0 2px 0; }'
		+ '#meowy-browser .mb-join { display: inline-block; font-size: 16px; font-weight: bold; padding: 7px 18px; border-radius: 6px; background: rgba(120,200,120,0.55); cursor: pointer; margin: 10px 6px 4px 0; }'
		+ '#meowy-browser .mb-join:hover { background: rgba(120,220,120,0.8); }'
		+ '#meowy-browser .mb-join.full { background: rgba(255,255,255,0.16); }'
		+ '#meowy-browser .mb-prow { position: relative; font-size: 13px; height: 20px; line-height: 20px; white-space: nowrap; overflow: hidden; }'
		+ '#meowy-browser .mb-prow .mb-pn { position: absolute; left: 0; right: 110px; overflow: hidden; text-overflow: ellipsis; }'
		+ '#meowy-browser .mb-prow .mb-ps { position: absolute; right: 64px; width: 44px; text-align: right; opacity: 0.8; }'
		+ '#meowy-browser .mb-prow .mb-pt { position: absolute; right: 0; width: 60px; text-align: right; opacity: 0.8; }'
		+ '#meowy-browser .mb-prow.head { font-weight: bold; opacity: 0.6; }'
		+ '#meowy-browser .mb-found { font-size: 13px; padding: 5px 0; border-bottom: 1px solid rgba(255,255,255,0.1); }';

	function everyGamemode() { var g = gamemode(ALL); g.title = 'Every gamemode'; g.fixed = true; return g; }
	function newList(gen) {
		return { gen: gen, ok: true, done: false, complete: false, stopped: false, stage: 1, secs: 0, at: 0,
			servers: [], byI: {}, gms: {}, gmOrder: [], all: everyGamemode(), src: null };
	}
	// The gamemodes again from the list's servers (after updates changed counts or titles).
	function regroup(l) {
		l.gms = {};
		l.gmOrder = [];
		l.all = everyGamemode();
		for (var i = 0; i < l.servers.length; i++) {
			var s = l.servers[i];
			if (!l.gms[s.key]) { l.gms[s.key] = gamemode(s.key); l.gmOrder.push(s.key); }
			addServer(l.gms[s.key], s);
			addServer(l.all, s);
		}
	}
	function freshFilters() {
		return { notFull: false, hasPlayers: false, noPass: false, hideOutdated: false, minPlayers: 0, maxPlayers: 0, maxPing: 0, flags: {} };
	}
	var st = {
		open: false, built: false, tab: 'internet', lists: {}, gm: null, gmSort: 'players', gmQuery: '', cats: {}, f: freshFilters(),
		q: '', sortKey: 'rank', reverse: false, sel: null, view: [], viewFor: null, timer: null, first: -1, last: -1,
		players: null, playersFor: null, waitFor: null, refresh: null, selTimer: null, prefsTimer: null, prefsLoaded: false,
		passwords: {}, found: [], foundGen: 0, foundDone: true, foundAsked: ''
	};
	var ui = {};

	function L() { return st.lists[st.tab] || (st.lists[st.tab] = newList(-1)); }
	function listByGen(gen) { for (var k in st.lists) if (st.lists[k].gen === gen) return st.lists[k]; return null; }
	function current() { var l = L(); return st.gm === ALL ? l.all : st.gm ? l.gms[st.gm] : null; }
	function pageKey() { return st.tab + '|' + (st.gm || ''); }

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
	function stop(e) { if (e && e.stopPropagation) e.stopPropagation(); }
	function plural(n, one, many) { return n + ' ' + (n === 1 ? one : many); }
	function clampInt(v, lo, hi) { v = Math.floor(Number(v)); return isFinite(v) ? Math.max(lo, Math.min(hi, v)) : lo; }
	function ours() { var v = window.GMOD_VERSION_INT; return typeof v === 'number' && v > 0 ? v : 0; }
	function haveMap(map) { return typeof window.DoWeHaveMap === 'function' ? window.DoWeHaveMap(map) : true; }
	function subscribed(ws) {
		var s = window.subscriptions;
		return !!(s && typeof s.Contains === 'function' && s.Contains(String(ws)));
	}
	function flagImg(flag, cls) {
		var f = node('img', cls);
		f.onerror = function () { f.onerror = null; f.src = 'img/unk_flag.png'; };
		f.src = 'asset://garrysmod/materials/flags16/' + flag + '.png';
		return f;
	}
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
			stop(e);
		};
		return b;
	}
	function honestTag(s) {
		var h = s.honest, cls = h.indexOf('VERIFIED') === 0 ? 'good' : h.indexOf('FAKE') === 0 ? 'bad' : '';
		var t = node('span', 'mb-tag' + (cls ? ' ' + cls : ''), h);
		t.title = cls === 'good' ? "Meowy Network's observer joined it and found nothing fake"
			: cls === 'bad' ? 'Its player count is faked: the real number is shown' : 'Its player list could not be checked';
		return t;
	}
	function versionTag(s) {
		return node('span', 'mb-tag ' + (s.verC > 0 ? 'future' : 'old'), (s.verC > 0 ? 'newer: ' : 'outdated: ') + formatVersion(s.ver));
	}

	// Sort, categories and filters, kept between starts: menu_page.lua stores the
	// JSON and sends it back with the first list.
	function savePrefsSoon() {
		if (st.prefsTimer) clearTimeout(st.prefsTimer);
		st.prefsTimer = setTimeout(function () {
			st.prefsTimer = null;
			var f = st.f, flags = {}, n = 0;
			for (var k in f.flags) if (f.flags[k] && n < 24) { flags[k] = f.flags[k]; n++; }
			var p = { s: st.gmSort, c: st.cats, f: [f.notFull ? 1 : 0, f.hasPlayers ? 1 : 0, f.noPass ? 1 : 0, f.hideOutdated ? 1 : 0,
				f.minPlayers, f.maxPlayers, f.maxPing], r: flags };
			var text = JSON.stringify(p);
			if (text.length > PREFS_MAX) { p.r = {}; text = JSON.stringify(p); }
			post('browse_prefs', text);
		}, PREFS_MS);
	}
	function loadPrefs(text) {
		var p = null;
		try { p = JSON.parse(text); } catch (e) { return; }
		if (!p || typeof p !== 'object') return;
		if (p.s === 'players' || p.s === 'servers' || p.s === 'name') st.gmSort = p.s;
		if (p.c && typeof p.c === 'object') {
			for (var i = 0; i < CATS.length; i++) if (p.c[CATS[i]] === 'only' || p.c[CATS[i]] === 'hide') st.cats[CATS[i]] = p.c[CATS[i]];
		}
		if (p.f && p.f.length === 7) {
			st.f.notFull = p.f[0] === 1; st.f.hasPlayers = p.f[1] === 1; st.f.noPass = p.f[2] === 1; st.f.hideOutdated = p.f[3] === 1;
			st.f.minPlayers = clampInt(p.f[4], 0, 1000); st.f.maxPlayers = clampInt(p.f[5], 0, 1000); st.f.maxPing = clampInt(p.f[6], 0, 5000);
		}
		if (p.r && typeof p.r === 'object') {
			for (var k in p.r) if (/^[a-z]{1,20}$/.test(k) && (p.r[k] === 'prefer' || p.r[k] === 'avoid')) st.f.flags[k] = p.r[k];
		}
		syncInputs();
	}
	function filtersOn() {
		var f = st.f;
		if (f.notFull || f.hasPlayers || f.noPass || f.hideOutdated || f.minPlayers || f.maxPlayers || f.maxPing) return true;
		for (var k in f.flags) if (f.flags[k]) return true;
		return false;
	}
	function syncInputs() {
		if (!ui.minP) return;
		ui.minP.value = st.f.minPlayers ? String(st.f.minPlayers) : '';
		ui.maxP.value = st.f.maxPlayers ? String(st.f.maxPlayers) : '';
		ui.maxPing.value = st.f.maxPing ? String(st.f.maxPing) : '';
	}
	function filtersChanged(redrawSide) {
		st.viewFor = null;
		savePrefsSoon();
		if (redrawSide) drawSide();
		drawRows(true);
		drawServerPageHead();
	}

	// The left column: the lists (tabs), counts and state, Refresh or Stop, the way
	// back; on the gamemodes page their sort, categories and search; on a server
	// list its filters; on Favourites, adding a server by its address.
	function sideState() {
		var l = L();
		ui.count.textContent = plural(l.servers.length, 'server', 'servers') + (st.tab === 'internet' ? ' \u00b7 ' + plural(l.gmOrder.length, 'gamemode', 'gamemodes') : '');
		var status, src = l.src || {};
		if (!l.ok) status = st.tab === 'internet' || st.tab === 'friends'
			? "Steam's server list isn't available here: use Garry's Mod's list." : "Garry's Mod's list isn't available here.";
		else if (l.gen < 0) status = '';
		else if (src.idx === 'shown') {
			// Meowy's index first, then Steam's own answers update the rows.
			status = "From Meowy's index (" + (src.age >= 0 ? src.age + ' s old' : 'fresh') + ') \u00b7 '
				+ (l.stopped ? 'Steam stopped after ' + l.secs + ' s' : l.done ? 'checked with Steam in ' + l.secs + ' s' : 'checking with Steam\u2026 ' + l.secs + ' s');
		}
		else if (src.idx === 'waiting' && !l.servers.length) status = "Asking Meowy's index and Steam\u2026 " + l.secs + ' s';
		else if (l.stopped) status = 'Stopped after ' + l.secs + ' s';
		else if (l.done) status = 'Complete in ' + l.secs + ' s';
		else status = (st.tab === 'internet' ? (l.stage === 2 ? 'Listing empty servers\u2026 ' : 'Listing servers with players\u2026 ') : 'Listing\u2026 ') + l.secs + ' s';
		if (src.idx === 'failed' && st.tab === 'internet') status += " \u00b7 Meowy's index didn't answer: Steam's list alone";
		ui.status.textContent = status;
		ui.status.title = src.idx === 'shown' && src.geo ? 'Ping estimates use IP Geolocation by DB-IP (db-ip.com)' : '';
		var listing = l.gen >= 0 && !l.done && l.ok;
		ui.action.textContent = listing ? 'Stop' : 'Refresh';
		ui.action.onclick = listing
			? function () { l.stopped = l.done = true; post('browse_stop'); sideState(); }
			: function () { post('browse', st.tab); };
	}
	function drawSide() {
		var side = ui.side;
		clear(side);
		ui.sideFor = pageKey();
		side.appendChild(node('div', 'mb-title', 'Find servers'));
		var tabs = node('div', 'mb-tabs');
		for (var t = 0; t < TABS.length; t++) {
			var a = node('div', 'mb-link' + (st.tab === TABS[t][0] ? ' on' : ''), TABS[t][1]);
			a.onclick = (function (tab) { return function () { switchTab(tab); }; })(TABS[t][0]);
			tabs.appendChild(a);
		}
		side.appendChild(tabs);
		ui.count = side.appendChild(node('div', 'mb-small'));
		ui.status = side.appendChild(node('div', 'mb-small mb-status'));
		var row = node('div');
		ui.action = row.appendChild(button('Refresh', '', null));
		side.appendChild(row);
		sideState();
		var back = node('div');
		back.style.marginTop = '8px';
		if (st.tab === 'internet' && st.gm) {
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
		if (st.tab === 'favorite') drawFind(side);
		if (st.gm) { drawFilters(side); return; }
		side.appendChild(node('div', 'mb-label', 'Sort gamemodes by'));
		var sorts = [['players', 'Players'], ['servers', 'Servers'], ['name', 'Name']], box = node('div');
		for (var i = 0; i < sorts.length; i++) {
			box.appendChild(button(sorts[i][1], st.gmSort === sorts[i][0] ? 'on' : '', (function (by) {
				return function () { st.gmSort = by; savePrefsSoon(); drawSide(); drawGrid(); };
			})(sorts[i][0])));
		}
		side.appendChild(box);
		side.appendChild(node('div', 'mb-label', 'Categories'));
		var cats = node('div');
		for (var c = 0; c < CATS.length; c++) {
			var mode = st.cats[CATS[c]];
			var b = button(CAT_NAMES[CATS[c]], mode === 'only' ? 'on' : mode === 'hide' ? 'off' : '', (function (cat) {
				return function () { st.cats[cat] = cycle(st.cats[cat], 'only', 'hide'); savePrefsSoon(); drawSide(); drawGrid(); };
			})(CATS[c]));
			b.title = 'Click: only this, again: hide it, again: either';
			cats.appendChild(b);
		}
		side.appendChild(cats);
		side.appendChild(node('div', 'mb-label', 'Find a gamemode'));
		side.appendChild(ui.gmSearch);
	}
	function drawFilters(side) {
		side.appendChild(node('div', 'mb-label', 'Filters'));
		var box = node('div');
		for (var i = 0; i < TOGGLES.length; i++) {
			box.appendChild(button(TOGGLES[i][1], st.f[TOGGLES[i][0]] ? 'on' : '', (function (k) {
				return function () { st.f[k] = !st.f[k]; filtersChanged(true); };
			})(TOGGLES[i][0])));
		}
		side.appendChild(box);
		side.appendChild(node('div', 'mb-small', 'Players'));
		var ply = node('div');
		ply.appendChild(ui.minP);
		ply.appendChild(document.createTextNode('  to  '));
		ply.appendChild(ui.maxP);
		side.appendChild(ply);
		side.appendChild(node('div', 'mb-small', 'Ping at most'));
		side.appendChild(ui.maxPing);
		// Regions: the flags this list's servers have, each preferred, avoided or either.
		var g = current(), flags = g ? g.flagOrder.slice(0) : [];
		for (var k in st.f.flags) if (st.f.flags[k] && flags.indexOf(k) < 0) flags.push(k);
		if (flags.length) {
			flags.sort();
			side.appendChild(node('div', 'mb-label', 'Regions'));
			var regions = node('div');
			for (var r = 0; r < flags.length; r++) {
				var mode = st.f.flags[flags[r]];
				var chip = node('span', 'mb-flagchip' + (mode === 'prefer' ? ' on' : mode === 'avoid' ? ' off' : ''));
				chip.appendChild(flagImg(flags[r]));
				chip.appendChild(document.createTextNode(flags[r] === 'europeanunion' ? 'eu' : flags[r]));
				chip.title = 'Click: only these, again: never these, again: either';
				chip.onclick = (function (fl) {
					return function () { st.f.flags[fl] = cycle(st.f.flags[fl], 'prefer', 'avoid'); if (!st.f.flags[fl]) delete st.f.flags[fl]; filtersChanged(true); };
				})(flags[r]);
				regions.appendChild(chip);
			}
			side.appendChild(regions);
		}
		if (filtersOn()) {
			var reset = node('div', 'mb-link', 'Clear filters');
			reset.style.marginTop = '8px';
			reset.onclick = function () { st.f = freshFilters(); syncInputs(); filtersChanged(true); };
			side.appendChild(reset);
		}
	}
	// Favourites: a server by its address (with or without the port, as Garry's
	// Mod's own find), answered by menu_page.lua's pings.
	function drawFind(side) {
		side.appendChild(node('div', 'mb-label', 'Add a server'));
		side.appendChild(ui.find);
		var go = button('Find', '', function () { findServers(); });
		go.style.marginTop = '4px';
		side.appendChild(go);
		ui.foundBox = side.appendChild(node('div'));
		drawFound();
	}
	function findServers() {
		var text = (ui.find.value || '').replace(/^\s+|\s+$/g, '');
		if (!text) return;
		st.found = [];
		st.foundDone = false;
		st.foundAsked = text;
		post('browse_find', text.substr(0, 64));
		drawFound();
	}
	function drawFound() {
		var box = ui.foundBox;
		if (!box) return;
		clear(box);
		if (!st.foundAsked) return;
		if (!st.found.length) {
			box.appendChild(node('div', 'mb-small', st.foundDone ? 'Nothing answered at ' + st.foundAsked + '.' : 'Asking ' + st.foundAsked + '\u2026'));
			return;
		}
		for (var i = 0; i < st.found.length; i++) {
			var s = st.found[i], el = node('div', 'mb-found');
			el.appendChild(node('div', '', s.name));
			el.appendChild(node('div', 'mb-small', s.address + ' \u00b7 ' + s.players + ' / ' + s.max + ' \u00b7 ' + s.ping + ' ms'));
			el.appendChild(button(s.fav ? '\u2605 Favourite' : '\u2606 Add to favourites', s.fav ? 'on' : '', (function (sv) {
				return function () { sv.fav = !sv.fav; post('browse_fav', sv.i); drawFound(); post('browse', 'favorite'); };
			})(s)));
			el.appendChild(button('Join', '', (function (sv) { return function () { join(sv); }; })(s)));
			box.appendChild(el);
		}
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
		var grid = ui.grid, l = L();
		if (!grid) return;
		var top = grid.scrollTop || 0, list = [], anyCat = false;
		for (var c in st.cats) if (st.cats[c]) anyCat = true;
		clear(grid);
		for (var i = 0; i < l.gmOrder.length; i++) {
			var g = l.gms[l.gmOrder[i]];
			if (gamemodeShown(g, st.gmQuery, st.cats)) list.push(g);
		}
		sortGamemodes(list, st.gmSort);
		if (list.length > 1 && !st.gmQuery && !anyCat) list.unshift(l.all);
		if (!list.length) grid.appendChild(node('div', 'mb-empty', l.servers.length ? 'No gamemode matches.' : (l.ok ? 'Asking Steam for servers\u2026' : '')));
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

	// A list of servers: the title (and Install), the search, the columns, and the
	// picked server's details at the side.
	function wide() { return st.gm === ALL; }
	function pageTitle() { return st.gm === ALL ? (st.tab === 'internet' ? 'Every gamemode' : TAB_NAMES[st.tab]) : current().title; }
	function drawServerPage() {
		var g = current(), main = ui.main;
		clear(main);
		var head = node('div', 'mb-head1');
		head.appendChild(node('span', 'mb-title', pageTitle()));
		head.appendChild(node('span', 'mb-small', ''));
		var install = g.key !== ALL ? installButton(g, 'mb-btn') : null;
		if (install) head.appendChild(install);
		if (g.key !== ALL && g.ws) {
			var page = button('Workshop page', '', function () { post('browse_workshop', g.ws); });
			page.style.marginLeft = install ? '0' : '10px';
			head.appendChild(page);
		}
		main.appendChild(head);
		var tools = node('div', 'mb-tools');
		tools.appendChild(ui.search);
		main.appendChild(tools);
		var table = node('div', 'mb-table' + (wide() ? ' wide' : ''));
		ui.table = table;
		var thead = node('div', 'mb-thead');
		var cols = [['mb-c-name', 'name', 'Name']];
		if (wide()) cols.push(['mb-c-gm', 'gm', 'Gamemode']);
		cols.push(['mb-c-map', 'map', 'Map'], ['mb-c-ply', 'players', 'Players'], ['mb-c-ping', 'ping', 'Ping'], ['mb-c-rank', 'rank', 'Ranking']);
		if (st.tab === 'history' || st.sortKey === 'last') cols.push(['mb-lasthead', 'last', 'Last visit']);
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
		ui.detail = main.appendChild(node('div', 'mb-detail'));
		st.first = st.last = -1;
		drawServerPageHead();
		drawRows(true);
		buildDetail();
	}
	function drawServerPageHead() {
		var g = current(), head = ui.main && ui.main.firstChild;
		if (!g || !head || !head.children || !st.gm) return;
		var small = head.children[1];
		updateView();
		var shown = st.view.length !== g.count ? plural(st.view.length, 'server', 'servers') + ' shown of ' + g.count : plural(g.count, 'server', 'servers');
		if (small) small.textContent = plural(g.players, 'player', 'players') + ' on ' + shown;
	}

	// The servers that pass, sorted; worked out again only when something changed.
	function filters() {
		var f = st.f;
		return { q: st.q, notFull: f.notFull, hasPlayers: f.hasPlayers, noPass: f.noPass, hideOutdated: f.hideOutdated,
			minPlayers: f.minPlayers, maxPlayers: f.maxPlayers, maxPing: f.maxPing, flags: f.flags };
	}
	function updateView() {
		var g = current();
		if (!g) { st.view = []; return; }
		var key = st.tab + '|' + g.key + '|' + g.count + '|' + st.q + '|' + st.sortKey + '|' + st.reverse;
		if (st.viewFor === key) return;
		var f = filters(), out = [], gms = L().gms;
		for (var i = 0; i < g.servers.length; i++) {
			var s = g.servers[i];
			if (!serverShown(s, f)) continue;
			if (wide()) { var gm = gms[s.key]; s.gmName = gm ? gm.title : s.gm; s.gmTitle = s.gmName.toLowerCase(); }
			out.push(s);
		}
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
			var g = current();
			ui.rows.appendChild(node('div', 'mb-empty', g && g.count ? 'No server matches the search or filters.' : (L().done ? 'No servers here.' : 'No servers yet.')));
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
		fav.onclick = function (e) { toggleFav(s); stop(e); };
		name.appendChild(fav);
		if (s.lastText) name.appendChild(node('span', 'mb-last', (st.tab === 'history' ? '' : 'last visit ') + s.lastText));
		if (s.flag) name.appendChild(flagImg(s.flag, 'mb-flag'));
		if (s.pass) {
			var lock = node('img', 'mb-lock');
			lock.src = 'img/server-passworded.png';
			lock.title = 'Needs a password';
			name.appendChild(lock);
		}
		name.appendChild(document.createTextNode(s.name));
		if (s.honest) name.appendChild(honestTag(s));
		if (s.verC) name.appendChild(versionTag(s));
		el.appendChild(name);
		if (wide()) el.appendChild(node('span', 'mb-c-gm', s.gmName || s.gm));
		var map = node('span', 'mb-c-map', s.map);
		if (!s.hasmap) map.title = "You don't have this map: it downloads when you join";
		el.appendChild(map);
		el.appendChild(node('span', 'mb-c-ply', s.players + ' / ' + s.max));
		var ping = node('span', 'mb-c-ping' + (s.hint ? ' mb-hint' : ''), pingText(s));
		if (s.hint) ping.title = HINT_TITLES[s.hint];
		el.appendChild(ping);
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
	function toggleFav(s) {
		s.fav = !s.fav;
		post('browse_fav', s.i);
		drawRows(true);
		if (selectedServer() === s) buildDetail();
	}

	// The picked server: its details and players (asked for now and every 10
	// seconds while it stays picked), its password, Join, and "Join when a slot
	// opens" for a full one (asked every 5 seconds, joined the moment there's room).
	function selectedServer() { return st.sel === null ? null : (L().byI[st.sel] || null); }
	function scheduleRefresh() {
		if (st.refresh) clearTimeout(st.refresh);
		st.refresh = null;
		if (st.sel === null) return;
		st.refresh = setTimeout(function () {
			st.refresh = null;
			if (!st.open || st.sel === null) return;
			post('browse_select', st.sel);
			scheduleRefresh();
		}, st.waitFor !== null ? WAIT_MS : REFRESH_MS);
	}
	function select(i) {
		if (st.sel === i) return;
		st.sel = i;
		st.players = null;
		st.playersFor = null;
		st.waitFor = null;
		if (st.selTimer) clearTimeout(st.selTimer);
		st.selTimer = null;
		if (i !== null) {
			// Arrowing through the list doesn't ask every server it passes.
			st.selTimer = setTimeout(function () { st.selTimer = null; if (st.sel === i) post('browse_select', i); }, SELECT_MS);
		}
		scheduleRefresh();
		drawRows(true);
		buildDetail();
	}
	function join(s) {
		if (!s) return;
		var pw = st.passwords[s.i] || '';
		if (s.pass && !pw && L().byI[s.i] === s) {
			// The password first: pick it, and the field is ready to type in.
			if (st.sel !== s.i) select(s.i);
			if (ui.pass && ui.pass.focus) ui.pass.focus();
			return;
		}
		if (s.pass && pw) post('browse_pass', s.i + ':' + pw);
		st.waitFor = null;
		post('browse_join', s.i);
	}
	function fact(box, label, text) {
		var f = node('div', 'mb-fact');
		f.appendChild(node('span', '', label));
		f.appendChild(document.createTextNode(text));
		box.appendChild(f);
	}
	function buildDetail() {
		var d = ui.detail;
		if (!d || !ui.table) return;
		var s = selectedServer();
		ui.table.style.right = s ? (DETAIL_W + 12) + 'px' : '0px';
		d.style.display = s ? 'block' : 'none';
		clear(d);
		if (!s) return;
		var x = node('span', 'mb-x', '\u00d7');
		x.title = 'Close';
		x.onclick = function () { select(null); };
		d.appendChild(x);
		d.appendChild(node('div', 'mb-dname', s.name));
		d.appendChild(node('div', 'mb-small', s.address));
		ui.dTags = d.appendChild(node('div'));
		ui.dFacts = d.appendChild(node('div'));
		if (s.hasmap && /^[\w.\-]+$/.test(s.map)) {
			var thumb = node('img', 'mb-thumb');
			thumb.onerror = function () { if (thumb.parentNode) thumb.parentNode.removeChild(thumb); };
			thumb.src = 'asset://mapimage/' + s.map;
			d.appendChild(thumb);
		}
		if (s.pass) {
			ui.pass.value = st.passwords[s.i] || '';
			ui.pass.passFor = s.i;
			var pw = node('div');
			pw.style.marginTop = '8px';
			pw.appendChild(ui.pass);
			d.appendChild(pw);
		}
		ui.dActions = d.appendChild(node('div'));
		var more = node('div');
		ui.dFav = more.appendChild(button('', '', function () { toggleFav(s); }));
		more.appendChild(button('Copy address', '', function () { post('browse_copy', s.i); }));
		var g = L().gms[s.key];
		if (g && g.ws) more.appendChild(button('Workshop page', '', function () { post('browse_workshop', g.ws); }));
		d.appendChild(more);
		ui.dPlayers = d.appendChild(node('div'));
		updateDetail();
	}
	function updateDetail() {
		var s = selectedServer();
		if (!s || !ui.dFacts) return;
		clear(ui.dTags);
		if (s.honest) ui.dTags.appendChild(honestTag(s));
		if (s.verC) ui.dTags.appendChild(versionTag(s));
		if (s.pass) ui.dTags.appendChild(node('span', 'mb-tag', 'password'));
		if (s.listen) ui.dTags.appendChild(node('span', 'mb-tag', 'listen server'));
		if (s.anon) ui.dTags.appendChild(node('span', 'mb-tag', 'anonymous'));
		clear(ui.dFacts);
		fact(ui.dFacts, 'Gamemode', (s.desc || s.gm) + (s.desc && s.desc.toLowerCase() !== s.key ? ' (' + s.key + ')' : ''));
		fact(ui.dFacts, 'Map', s.map + (s.hasmap ? '' : "  (you don't have it: it downloads when you join)"));
		fact(ui.dFacts, 'Players', s.players + ' / ' + s.max + (s.bots ? ' and ' + plural(s.bots, 'bot', 'bots') : '') + (s.honest.indexOf('FAKE') === 0 ? ' (the real count)' : ''));
		fact(ui.dFacts, 'Ping', s.hint === 3 ? 'not measured yet' : pingText(s) + ' ms' + (s.hint === 1 ? ' (your last visit)' : s.hint === 2 ? ' (estimate)' : ''));
		if (s.ver) fact(ui.dFacts, 'Version', formatVersion(s.ver) + (s.verC < 0 ? " (older than yours: you can't join)" : s.verC > 0 ? ' (newer than yours)' : ''));
		if (s.flag) fact(ui.dFacts, 'Region', s.flag === 'europeanunion' ? 'EU' : s.flag.toUpperCase());
		if (s.lastText) fact(ui.dFacts, 'Last visit', s.lastText);
		clear(ui.dActions);
		var full = s.players >= s.max, waiting = st.waitFor === s.i;
		var jb = node('span', 'mb-join' + (full ? ' full' : ''), full ? 'Join anyway' : 'Join');
		jb.title = full ? 'It is full: the game will say so unless a slot opens' : '';
		jb.onclick = function () { join(s); };
		ui.dActions.appendChild(jb);
		if (full || waiting) {
			var w = button(waiting ? 'Waiting for a slot\u2026 (cancel)' : 'Join when a slot opens', waiting ? 'on' : '', function () {
				st.waitFor = waiting ? null : s.i;
				if (st.waitFor !== null) post('browse_select', s.i);
				scheduleRefresh();
				updateDetail();
			});
			ui.dActions.appendChild(w);
		}
		ui.dFav.textContent = s.fav ? '\u2605 Favourite' : '\u2606 Add to favourites';
		ui.dFav.className = 'mb-btn' + (s.fav ? ' on' : '');
		var box = ui.dPlayers;
		clear(box);
		box.appendChild(node('div', 'mb-label', 'Players'));
		if (st.playersFor !== s.address) {
			box.appendChild(node('div', 'mb-small', 'Asking the server\u2026'));
			return;
		}
		var list = st.players || [];
		if (!list.length) {
			box.appendChild(node('div', 'mb-small', s.players > 0 ? "The server didn't send its player list." : 'Nobody is playing.'));
			return;
		}
		var head = node('div', 'mb-prow head');
		head.appendChild(node('span', 'mb-pn', plural(list.length, 'player', 'players')));
		head.appendChild(node('span', 'mb-ps', 'Score'));
		head.appendChild(node('span', 'mb-pt', 'Time'));
		box.appendChild(head);
		for (var i = 0; i < list.length; i++) {
			var row = node('div', 'mb-prow');
			row.appendChild(node('span', 'mb-pn', list[i].name || '(no name)'));
			row.appendChild(node('span', 'mb-ps', String(list[i].score)));
			row.appendChild(node('span', 'mb-pt', duration(list[i].time)));
			box.appendChild(row);
		}
	}
	function duration(sec) {
		sec = Math.max(0, Math.floor(sec));
		if (sec < 60) return sec + ' s';
		if (sec < 3600) return Math.floor(sec / 60) + ' min';
		var m = Math.floor(sec / 60) % 60;
		return Math.floor(sec / 3600) + ' h ' + (m < 10 ? '0' : '') + m;
	}
	function toPlayers(players) {
		var out = [];
		if (!players || typeof players !== 'object') return out;
		for (var k in players) {
			var p = players[k];
			if (!p || typeof p !== 'object' || out.length >= PLAYERS_MAX) continue;
			out.push({ name: str(p.name), score: int(p.score), time: Number(p.time) || 0 });
		}
		out.sort(function (a, b) { return b.time - a.time; });
		return out;
	}

	// The game's Lua answers the picked server through the stock page's own
	// functions (GetPlayerList -> SetPlayerList, PingServer -> UpdateServer): this
	// page listens in, and the stock browser still gets them.
	var stockSetPlayerList = window.SetPlayerList, stockUpdateServer = window.UpdateServer;
	// The stock functions read its browser's scope, which is null until it has been opened.
	function stockReady() { return typeof stockSetPlayerList === 'function' && !!window.RootScope; }
	window.SetPlayerList = function (ip, players) {
		var s = selectedServer();
		if (st.open && s && s.address === ip) {
			st.players = toPlayers(players);
			st.playersFor = ip;
			updateDetail();
		}
		if (stockReady()) return stockSetPlayerList.apply(this, arguments);
	};
	window.UpdateServer = function (address, ping, name, map, players, maxplayers, botplayers, pass) {
		var s = selectedServer();
		if (st.open && s && s.address === address) {
			var bots = Math.max(0, int(botplayers)), real = s.honest.indexOf('FAKE') === 0 ? s.players : null;
			s.ping = Math.max(0, int(ping));
			s.hint = 0;
			if (typeof map === 'string' && map) { s.map = map; s.lmap = map.toLowerCase(); }
			s.bots = bots;
			s.players = Math.max(0, int(players) - bots);
			if (real !== null) s.players = Math.min(s.players, real);
			s.max = Math.max(0, int(maxplayers) - bots);
			s.pass = pass === true || pass === 1 || pass === '1' || pass === 'true';
			s.rank = rank(s) + s.penalty;
			st.viewFor = null;
			if (st.waitFor === s.i && s.players < s.max) {
				st.waitFor = null;
				join(s);
			}
			drawRows(true);
			updateDetail();
		}
		if (stockReady() && typeof stockUpdateServer === 'function') return stockUpdateServer.apply(this, arguments);
	};

	function moveSelection(dir) {
		updateView();
		if (!st.view.length) return;
		var at = -1;
		for (var i = 0; i < st.view.length; i++) if (st.view[i].i === st.sel) { at = i; break; }
		at = at < 0 ? (dir > 0 ? 0 : st.view.length - 1) : Math.max(0, Math.min(st.view.length - 1, at + dir));
		var body = ui.body;
		if (body) {
			var top = at * ROW_H, h = body.clientHeight || 700;
			if (top < body.scrollTop) body.scrollTop = top;
			else if (top + ROW_H > body.scrollTop + h) body.scrollTop = top + ROW_H - h;
		}
		select(st.view[at].i);
	}

	function pickGamemode(key) {
		st.gm = key;
		st.q = '';
		ui.search.value = '';
		st.viewFor = null;
		select(null);
		render();
		var f = key ? ui.search : ui.gmSearch;
		if (f.focus) f.focus();
	}
	function switchTab(tab) {
		if (st.tab === tab) return;
		var was = L();
		if (was.gen >= 0 && !was.done) { was.done = was.stopped = true; }
		st.tab = tab;
		st.gm = tab === 'internet' ? null : ALL;
		st.q = '';
		ui.search.value = '';
		st.sortKey = tab === 'history' ? 'last' : 'rank';
		st.reverse = false;
		st.viewFor = null;
		st.sel = null;
		st.waitFor = null;
		var l = st.lists[tab];
		if (!l || !l.complete || Date.now() - l.at > STALE) post('browse', tab);
		scheduleIndexRefresh();
		render();
		var f = st.gm ? ui.search : ui.gmSearch;
		if (f && f.focus) f.focus();
	}

	// The page is drawn when it changes (the gamemodes, or a list of servers);
	// otherwise only the counts, the tiles or the rows, so typing and scrolling
	// aren't disturbed while servers arrive.
	function render() {
		if (!st.built || !st.open) return;
		if (st.gm && !current()) st.gm = st.tab === 'internet' ? null : ALL;
		var page = pageKey();
		if (ui.sideFor !== page) drawSide(); else sideState();
		if (ui.mainFor === page) {
			if (st.gm) { drawServerPageHead(); drawRows(true); updateDetail(); } else drawGrid();
			return;
		}
		ui.mainFor = page;
		if (st.gm) { ui.grid = null; drawServerPage(); } else { ui.body = null; ui.detail = null; ui.table = null; drawGamemodes(); }
	}

	function numberInput(placeholder, max, onValue) {
		var e = node('input', 'mb-input mb-num');
		e.type = 'text';
		e.maxLength = 4;
		e.placeholder = placeholder;
		e.oninput = function () {
			var v = clampInt(e.value.replace(/[^0-9]/g, ''), 0, max);
			onValue(v);
			st.viewFor = null;
			savePrefsSoon();
			drawRows(true);
			drawServerPageHead();
		};
		return e;
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
		ui.search.oninput = function () { st.q = ui.search.value; st.viewFor = null; drawRows(true); drawServerPageHead(); };
		ui.search.onkeydown = function (e) {
			var k = e.keyCode;
			if (k === 40 || k === 38) { moveSelection(k === 40 ? 1 : -1); e.preventDefault(); }
			else if (k === 13) { updateView(); join(selectedServer() || st.view[0]); e.preventDefault(); }
			else if (k === 27) {
				if (ui.search.value) { ui.search.value = ''; st.q = ''; st.viewFor = null; drawRows(true); drawServerPageHead(); }
				else if (st.tab === 'internet' && st.gm) pickGamemode(null);
				else window.MeowyBrowser.open(false);
				e.preventDefault();
			}
		};

		ui.minP = numberInput('0', 1000, function (v) { st.f.minPlayers = v; });
		ui.maxP = numberInput('any', 1000, function (v) { st.f.maxPlayers = v; });
		ui.maxPing = numberInput('any', 5000, function (v) { st.f.maxPing = v; });
		ui.maxPing.style.width = '100px';
		syncInputs();

		ui.pass = node('input', 'mb-input mb-pass');
		ui.pass.type = 'password';
		ui.pass.maxLength = 64;
		ui.pass.placeholder = 'Password';
		ui.pass.oninput = function () { if (ui.pass.passFor !== undefined) st.passwords[ui.pass.passFor] = ui.pass.value; };
		ui.pass.onkeydown = function (e) { if (e.keyCode === 13) { join(selectedServer()); e.preventDefault(); } };

		ui.find = node('input', 'mb-input mb-find');
		ui.find.type = 'text';
		ui.find.maxLength = 64;
		ui.find.placeholder = '1.2.3.4 or 1.2.3.4:27015';
		ui.find.onkeydown = function (e) { if (e.keyCode === 13) { findServers(); e.preventDefault(); } };
		st.built = true;
		return root;
	}

	// Data from menu_page.lua. A list is a generation: rows of an older one are
	// dropped. stage 1: servers with players, 2: the empty ones after them.
	function scheduleRender() {
		if (st.timer) return;
		st.timer = setTimeout(function () { st.timer = null; render(); }, RENDER_MS);
	}
	// While the Internet list from Meowy's index is open: a newer index each minute.
	function scheduleIndexRefresh() {
		var want = st.open && st.tab === 'internet' && !!L().src && L().src.idx === 'shown';
		if (!want) {
			if (st.indexTimer) clearTimeout(st.indexTimer);
			st.indexTimer = null;
			return;
		}
		if (st.indexTimer) return;
		st.indexTimer = setTimeout(function () {
			st.indexTimer = null;
			if (st.open && st.tab === 'internet') post('browse_refresh');
			scheduleIndexRefresh();
		}, INDEX_REFRESH_MS);
	}
	window.MeowyBrowse = {
		reset: function (gen, ok, tab, prefs) {
			if (!st.prefsLoaded && typeof prefs === 'string' && prefs) { loadPrefs(prefs); ui.sideFor = null; }
			st.prefsLoaded = true;
			tab = TAB_NAMES[tab] ? tab : 'internet';
			var l = newList(gen);
			l.ok = !!ok;
			l.done = !ok;
			l.at = Date.now();
			st.lists[tab] = l;
			if (tab === st.tab) {
				st.sel = null;
				st.waitFor = null;
				st.viewFor = null;
				if (st.gm && st.gm !== ALL) st.gm = null;
				ui.mainFor = null;
			}
			render();
		},
		add: function (gen, rows, finished, secs, stage) {
			var l = listByGen(gen);
			if (!l || !rows) return;
			var v = ours();
			for (var i = 0; i < rows.length; i++) {
				var r = rows[i];
				if (!r || typeof r.i !== 'number' || typeof r.n !== 'string' || l.byI[r.i]) continue;
				var s = server(r, v, haveMap);
				if (s.max <= 1) continue;   // as Garry's Mod's list: not a server to join
				l.byI[s.i] = s;
				l.servers.push(s);
				if (!l.gms[s.key]) { l.gms[s.key] = gamemode(s.key); l.gmOrder.push(s.key); }
				addServer(l.gms[s.key], s);
				addServer(l.all, s);
			}
			if (finished) { l.done = l.complete = true; l.stopped = false; }
			if (stage) l.stage = stage;
			l.secs = secs;
			if (l === L()) { st.viewFor = null; scheduleRender(); }
		},
		// Rows the list already has (same numbers), with newer numbers: Steam's
		// answers for rows from Meowy's index, or a newer index.
		update: function (gen, rows) {
			var l = listByGen(gen);
			if (!l || !rows) return;
			var v = ours(), changed = false;
			for (var i = 0; i < rows.length; i++) {
				var r = rows[i];
				if (!r || typeof r.i !== 'number' || typeof r.n !== 'string') continue;
				var s = l.byI[r.i];
				if (!s || s.address !== str(r.a)) continue;
				updateServer(s, server(r, v, haveMap));
				changed = true;
			}
			if (!changed) return;
			regroup(l);
			if (l === L()) { st.viewFor = null; scheduleRender(); }
		},
		// Where the list comes from: { idx: "waiting" | "shown" | "failed" | "off", age, geo, n }.
		source: function (gen, info) {
			var l = listByGen(gen);
			if (!l || !info || typeof info !== 'object') return;
			l.src = { idx: str(info.idx), age: int(info.age), geo: info.geo === true, n: int(info.n) };
			if (l === L()) { scheduleIndexRefresh(); scheduleRender(); }
		},
		// Find by address's answers (Favourites).
		found: function (gen, rows, done) {
			if (gen < st.foundGen || !rows) return;
			st.foundGen = gen;
			var v = ours(), out = [];
			for (var i = 0; i < rows.length; i++) {
				if (rows[i] && typeof rows[i].i === 'number' && typeof rows[i].n === 'string') out.push(server(rows[i], v, haveMap));
			}
			st.found = out;
			st.foundDone = !!done;
			drawFound();
		},
		flush: function () { if (st.timer) { clearTimeout(st.timer); st.timer = null; } render(); },
		// The tests' and shell's way in: open(false) closes.
		open: function (on) { window.MeowyBrowser.open(on !== false); }
	};

	window.MeowyBrowser = {
		// The browser's element, built once (the home screen puts it in its page).
		element: function () { return st.built ? ui.root : build(); },
		isOpen: function () { return st.open; },
		// Opens it (asking for a list if there's none, it didn't finish or it's old)
		// or closes it (stopping the list). onToggle(open) lets the home screen show
		// or hide it.
		open: function (on) {
			on = on !== false;
			if (on === st.open) { if (on) render(); return; }
			st.open = on;
			if (typeof window.MeowyBrowser.onToggle === 'function') window.MeowyBrowser.onToggle(on);
			if (on) {
				if (!st.built) build();
				var l = L();
				if (!l.servers.length || !l.complete || Date.now() - l.at > STALE) post('browse', st.tab);
				render();
				var f = st.gm ? ui.search : ui.gmSearch;
				if (f && f.focus) f.focus();
				scheduleRefresh();
			} else {
				if (L().gen >= 0 && !L().done) post('browse_stop');
				if (st.refresh) clearTimeout(st.refresh);
				st.refresh = null;
			}
			scheduleIndexRefresh();
		},
		focusSearch: function () { var f = st.gm ? ui.search : ui.gmSearch; if (f && f.focus) f.focus(); },
		onToggle: null
	};
})();
