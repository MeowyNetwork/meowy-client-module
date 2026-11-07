// Meowy home screen (preview; meowy_menu 1): our home inside Garry's Mod's own
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
		+ '#meowy-shell .ms-detail { font-size: 14px; opacity: 0.75; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }';
	// [label, kind, value]: "route" goes to the page's own screen; the rest are posts.
	var LINKS = [
		['Find servers', 'route', 'servers'], ['Start new game', 'route', 'newgame'], null,
		['Addons', 'route', 'addons'], ['Dupes', 'route', 'dupes'], ['Saves', 'route', 'saves'], ['Demos', 'route', 'demos'], null,
		['Options', 'options', ''], ['Meowy settings', 'settings', ''], null,
		['Quit', 'quit', '']
	];
	var MAX_MATCHES = 10;

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
		nav.appendChild(node('div', 'ms-tag', 'Meowy home (preview)'));
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
		search.placeholder = 'Search your servers   ( / )';
		search.value = query;
		search.oninput = function () { query = search.value; selected = 0; lists(); };
		search.onfocus = function () { focused = true; mark(); };
		search.onblur = function () { focused = false; mark(); };
		search.onkeydown = keys;
		home.appendChild(search);
		home.appendChild(node('div', 'ms-lists'));
		root.appendChild(home);
		return root;
	}

	function mark() {
		for (var i = 0; i < rows.length; i++) {
			var sel = focused && i === selected;   // the row Enter joins, while the search box has the keyboard
			rows[i].el.className = rows[i].cls + (sel ? ' ms-sel' : '');
			if (sel && rows[i].el.scrollIntoView) rows[i].el.scrollIntoView(false);
		}
	}

	// The lists: each section's first rows; while searching, every row whose
	// name or map has the text (at most MAX_MATCHES a section).
	function lists() {
		var s = shell();
		if (!s) return;
		var list = s.querySelector('.ms-lists');
		var data = window.MeowyHomeData, q = query.toLowerCase();
		while (list.firstChild) list.removeChild(list.firstChild);
		rows = [];
		if (!data || !data.sections) return;
		for (var i = 0; i < data.sections.length; i++) {
			var sec = data.sections[i], picked = [];
			for (var j = 0; j < sec.rows.length; j++) {
				var r = sec.rows[j];
				if (q && (r.name + ' ' + r.detail).toLowerCase().indexOf(q) < 0) continue;
				picked.push(r);
				if (picked.length >= (q ? MAX_MATCHES : Math.max(sec.show, 8))) break;
			}
			if (q && !picked.length) continue;
			list.appendChild(node('div', 'ms-title', sec.title));
			for (var k = 0; k < picked.length; k++) {
				var p = picked[k], cls = 'ms-row' + (p.dim ? ' ms-dim' : '');
				var el = node('div', cls);
				if (p.stats) el.appendChild(node('span', 'ms-stats', p.stats));
				el.appendChild(node('div', 'ms-name', p.name));
				if (p.detail) el.appendChild(node('div', 'ms-detail', p.detail));
				el.title = p.name;
				el.onclick = (function (id) { return function () { post('join', Number(id)); }; })(p.id);
				list.appendChild(el);
				rows.push({ id: p.id, el: el, cls: cls });
			}
			if (!q && sec.note) list.appendChild(node('div', 'ms-note', sec.note));
		}
		if (q && !rows.length) list.appendChild(node('div', 'ms-note', 'No server matches'));
		if (selected >= rows.length) selected = rows.length - 1;
		if (selected < 0) selected = 0;
		mark();
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
			if (rows[selected]) post('join', Number(rows[selected].id));
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
		if (want === on && (!on || shell() || !document.querySelector(MAIN))) return;
		on = want;
		draw();
	};
	window.MeowyShellLists = lists;
	window.MeowyShellContinue = continueEntry;

	// "/" anywhere on our home (not while typing elsewhere) goes to the search box.
	document.addEventListener('keydown', function (e) {
		var s = shell(), t = e.target && e.target.tagName;
		if (!s || t === 'INPUT' || t === 'TEXTAREA') return;
		if (e.key === '/' || e.keyCode === 191) { s.querySelector('.ms-search').focus(); e.preventDefault(); }
	}, true);

	var MO = window.MutationObserver || window.WebKitMutationObserver;
	if (MO && document.body) new MO(function () { if (on) draw(); }).observe(document.body, { childList: true, subtree: true });
})();
