// The Home card's and Escape card's page code (the JS inside src/lua/home.lua),
// run against a small fake DOM, no game:
//   node tests/home_page_test.js        (from the repository root)
// What it can't show: how the stock menu's CSS lays it out.
'use strict';
const fs = require('fs');
const path = require('path');

const lua = fs.readFileSync(path.join(__dirname, '..', 'src', 'lua', 'home.lua'), 'utf8');
function block(name) {
	const m = lua.match(new RegExp('local ' + name + ' = \\[==\\[([\\s\\S]*?)\\]==\\]'));
	if (!m) throw new Error('no ' + name + ' in home.lua');
	return m[1];
}

let failures = 0, count = 0;
function check(name, got, want) {
	count++;
	if (got !== want) {
		failures++;
		console.log(`FAIL ${name}: got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`);
	}
}

// A small DOM: elements with classes, ids, attributes, children, text.
class El {
	constructor(tag) {
		this.tagName = tag.toUpperCase();
		this.children = [];
		this.parentNode = null;
		this.attrs = {};
		this.style = {};
		this.className = '';
		this._text = '';
		this.id = '';
		this.value = '';
	}
	get firstChild() { return this.children[0] || null; }
	get nextSibling() { const p = this.parentNode; return p ? p.children[p.children.indexOf(this) + 1] || null : null; }
	appendChild(c) { if (c.parentNode) c.parentNode.removeChild(c); c.parentNode = this; this.children.push(c); return c; }
	insertBefore(c, ref) {
		if (c.parentNode) c.parentNode.removeChild(c);
		const i = this.children.indexOf(ref);
		c.parentNode = this;
		if (i < 0) this.children.push(c); else this.children.splice(i, 0, c);
		return c;
	}
	removeChild(c) { const i = this.children.indexOf(c); if (i >= 0) this.children.splice(i, 1); c.parentNode = null; return c; }
	setAttribute(k, v) { this.attrs[k] = String(v); }
	getAttribute(k) { return k in this.attrs ? this.attrs[k] : null; }
	set textContent(t) { this.children = []; this._text = String(t); }
	get textContent() { return this._text + this.children.map(c => c.textContent).join(''); }
	matches(sel) {
		let m;
		if (sel.includes(',')) return sel.split(',').some(s => this.matches(s.trim()));
		if ((m = sel.match(/^#([\w-]+) a\[data-act\]$/))) {
			let p = this.parentNode;
			while (p && p.id !== m[1]) p = p.parentNode;
			return this.tagName === 'A' && 'data-act' in this.attrs && !!p;
		}
		if ((m = sel.match(/^\.page\[ng-controller="(\w+)"\]$/))) return this.className === 'page' && this.attrs['ng-controller'] === m[1];
		if ((m = sel.match(/^(\w+)\[([\w-]+)="(.*)"\]$/))) return this.tagName === m[1].toUpperCase() && this.attrs[m[2]] === m[3];
		if ((m = sel.match(/^\.([\w-]+)$/))) return this.className.split(' ').includes(m[1]);
		if ((m = sel.match(/^#([\w-]+)$/))) return this.id === m[1];
		return false;
	}
	all(out = []) { for (const c of this.children) { out.push(c); c.all(out); } return out; }
	querySelector(sel) { return this.all().find(e => e.matches(sel)) || null; }
	querySelectorAll(sel) { return this.all().filter(e => e.matches(sel)); }
	focus() { doc.activeElement = this; if (this.onfocus) this.onfocus(); }
	blur() { if (doc.activeElement === this) doc.activeElement = null; if (this.onblur) this.onblur(); }
	scrollIntoView() {}
}
class Text extends El { constructor(t) { super('#text'); this._text = t; } }

let doc, listeners, luaRuns;
let timers = [];   // setTimeout callbacks, run by flush()
function flush() { const t = timers; timers = []; t.forEach(f => f()); }
function fresh() {
	listeners = [];
	luaRuns = [];
	const body = new El('body');
	const head = new El('head');
	const page = new El('div');
	page.className = 'page';
	page.setAttribute('ng-controller', 'ControllerMain');
	body.appendChild(page);
	doc = {
		body, head, activeElement: null,
		createElement: t => new El(t),
		createTextNode: t => new Text(t),
		getElementById: id => [head, body].map(r => r.all().find(e => e.id === id)).find(Boolean) || null,
		querySelector: sel => body.querySelector(sel),
		querySelectorAll: sel => body.querySelectorAll(sel),
		addEventListener: (type, fn) => listeners.push({ type, fn }),
	};
	const win = {};
	const run = new Function('window', 'document', 'lua', 'setTimeout', block('HOME_JS') + '\n' + block('ESC_JS'));
	timers = [];
	run(win, doc, { Run: s => luaRuns.push(s) }, fn => { timers.push(fn); return timers.length; });
	return { win, page };
}
function key(target, keyCode, keyName) {
	const e = { keyCode, key: keyName, target, prevented: false, preventDefault() { this.prevented = true; }, stopPropagation() {} };
	return e;
}
function visibleNames(box) { return box.querySelectorAll('.mh-name').map(e => e.textContent); }
function row(id, name, detail) { return { id, name, detail: detail || '', stats: '1/2 · 3 ms', dim: false }; }

// ---------------------------------------------------------------------------
let { win, page } = fresh();
const recent = [];
for (let i = 0; i < 12; i++) recent.push(row(10 + i, 'Server ' + i, i % 2 ? 'rp_downtown' : 'gm_construct'));
const data = {
	sections: [
		{ title: 'Meowy Network', show: 1, rows: [row(1, 'Meowy Network | DarkRP', 'rp_downtown_meowy')] },
		{ title: 'Favourites', show: 5, note: '2 more aren\'t answering', rows: [row(2, 'Big RP', 'rp_city'), row(3, 'Small TTT', 'ttt_67thway')] },
		{ title: 'Recent', show: 5, rows: recent },
	],
};
win.MeowyHome(data, 1);
let box = doc.getElementById('meowy-home');
check('card added to the main page', box && box.parentNode === page, true);
check('style added once', doc.head.querySelectorAll('#meowy-home-style').length + (doc.getElementById('meowy-home-style') ? 0 : 0), 1);
check('each section shows its first rows', visibleNames(box).length, 1 + 2 + 5);
check('the note under favourites', box.querySelectorAll('.mh-note').map(e => e.textContent).join('|'), "2 more aren't answering");
check('names are text', box.querySelector('.mh-name').textContent, 'Meowy Network | DarkRP');

// Clicking a row sends only its number.
box.querySelectorAll('.mh-row')[1].onclick();
check('click joins by number', luaRuns.pop(), 'MeowyClientPackHomeJoin(2)');
// Resting the pointer on a row: after a moment, that server is got ready (prewarm.h).
const hoverRow = box.querySelectorAll('.mh-row')[1];
hoverRow.onmouseenter();
check('hover: nothing at once', luaRuns.length, 0);
flush();
check('hover: after a moment, the row number', luaRuns.pop(), 'MeowyClientPackHomeIntent(2)');
hoverRow.onmouseenter();
hoverRow.onmouseleave();
check('hover: leaving first sends nothing', luaRuns.length, 0);

// Search: every row is searched, not just the shown ones.
const input = box.querySelector('.mh-search');
check('a search box', !!input, true);
input.value = 'server 1';
input.oninput();
check('search finds hidden rows', visibleNames(box).join(','), 'Server 1,Server 10,Server 11');
check('sections without matches hidden', box.querySelectorAll('.mh-title').map(e => e.textContent).join(','), 'Recent');
check('no note while searching', box.querySelectorAll('.mh-note').length, 0);
input.value = 'rp_downtown';
input.oninput();
check('search matches the map too, at most 8 a section', visibleNames(box).filter(n => n.startsWith('Server')).length, 6);
check('and other sections', visibleNames(box).includes('Meowy Network | DarkRP'), true);
input.value = 'nothing like this';
input.oninput();
check('no match: a note', box.querySelector('.mh-note').textContent, 'No server matches');
input.value = 'BIG';
input.oninput();
check('case doesn\'t matter', visibleNames(box).join(','), 'Big RP');

// Keyboard: focus marks the first row, arrows move, Enter joins, Esc clears.
input.value = 'server';
input.oninput();
input.focus();
let sel = box.querySelectorAll('.mh-sel');
check('focus marks the first match', sel.length === 1 && sel[0].querySelector('.mh-name').textContent, 'Server 0');
let e = key(input, 40); input.onkeydown(e);
check('down moves', box.querySelector('.mh-sel').querySelector('.mh-name').textContent, 'Server 1');
check('the key is taken', e.prevented, true);
input.onkeydown(key(input, 38));
input.onkeydown(key(input, 38));
check('up stops at the top', box.querySelector('.mh-sel').querySelector('.mh-name').textContent, 'Server 0');
input.onkeydown(key(input, 40));
input.onkeydown(key(input, 13));
check('Enter joins the marked row', luaRuns.pop(), 'MeowyClientPackHomeJoin(11)');
e = key(input, 65); input.onkeydown(e);
check('other keys are left alone', e.prevented, false);
input.onkeydown(key(input, 27));
check('Esc clears the search', input.value, '');
check('and shows the usual rows', visibleNames(box).length, 8);
check('and leaves the box', doc.activeElement, null);
check('nothing marked without the keyboard', box.querySelectorAll('.mh-sel').length, 0);

// "/" on the page focuses the search box, but not while typing elsewhere.
const keydown = listeners.find(l => l.type === 'keydown').fn;
e = key(doc.body, 191, '/'); keydown(e);
check('"/" focuses the search', doc.activeElement, input);
check('"/" isn\'t typed into it', e.prevented, true);
input.blur();
const other = new El('input');
e = key(other, 191, '/'); keydown(e);
check('"/" in another box is left alone', e.prevented, false);

// New data keeps what was typed; the same version isn't redrawn.
input.value = 'big';
input.oninput();
const before = box.querySelector('.mh-list').children.length;
win.MeowyHome(data, 1);
check('same version, nothing redrawn', box.querySelector('.mh-list').children.length, before);
const data2 = JSON.parse(JSON.stringify(data));
data2.sections[1].rows.push(row(4, 'Bigger RP', 'rp_x'));
win.MeowyHome(data2, 2);
check('new data, the search kept', visibleNames(box).join(','), 'Big RP,Bigger RP');
check('same box, same input', doc.getElementById('meowy-home').querySelector('.mh-search'), input);

// A hostile name stays text.
win.MeowyHome({ sections: [{ title: 'Favourites', show: 5, rows: [row(5, '<img src=x onerror=alert(1)>')] }] }, 3);
input.value = ''; input.oninput();
check('hostile name as text', box.querySelector('.mh-name').textContent, '<img src=x onerror=alert(1)>');
check('no element made from it', box.all().some(el => el.tagName === 'IMG'), false);

// null removes the card; the page being rebuilt brings it back.
win.MeowyHome(null, 4);
check('null removes the card', doc.getElementById('meowy-home'), null);
win.MeowyHome(data, 5);
check('back', !!doc.getElementById('meowy-home'), true);

// ---------------------------------------------------------------------------
// The Escape card.
({ win, page } = fresh());
const esc = {
	server: { name: 'Meowy Network', line1: 'rp_downtown_meowy · 1/31 players', line2: 'ping 48 ms · loss 0% · 45 s here', address: '138.201.62.52:27015' },
	settings: [
		{ id: 1, label: 'Master volume', min: 0, max: 1, step: 0.05, value: 0.2, percent: true },
		{ id: 3, label: 'Field of view', min: 75, max: 100, step: 1, value: 90, percent: false },
	],
};
win.MeowyEsc(esc, 1);
box = doc.getElementById('meowy-esc');
check('escape card added', box && box.parentNode === page, true);
check('server name as text', box.querySelector('.me-name').textContent.endsWith('Meowy Network'), true);
const buttons = box.querySelectorAll('.me-btn');
buttons[0].onclick();
check('Reconnect sends its fixed name', luaRuns.pop(), 'MeowyClientPackEscAction("reconnect")');
buttons[1].onclick();
check('Copy sends its fixed name', luaRuns.pop(), 'MeowyClientPackEscAction("copy")');
check('Copy says so', buttons[1].textContent, 'Copied');
const sliders = box.all().filter(el => el.tagName === 'INPUT');
check('two sliders', sliders.length, 2);
check('percent shown', box.all().filter(el => el.tagName === 'B')[0].textContent, '20%');
sliders[1].value = '82';
sliders[1].onchange();
check('slider sends its row and a number', luaRuns.pop(), 'MeowyClientPackEscSet(3,82)');
sliders[0].value = '0.5); RunConsoleCommand("quit"';
sliders[0].onchange();
check('junk in a slider becomes NaN, never code', luaRuns.pop(), 'MeowyClientPackEscSet(1,NaN)');
sliders[1].onmousedown();
const kids = box.children.length;
win.MeowyEsc({ server: null, settings: [] }, 2);
check('not redrawn while a slider is held', box.children.length, kids);
sliders[1].onmouseup();
win.MeowyEsc({ server: null, settings: [] }, 3);
check('redrawn once let go', box.children.length, 0);
win.MeowyEsc(null, 4);
check('null removes the escape card', doc.getElementById('meowy-esc'), null);

// ---------------------------------------------------------------------------
// The Meowy home screen (src/menu/shell.js): our home inside the stock page.
const shellSrc = fs.readFileSync(path.join(__dirname, '..', 'src', 'menu', 'shell.js'), 'utf8');
const browserSrc = fs.readFileSync(path.join(__dirname, '..', 'src', 'menu', 'browser.js'), 'utf8');
function stockPage() {
	fresh();
	const view = new El('div'); // ng-view
	doc.body.appendChild(view);
	const main = new El('div');
	main.className = 'page';
	main.setAttribute('ng-controller', 'ControllerMain');
	const options = new El('div'); options.className = 'options';
	const news = new El('div'); news.className = 'news';
	main.appendChild(options); main.appendChild(news);
	// fresh() put a main page in the body already; this one is in the view instead.
	doc.body.children.filter(c => c !== view).forEach(c => doc.body.removeChild(c));
	view.appendChild(main);
	doc.body.className = 'ng-scope';
	const posts = [];
	const win = { meowy: { post: (k, v) => posts.push([k, v]) } };
	const loc = { hash: '#/' };
	// As module.cpp's MenuShellJs sends them: Find servers, then the home.
	new Function('window', 'document', 'location', 'lua', 'setTimeout', 'clearTimeout', browserSrc + '\n' + shellSrc)(win, doc, loc, { Run: s => luaRuns.push(s) },
		fn => { timers.push(fn); return timers.length; }, () => {});
	return { win, posts, loc, view, main };
}
let S = stockPage();
const shellEl = () => doc.getElementById('meowy-shell');
check('shell: nothing until asked', shellEl(), null);
S.win.MeowyShell(true);
check('shell: put into the main screen', shellEl() && shellEl().parentNode === S.main, true);
check('shell: the stock list and news hidden by a rule on <body>', doc.body.className.split(' ').includes('meowy-shell'), true);
check('shell: the rule is there once', !!doc.getElementById('meowy-shell-style'), true);
check('shell: the rule hides the stock list and news', /meowy-shell .*> \.options/.test(doc.getElementById('meowy-shell-style').textContent), true);
check('shell: body keeps its own classes', doc.body.className.split(' ').includes('ng-scope'), true);
S.win.MeowyShell(true);
check('shell: sent again, still one', S.main.querySelectorAll('#meowy-shell').length, 1);
// Links: the page's own routes (no reload) or fixed posts.
const navs = shellEl().querySelectorAll('.ms-link').filter(a => a.textContent && !a.className.includes('ms-cont'));
const byText = t => navs.find(a => a.textContent === t);
byText('Find servers').onclick();
check('shell: Find servers opens the new browser', shellEl().className, 'ms-browsing');
check('shell: and asks for the list', JSON.stringify(S.posts.pop()), '["browse",""]');
// Find servers (src/menu/browser.js), in the home: the list in batches, the
// gamemodes, a gamemode's servers in columns; rows asked for by number.
const bview = () => shellEl().querySelector('.ms-browse');
const BL = S.win.MeowyBrowserLogic;
S.win.GMOD_VERSION_INT = 250107;
S.win.DoWeHaveMap = m => m !== 'rp_missing';
S.win.subscriptions = { Contains: id => id === '111' };
S.win.MeowyBrowse.reset(1, true);
S.win.MeowyBrowse.add(1, [
	{ i: 1, a: '1.1.1.1:27015', n: 'Small RP', d: 'DarkRP', m: 'rp_a', g: 'darkrp', c: 'rp', p: 5, x: 32, b: 0, ms: 40, pw: false, ws: '111', f: 'gb', v: 250107 },
	{ i: 2, a: '2.2.2.2:27015', n: '<b>Big</b> TTT', d: 'Trouble in Terrorist Town', m: 'ttt_b', g: 'terrortown', c: 'pvp', p: 30, x: 32, ms: 90, ws: '222', v: 250101 },
	{ i: 3, a: '3.3.3.3:27015', n: 'Full RP', d: 'DarkRP', m: 'rp_missing', g: 'darkrp', c: 'rp', p: 64, x: 64, ms: 20, pw: true, v: 250201, lp: 1700000000, fv: true },
	{ i: 'x', n: 'bad row' },
	{ i: 4, a: '4.4.4.4:27015', n: 'One slot', g: 'sandbox', p: 0, x: 1, ms: 5 },
], false, 0.8, 1);
S.win.MeowyBrowse.add(9, [{ i: 9, a: '9.9.9.9:1', n: 'Old list', g: 'x', p: 99, x: 100, ms: 1 }], false, 1, 1);
S.win.MeowyBrowse.flush();
const tileNames = () => bview().querySelectorAll('.mb-gm').map(t => t.querySelector('.mb-gmname')._text);
const sideText = cls => bview().querySelector(cls).textContent;
const btn = t => bview().querySelectorAll('.mb-btn').find(b => b.textContent === t);
const link = t => bview().querySelectorAll('.mb-link').find(b => b.textContent === t);
check('browser: gamemodes, most players first, every gamemode on top', tileNames().join('|'), 'Every gamemode|DarkRP|Trouble in Terrorist Town');
check('browser: counts (older lists and one-slot servers left out)', bview().querySelector('.mb-side').querySelectorAll('.mb-small')[0].textContent, '3 servers \u00b7 2 gamemodes');
check('browser: still listing', sideText('.mb-status'), 'Listing servers with players\u2026 0.8 s');
check('browser: a tile has its players, servers and category',
	bview().querySelectorAll('.mb-gm')[1].querySelector('.mb-gmstats').textContent + ' / ' + bview().querySelectorAll('.mb-gm')[1].querySelector('.mb-tag').textContent,
	'69 players on 2 servers / Roleplay');
check('browser: Install for a gamemode not subscribed to', bview().querySelectorAll('.mb-gm').map(t => !!t.querySelector('.mb-install')).join(','), 'false,false,true');
bview().querySelectorAll('.mb-gm')[2].querySelector('.mb-install').onclick({ stopPropagation() {} });
check('browser: Install asks for that Workshop item', JSON.stringify(S.posts.pop()), '["browse_install","222"]');
btn('Roleplay').onclick();
check('browser: a category, only it', tileNames().join('|'), 'DarkRP');
btn('Roleplay').onclick();
check('browser: again, hidden', tileNames().join('|'), 'Trouble in Terrorist Town');
btn('Roleplay').onclick();
check('browser: again, either', tileNames().join('|'), 'Every gamemode|DarkRP|Trouble in Terrorist Town');
btn('Name').onclick();
check('browser: gamemodes by name', tileNames().join('|'), 'Every gamemode|DarkRP|Trouble in Terrorist Town');
btn('Servers').onclick();
check('browser: by servers', tileNames()[1], 'DarkRP');
btn('Players').onclick();
const gmSearch = bview().querySelector('.mb-gmsearch');
gmSearch.value = 'TERROR';
gmSearch.oninput();
check('browser: a gamemode by name or title', tileNames().join('|'), 'Trouble in Terrorist Town');
gmSearch.value = '';
gmSearch.oninput();
// A gamemode's servers.
bview().querySelectorAll('.mb-gm')[1].onclick();
const rowNames = () => bview().querySelectorAll('.mb-srow').map(r => r.querySelector('.mb-c-name').children.filter(c => c.tagName === '#TEXT').map(c => c.textContent).join(''));
check('browser: its servers, best ranked first', rowNames().join('|'), 'Small RP|Full RP');
check('browser: the page says which gamemode', bview().querySelector('.mb-main').querySelector('.mb-title').textContent, 'DarkRP');
const full = bview().querySelectorAll('.mb-srow')[1];
check('browser: a missing map marked', full.className.split(' ').includes('nomap'), true);
check('browser: a favourite starred', full.querySelector('.mb-fav').className, 'mb-fav on');
check('browser: a password marked', !!full.querySelector('.mb-lock'), true);
check('browser: a newer version tagged', full.querySelector('.mb-tag').textContent, 'newer: 2025.02.01');
check('browser: last visit', /^last visit \d\d\.\d\d\.2023 \d\d:\d\d$/.test(full.querySelector('.mb-last').textContent), true);
check('browser: players and ping', full.querySelector('.mb-c-ply').textContent + ' ' + full.querySelector('.mb-c-ping').textContent, '64 / 64 20');
check('browser: a region flag', bview().querySelectorAll('.mb-srow')[0].querySelector('.mb-flag').src, 'asset://garrysmod/materials/flags16/gb.png');
const head = t => bview().querySelector('.mb-thead').children.find(h => h.textContent.indexOf(t) === 0);
head('Players').onclick();
check('browser: by players', rowNames().join('|'), 'Full RP|Small RP');
head('Players').onclick();
check('browser: clicked again, reversed', rowNames().join('|'), 'Small RP|Full RP');
head('Ranking').onclick();
bview().querySelectorAll('.mb-srow')[1].onclick();
check('browser: a click selects', bview().querySelectorAll('.mb-srow')[1].className.split(' ').includes('sel'), true);
check('browser: and joins nothing', S.posts.length === 0 || S.posts[S.posts.length - 1][0] !== 'browse_join', true);
bview().querySelectorAll('.mb-srow')[1].ondblclick();
check('browser: a double-click joins by number', JSON.stringify(S.posts.pop()), '["browse_join",3]');
let prevented = false;
bview().querySelectorAll('.mb-srow')[0].oncontextmenu({ preventDefault() { prevented = true; } });
check('browser: right-click copies the address', JSON.stringify(S.posts.pop()) + prevented, '["browse_copy",1]true');
bview().querySelectorAll('.mb-srow')[0].querySelector('.mb-fav').onclick({ stopPropagation() {} });
check('browser: the star', JSON.stringify(S.posts.pop()), '["browse_fav",1]');
check('browser: starred at once', bview().querySelectorAll('.mb-srow')[0].querySelector('.mb-fav').className, 'mb-fav on');
const search = bview().querySelector('.mb-search');
search.value = 'full';
search.oninput();
check('browser: search names', rowNames().join('|'), 'Full RP');
search.value = '1.1.1';
search.oninput();
check('browser: search an address', rowNames().join('|'), 'Small RP');
search.value = 'RP_A';
search.oninput();
check('browser: search maps, any case', rowNames().join('|'), 'Small RP');
search.onkeydown(key(search, 27));
check('browser: Esc clears the search', search.value + rowNames().length, '2');
search.onkeydown(key(search, 40));
search.onkeydown(key(search, 40));
search.onkeydown(key(search, 13));
check('browser: Down, Down, Enter joins the second', JSON.stringify(S.posts.pop()), '["browse_join",3]');
check('browser: names are text', bview().all().some(e => e.tagName === 'B'), false);
// Every gamemode, and a long list: only the rows on screen are drawn.
search.onkeydown(key(search, 27));
check('browser: Esc again, back to the gamemodes', tileNames()[0], 'Every gamemode');
const many = [];
for (let i = 0; i < 600; i++) many.push({ i: 100 + i, a: '5.5.' + Math.floor(i / 250) + '.' + (i % 250) + ':27015', n: 'Box ' + i, g: 'sandbox', p: 1, x: 10, ms: 10 + i });
S.win.MeowyBrowse.add(1, many, false, 3, 2);
S.win.MeowyBrowse.flush();
check('browser: the empty ones being listed', sideText('.mb-status'), 'Listing empty servers\u2026 3 s');
bview().querySelectorAll('.mb-gm')[0].onclick();
check('browser: every gamemode, every server', bview().querySelector('.mb-main').querySelector('.mb-small').textContent, '699 players on 603 servers');
const body = bview().querySelector('.mb-tbody');
check('browser: a long list, only a screenful drawn', bview().querySelectorAll('.mb-srow').length <= 60, true);
body.clientHeight = 260;
body.scrollTop = 26 * 300;
body.onscroll();
const firstDrawn = Math.floor(26 * 300 / 26) - 20;
check('browser: scrolled, the rows there drawn', body.children[0].style.height, (firstDrawn * 26) + 'px');
check('browser: the rest kept as space', body.children[2].style.height, ((603 - (300 + 10 + 20)) * 26) + 'px');
S.win.MeowyBrowse.add(1, [{ i: 900, a: '6.6.6.6:27015', n: 'Late', g: 'sandbox', p: 50, x: 60, ms: 5 }], false, 4, 2);
S.win.MeowyBrowse.flush();
check('browser: a server arriving doesn\'t move the list', body.scrollTop, 26 * 300);
// Stop, complete, Back; reopening soon keeps the list.
btn('Stop').onclick();
check('browser: Stop', JSON.stringify(S.posts.pop()) + ' ' + sideText('.mb-status'), '["browse_stop",""] Stopped after 4 s');
S.win.MeowyBrowse.add(1, [], true, 19.9, 2);
S.win.MeowyBrowse.flush();
check('browser: complete', sideText('.mb-status'), 'Complete in 19.9 s');
check('browser: Refresh then', !!btn('Refresh'), true);
link('Back').onclick();
check('browser: Back closes it', shellEl().className, '');
check('browser: a finished list isn\'t stopped again', S.posts.length === 0 || S.posts[S.posts.length - 1][0] !== 'browse_stop', true);
S.win.MeowyBrowse.open();
check('browser: reopened soon after, the list is kept', S.posts.length === 0 || S.posts[S.posts.length - 1][0] !== 'browse', true);
link("Garry's Mod's list").onclick();
check("browser: Garry's Mod's list is still there", S.loc.hash + '|' + shellEl().className, '#/servers/|');
S.loc.hash = '#/';
S.win.MeowyBrowse.reset(2, false);
S.win.MeowyBrowse.open();
check('browser: without Steam\'s list, says where to go', sideText('.mb-status'), "Steam's server list isn't available here: use Garry's Mod's list.");
link('Back').onclick();

// The rules on their own.
check('rules: rank as Garry\'s Mod ranks', BL.rank({ ping: 40, players: 5, pass: false, verC: 0, anon: false }), 30);
check('rules: empty, password, anonymous', BL.rank({ ping: 10, players: 0, pass: true, verC: 0, anon: true }), 1385);
check('rules: bars', [10, 60, 150, 250, 900].map(BL.rankBars).join(''), '54321');
check('rules: version text', BL.formatVersion(250107), '2025.01.07');
check('rules: flags', BL.flag('EU') + ',' + BL.flag('u-s') + ',' + BL.flag('de'), 'europeanunion,,de');
const sv = BL.server({ i: 1, a: '1.2.3.4:1', n: 'X', d: '[L] Sandbox', m: 'gm_flat', p: 3, x: 8, ms: 50, f: 'us', v: 250101 }, 250107, () => true);
check('rules: a listen server', sv.listen + ' ' + sv.desc + ' ' + sv.gm, 'true Sandbox [L] Sandbox');
check('rules: outdated', sv.verC, -1);
const F = (f) => BL.serverShown(sv, f);
check('rules: filters', [F({}), F({ maxPing: 40 }), F({ notFull: true }), F({ minPlayers: 4 }), F({ maxPlayers: 2 }), F({ hideOutdated: true }), F({ hasPlayers: true })].join(','),
	'true,false,true,false,false,false,true');
check('rules: flags avoid and prefer', [F({ flags: { us: 'avoid' } }), F({ flags: { de: 'prefer' } }), F({ flags: { us: 'prefer' } })].join(','), 'false,false,true');
check('rules: cycle', [BL.cycle(undefined, 'only', 'hide'), BL.cycle('only', 'only', 'hide'), BL.cycle('hide', 'only', 'hide')].join(','), 'only,hide,');
const g = BL.gamemode('darkrp');
BL.addServer(g, BL.server({ i: 1, n: 'a', d: 'darkrp', g: 'darkrp', p: 50, x: 60 }, 0));
BL.addServer(g, BL.server({ i: 2, n: 'b', d: 'DarkRP', g: 'darkrp', p: 1, x: 60 }, 0));
check('rules: a title other than the internal name wins', g.title, 'DarkRP');
check('rules: roleplay guessed from the title', g.cat, 'rp');
S.loc.hash = '#/';
byText('Start new game').onclick();
check('shell: Start new game', S.loc.hash, '#/newgame/');
byText('Options').onclick();
check('shell: Options posts', JSON.stringify(S.posts.pop()), '["options",""]');
byText('Quit').onkeydown({ keyCode: 13, preventDefault() {} });
check('shell: Enter on a link', JSON.stringify(S.posts.pop()), '["quit",""]');
shellEl().querySelector('.ms-foot').onclick();
check('shell: the escape hatch posts', JSON.stringify(S.posts.pop()), '["stock",""]');
// Going to a screen and back: the stock main screen is redrawn, ours goes straight back in.
S.view.removeChild(S.main);
const servers = new El('div'); servers.className = 'page'; servers.setAttribute('ng-controller', 'ControllerServers');
S.view.appendChild(servers);
S.win.MeowyShell(true);
check('shell: on another screen, nothing put in', shellEl(), null);
S.view.removeChild(servers);
const main2 = new El('div'); main2.className = 'page'; main2.setAttribute('ng-controller', 'ControllerMain');
S.view.appendChild(main2);
S.win.MeowyShell(true);
check('shell: the redrawn main screen gets ours back', shellEl() && shellEl().parentNode === main2, true);
// Data: the lists and Continue, as the stock-page cards get them.
S.win.MeowyHomeData = data;
S.win.MeowyShellLists();
check('shell: lists drawn', shellEl().querySelectorAll('.ms-name').length >= 8, true);
shellEl().querySelectorAll('.ms-row')[1].onclick();
check('shell: a row posts its number', JSON.stringify(S.posts.pop()), '["join",2]');
const ssearch = shellEl().querySelector('.ms-search');
ssearch.value = 'server 1';
ssearch.oninput();
check('shell: search', shellEl().querySelectorAll('.ms-name').map(e => e.textContent).join(','), 'Server 1,Server 10,Server 11');
ssearch.focus();
ssearch.onkeydown(key(ssearch, 40));
ssearch.onkeydown(key(ssearch, 13));
check('shell: arrows and Enter', JSON.stringify(S.posts.pop()), '["join",20]');
ssearch.onkeydown(key(ssearch, 27));
check('shell: Esc clears', ssearch.value, '');
S.win.MeowyShellContinue();
check('shell: no Continue without a server', shellEl().querySelector('.ms-cont').className.includes(' on'), false);
S.win.MeowyContinue = ['Continue', '<b>Meowy</b>', ' · 2 h ago'];
S.win.MeowyShellContinue();
check('shell: Continue shown', shellEl().querySelector('.ms-cont').className.includes(' on'), true);
check('shell: its server as text', shellEl().querySelector('.ms-cserver').textContent, '<b>Meowy</b>');
check('shell: no element made from it', shellEl().all().some(e => e.tagName === 'B'), false);
shellEl().querySelector('.ms-cont').onclick();
check('shell: Continue posts', JSON.stringify(S.posts.pop()), '["continue",""]');
// News: the stock page's own feed (rootScope.NewsList), opened the stock way.
const newsBox = () => shellEl().querySelector('.ms-news');
check('shell: no news before the feed', newsBox().querySelectorAll('.ms-row').length, 0);
const opened = [];
S.win.scope = { OpenInSteam: url => opened.push(url) };
S.win.rootScope = { NewsList: [
	{ Title: '<img src=x onerror=alert(1)>November update', Url: 'https://gmod.facepunch.com/news/nov', Date: new Date(Date.now() - 2 * 86400000).toISOString() },
	{ Title: 'Older post', Url: 'https://gmod.facepunch.com/news/old', Date: '2026-01-02T00:00:00Z' },
	{ Title: 'Third', Url: 'https://gmod.facepunch.com/news/3', Date: 'not a date' },
	{ Title: 'Fourth, not shown', Url: 'https://gmod.facepunch.com/news/4', Date: '' },
	{ Title: 42, Url: 'https://x' },
] };
S.win.MeowyShell(true);   // the next send picks the feed up
check('shell: news shown, at most three', newsBox().querySelectorAll('.ms-name').map(e => e.textContent).join('|'),
	'<img src=x onerror=alert(1)>November update|Older post|Third');
check('shell: news titles are text', newsBox().all().some(e => e.tagName === 'IMG'), false);
check('shell: news age', newsBox().querySelectorAll('.ms-stats').map(e => e.textContent).join('|'), '2 days ago|Jan 02 2026|');
newsBox().querySelectorAll('.ms-row')[0].onclick();
check('shell: a news item opens the stock way', opened.pop(), 'https://gmod.facepunch.com/news/nov');
const newsRows = newsBox().querySelectorAll('.ms-row')[0];
S.win.MeowyShell(true);
check('shell: news not redrawn when unchanged', newsBox().querySelectorAll('.ms-row')[0], newsRows);
// Maps: the page's map list, searched with the servers; picked the New game way.
S.win.gScope = { MapList: [
	{ category: 'Favourites', maps: ['gm_meowtest'] },
	{ category: 'Sandbox', maps: ['gm_meowtest', 'gm_flatgrass'] },
	{ category: 'DarkRP', maps: ['rp_downtown_meowy', 'rp_meowtest_city'] },
] };
const digests = [];
S.win.UpdateDigest = (sc, t) => digests.push(sc);
ssearch.value = 'meowtest';
ssearch.oninput();
check('shell: maps found by name, once each', shellEl().querySelectorAll('.ms-name').map(e => e.textContent).join(','), 'gm_meowtest,rp_meowtest_city');
check('shell: a Maps section', shellEl().querySelectorAll('.ms-title').map(e => e.textContent).join(','), 'Maps');
check('shell: news hidden while searching', newsBox().querySelectorAll('.ms-row').length, 0);
ssearch.onkeydown(key(ssearch, 40));
ssearch.onkeydown(key(ssearch, 13));
check('shell: Enter on a map opens New game', S.loc.hash, '#/newgame/');
check('shell: with that map chosen', S.win.rootScope.Map + '/' + S.win.rootScope.LastCategory, 'rp_meowtest_city/DarkRP');
check('shell: the page told', digests.pop(), S.win.rootScope);
// Addons (menu_page.lua's list) and settings, in the same search.
S.win.MeowyAddons = [{ t: 'Wiremod', w: '160250458', m: true }, { t: '<img src=x>Wire Extras', w: '2', m: false }, { t: 7 }];
ssearch.value = 'wire';
ssearch.oninput();
check('shell: addons found', shellEl().querySelectorAll('.ms-name').map(e => e.textContent).join('|'), 'Wiremod|<img src=x>Wire Extras');
check('shell: an Addons section', shellEl().querySelectorAll('.ms-title').map(e => e.textContent).join(','), 'Addons');
check('shell: addon titles are text', shellEl().all().some(e => e.tagName === 'IMG'), false);
check('shell: a disabled addon dimmed', shellEl().querySelectorAll('.ms-row')[1].className.includes('ms-dim'), true);
shellEl().querySelectorAll('.ms-row')[0].onclick();
check('shell: an addon opens Addons', S.loc.hash, '#/addons/');
ssearch.value = 'volume';
ssearch.oninput();
check('shell: a setting found by what it covers', shellEl().querySelectorAll('.ms-name').map(e => e.textContent).join('|'), 'Options');
ssearch.onkeydown(key(ssearch, 13));
check('shell: Enter on a setting opens it', JSON.stringify(S.posts.pop()), '["options",""]');
ssearch.value = 'workshop';
ssearch.oninput();
shellEl().querySelectorAll('.ms-row')[0].onclick();
check('shell: Meowy settings by a pack word', JSON.stringify(S.posts.pop()), '["settings",""]');
ssearch.value = 'zzz';
ssearch.oninput();
check('shell: nothing found', shellEl().querySelector('.ms-note').textContent, 'Nothing matches');
ssearch.onkeydown(key(ssearch, 27));
check('shell: news back after the search', newsBox().querySelectorAll('.ms-row').length, 3);
// Off (in game, or switched off): taken out, the stock screen back.
S.win.MeowyShell(false);
check('shell: off takes it out', shellEl(), null);
check('shell: and the rule off', doc.body.className.split(' ').includes('meowy-shell'), false);
check('shell: body keeps its own classes after', doc.body.className, 'ng-scope');
check('shell: never lua.Run', luaRuns.length, 0);

// Find servers' Friends tab: after Local Network, home on or off, through the page's own SwitchType.
function serversPage() {
	const ul = new El('ul');
	const tab = type => { const li = new El('li'); const a = new El('a'); a.setAttribute('ng-click', `SwitchType( '${type}' )`); li.appendChild(a); ul.appendChild(li); return li; };
	tab('internet'); tab('favorite'); tab('history');
	const lanLi = tab('lan');
	const refresh = new El('li'); ul.appendChild(refresh);
	S.view.appendChild(ul);
	return { ul, lanLi, refresh };
}
const SP = serversPage();
S.win.MeowyFriendsTab();
const ftab = () => doc.getElementById('meowy-friends-tab');
check('friends tab: added', ftab() && ftab().textContent, 'Friends');
check('friends tab: right after Local Network', SP.ul.children.indexOf(ftab().parentNode), SP.ul.children.indexOf(SP.lanLi) + 1);
S.win.MeowyFriendsTab();
check('friends tab: once', doc.body.all().filter(e => e.id === 'meowy-friends-tab').length, 1);
const switched = [];
let applied = 0;
S.win.RootScope = { ServerType: 'internet' };
S.win.Scope = { SwitchType: t => { switched.push(t); S.win.RootScope.ServerType = t; }, $apply: fn => { applied++; fn(); } };
check('friends tab: not active on another list', ftab().className, '');
ftab().onclick();
check('friends tab: the page switches to the friends list', switched.join(','), 'friends');
check('friends tab: inside a digest', applied, 1);
S.win.MeowyFriendsTab();
check('friends tab: shown as active', ftab().className, 'active');
S.win.RootScope.ServerType = 'favorite';
S.win.MeowyFriendsTab();
check('friends tab: not active once another list is chosen', ftab().className, '');
check('friends tab: never lua.Run', luaRuns.length, 0);

console.log(`${count} checks, ${failures} failed`);
process.exit(failures ? 1 : 0);
