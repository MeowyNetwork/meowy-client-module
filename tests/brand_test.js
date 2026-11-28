// The pack names its site and its own server once per language (src/brand.h,
// launcher/core/src/brand.rs, installer/Installer.cs). This checks the three
// agree, and that no other source file spells either out: a build for another
// operator changes three lines, not thirty.
//
//   node tests/brand_test.js
const fs = require('fs');
const path = require('path');
const root = path.join(__dirname, '..');
let count = 0, failures = 0;
function check(name, got, want) {
	count++;
	if (got !== want) { failures++; console.log('FAIL ' + name + ': got ' + JSON.stringify(got) + ', want ' + JSON.stringify(want)); }
}
const read = p => fs.readFileSync(path.join(root, p), 'utf8');
const first = (text, re) => { const m = text.match(re); return m ? m[1] : null; };

const h = read('src/brand.h');
const cppHost = first(h, /#define MEOWY_BRAND_HOST "([^"]+)"/);
const cppIp = first(h, /#define MEOWY_BRAND_SERVER_IP "([^"]+)"/);
const cppPort = first(h, /#define MEOWY_BRAND_SERVER_PORT (\d+)/);
const rs = read('launcher/core/src/brand.rs');
const rsHost = first(rs, /pub const HOST: &str = "([^"]+)";/);
const rsServer = first(rs, /pub const SERVERS: \[&str; 1\] = \["([^"]+)"\];/);
const cs = read('installer/Installer.cs');
const csServer = first(cs, /public const string ServerAddress = "([^"]+)";/);

check('brand.h has a host', typeof cppHost === 'string' && /^[a-z0-9.-]+$/.test(cppHost), true);
check('brand.h has a server', /^\d+\.\d+\.\d+\.\d+$/.test(cppIp || '') && /^\d+$/.test(cppPort || ''), true);
check('the launcher names the same host', rsHost, cppHost);
check('the launcher names the same server', rsServer, cppIp + ':' + cppPort);
check('the installer names the same server', csServer, cppIp + ':' + cppPort);

// No other source file spells them out. Comments may (they explain), tests may
// (they need examples), and the release tools talk to the published site.
const OWN = new Set(['src/brand.h', 'launcher/core/src/brand.rs', 'installer/Installer.cs']);
const strays = [];
function walk(dir) {
	for (const e of fs.readdirSync(path.join(root, dir), { withFileTypes: true })) {
		const rel = dir + '/' + e.name;
		if (e.isDirectory()) {
			if (e.name === 'target' || e.name === 'node_modules' || e.name === 'gen') continue;
			walk(rel);
		} else if (/\.(cpp|h|rs|cs|lua|js|html)$/.test(e.name) && !OWN.has(rel)) {
			const lines = read(rel).split(/\r?\n/);
			let inTests = false;
			lines.forEach((line, i) => {
				if (/#\[cfg\(test\)\]/.test(line)) inTests = true;   // Rust: the unit tests below use real examples
				if (inTests) return;
				const code = line.replace(/^\s*(\/\/|--|\*|\/\*|#).*$/, '').replace(/\s(\/\/|--)\s.*$/, '');
				if (code.indexOf(cppHost) >= 0 || code.indexOf(cppIp) >= 0) strays.push(rel + ':' + (i + 1));
			});
		}
	}
}
for (const dir of ['src', 'launcher/core/src', 'launcher/app/src', 'launcher/ui', 'installer']) {
	if (fs.existsSync(path.join(root, dir))) walk(dir);
}
check('nothing else spells out the host or the server', strays.join(', '), '');

console.log(count + ' checks, ' + failures + ' failed');
process.exit(failures ? 1 : 0);
