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
	new Function('window', 'document', 'location', 'lua', shellSrc)(win, doc, loc, { Run: s => luaRuns.push(s) });
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
check('shell: Find servers is the page\'s own route', S.loc.hash, '#/servers/');
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

console.log(`${count} checks, ${failures} failed`);
process.exit(failures ? 1 : 0);
