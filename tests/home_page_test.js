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
		addEventListener: (type, fn) => listeners.push({ type, fn }),
	};
	const win = {};
	const run = new Function('window', 'document', 'lua', 'setTimeout', block('HOME_JS') + '\n' + block('ESC_JS'));
	run(win, doc, { Run: s => luaRuns.push(s) }, () => {});
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

console.log(`${count} checks, ${failures} failed`);
process.exit(failures ? 1 : 0);
