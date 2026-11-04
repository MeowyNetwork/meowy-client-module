/**
 * The observer's supervisor: asks Meowy Network for the next suspect, hands it
 * to the game (the module's menu Lua reads $MEOWY_OBSERVER_DIR/job.json),
 * waits for done.json and sends the counts back.
 *
 *   MEOWY_OBSERVER_DIR          shared with the game (outside garrysmod)
 *   MEOWY_OBSERVER_API          default https://meowynetwork.com
 *   SERVER_REP_OBSERVER_TOKEN   the same token the backend has
 *   MEOWY_OBSERVER_MANAGE_GAME  on Linux (the VM) the supervisor starts GMod for
 *                               a round of visits and quits it afterwards; set 0
 *                               to start the game yourself (as on Windows)
 *   MEOWY_OBSERVER_CONFIRM      set 0 to stop clicking through Steam's "another
 *                               session is playing" prompt when launching
 *
 * One visit at a time. A visit the game doesn't finish in VISIT_TIMEOUT is
 * dropped (the backend gives it up too).
 */

import fs from 'node:fs/promises';
import path from 'node:path';
import { execFile, spawn } from 'node:child_process';
import { pathToFileURL } from 'node:url';
import dgram from 'node:dgram';

const DIR = process.env.MEOWY_OBSERVER_DIR || path.join(process.env.HOME || '.', '.meowy-observer');
const API = (process.env.MEOWY_OBSERVER_API || 'https://meowynetwork.com').replace(/\/+$/, '');
const TOKEN = process.env.SERVER_REP_OBSERVER_TOKEN || '';
const MANAGE_GAME = process.platform === 'linux' && process.env.MEOWY_OBSERVER_MANAGE_GAME !== '0';
const CONFIRM = MANAGE_GAME && process.env.MEOWY_OBSERVER_CONFIRM !== '0';
const POLL_MS = 60 * 1000;
const VISIT_TIMEOUT_MS = 55 * 60 * 1000; // a first visit can download GBs of Workshop content
const LAUNCH_TIMEOUT_MS = 3 * 60 * 1000;
// -condebug: the game appends its console to garrysmod/console.log, which is
// how a visit learns the moment it was kicked, and why.
// +mat_picmip 2: lowest textures. Nothing here looks at them, and on big
// servers they're most of the game's memory (6.6 GB seen on one).
// +cl_downloadfilter mapsonly: of a server's own downloads (models, materials,
// sounds) take only the map, which joining needs. Lua files always come.
const GAME_ARGS = [
  '-applaunch', '4000', '-condebug', '-windowed', '-w', '800', '-h', '600', '-nosound', '-nojoy',
  '+fps_max', '15', '+mat_queue_mode', '0', '+mat_picmip', '2', '+cl_downloadfilter', 'mapsonly',
];
const CONSOLE_LOG =
  process.env.MEOWY_OBSERVER_CONSOLE_LOG ||
  path.join(process.env.HOME || '.', '.steam/steam/steamapps/common/GarrysMod/garrysmod/console.log');

// The observer's module is in the game's folders only while the game starts:
// the loader goes into lua/bin and its require line into menu.lua just before a
// launch, and both are removed the moment the module has loaded (a loaded
// library keeps working on Linux once its file is gone). Any server's client
// Lua can list and read those folders; by the time any server's Lua runs,
// there's nothing of the observer in them. (Who the observer is, is public:
// observer/OBSERVER.md.)
const GARRYSMOD = path.dirname(CONSOLE_LOG);
const LUA_BIN = path.join(GARRYSMOD, 'lua/bin');
const MENU_LUA = path.join(GARRYSMOD, 'lua/includes/menu.lua');
const LOADER_SRC = process.env.MEOWY_OBSERVER_LOADERS || '/opt/observer';
const LOADERS = ['gmsv_ldr_linux64.dll', 'gmsv_ldr_linux.dll'];
const REQUIRE_LINE = 'pcall(require, "ldr")';

export async function emptyData() {
  const data = path.join(GARRYSMOD, 'data');
  for (const e of await fs.readdir(data).catch(() => [])) await fs.rm(path.join(data, e), { recursive: true, force: true });
}

export async function armLoader() {
  await fs.mkdir(LUA_BIN, { recursive: true });
  for (const f of LOADERS) await fs.copyFile(path.join(LOADER_SRC, f), path.join(LUA_BIN, f)).catch(() => {});
  const menu = await fs.readFile(MENU_LUA, 'utf8').catch(() => null);
  if (menu !== null && !menu.split('\n').some((l) => l.trim() === REQUIRE_LINE)) {
    await fs.writeFile(MENU_LUA, `${menu.replace(/\n*$/, '\n')}${REQUIRE_LINE}\n`);
  }
}

export async function disarmLoader() {
  for (const f of LOADERS) await fs.rm(path.join(LUA_BIN, f), { force: true });
  const menu = await fs.readFile(MENU_LUA, 'utf8').catch(() => null);
  if (menu !== null && menu.includes(REQUIRE_LINE)) {
    await fs.writeFile(MENU_LUA, menu.split('\n').filter((l) => l.trim() !== REQUIRE_LINE).join('\n').replace(/\n*$/, '\n'));
  }
}

// Wait for this launch's "module loaded" line in observer.log, then take the
// loader and the require line away again.
async function disarmOnceLoaded(logFrom, ms = 120000) {
  const logPath = path.join(DIR, 'observer.log');
  const until = Date.now() + ms;
  while (Date.now() < until) {
    // Bytes, not characters: logFrom is a byte size, and the log holds
    // non-ASCII text (kick reasons in other languages).
    const buf = await fs.readFile(logPath).catch(() => Buffer.alloc(0));
    if (buf.length > logFrom && buf.subarray(logFrom).toString('utf8').includes('module loaded')) {
      await disarmLoader();
      log('loader_removed');
      return true;
    }
    await sleep(1000);
  }
  await disarmLoader();
  log('loader_removed', { note: "the module didn't report loading" });
  return false;
}
// Lines that end a join or a visit: the server's kick reason, or a failed connect.
// (A refusal at connect time - S2C_CONNREJECT, e.g. a VPN block - reads "Connection rejected...".)
const DISCONNECT =
  /^(Disconnect: .+|Disconnected: .+|Connection rejected.*|.*S2C_CONNREJECT.*|Connection failed.*|Server is full.*|Couldn't connect to server.*|Connection to server timed out.*|.*Rejected.*connection.*|Bad challenge.*|Server uses different class tables.*)$/gim;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (msg, extra = {}) => console.log(JSON.stringify({ ts: new Date().toISOString(), msg, ...extra }));
const run = (cmd, args) =>
  new Promise((resolve) => execFile(cmd, args, { timeout: 10000 }, (err, stdout) => resolve(err ? null : String(stdout))));

async function api(method, route, body) {
  const res = await fetch(`${API}/api/pack/observer${route}`, {
    method,
    headers: { Authorization: `Bearer ${TOKEN}`, ...(body ? { 'Content-Type': 'application/json' } : {}) },
    body: body ? JSON.stringify(body) : undefined,
    signal: AbortSignal.timeout(15000),
  });
  if (!res.ok) throw new Error(`${method} ${route}: HTTP ${res.status}`);
  return res.json();
}

// --- the game (Linux VM only) ------------------------------------------------

// The 32-bit game is hl2_linux; the x86-64 branch (needed: 32-bit runs out of
// address space on big servers) is bin/linux64/gmod.
const GAME_PROCESS = 'hl2_linux|gmod';
// The game's process, once found: checked with signal 0, which (unlike pgrep
// reading /proc) never stalls when the game is deep in memory pressure.
let gamePid = 0;
const alive = (pid) => {
  try {
    process.kill(pid, 0);
    return true;
  } catch (err) {
    return err.code === 'EPERM';
  }
};
async function gameRunning() {
  if (gamePid && alive(gamePid)) return true;
  gamePid = 0;
  const out = await run('pgrep', ['-x', GAME_PROCESS]);
  if (out === null) return false;
  gamePid = Number(out.trim().split(/\s+/)[0]) || 0;
  return gamePid > 0;
}
// Closed for sure: not running in three checks a few seconds apart.
async function gameGone() {
  for (let i = 0; i < 3; i++) {
    if (await gameRunning()) return false;
    if (i < 2) await sleep(3000);
  }
  return true;
}

// Steam's main window: the largest window titled exactly "Steam". (Not
// --onlyvisible: on the VM's display xdotool counts none of Steam's windows as
// visible.)
// The tray icon is titled "Steam" too (48x48), so anything that small is skipped.
// The prompt has focus when it's up, so the focused window is tried first.
async function geometry(id) {
  const g = await run('xdotool', ['getwindowgeometry', '--shell', id]);
  if (!g) return null;
  const v = Object.fromEntries(g.trim().split('\n').map((l) => l.split('=')));
  const w = { id, x: +v.X, y: +v.Y, width: +v.WIDTH, height: +v.HEIGHT };
  return w.width >= 400 && w.height >= 300 ? w : null;
}

async function steamWindow() {
  const active = (await run('xdotool', ['getactivewindow']))?.trim();
  if (active && (await run('xdotool', ['getwindowname', active]))?.trim() === 'Steam') {
    const w = await geometry(active);
    if (w) return w;
  }
  let best = null;
  for (const id of ((await run('xdotool', ['search', '--name', '^Steam$'])) || '').split(/\s+/).filter(Boolean)) {
    const w = await geometry(id);
    if (w && (!best || w.width * w.height > best.width * best.height)) best = w;
  }
  return best;
}

// One screen pixel as [r, g, b] (ImageMagick's import), or null.
async function pixel(x, y) {
  const out = await run('import', ['-window', 'root', '-crop', `1x1+${x}+${y}`, 'txt:-']);
  // "srgb(46,124,222)" (8-bit) or "srgb(18%,49%,87%)"; the hex beside it can be 16-bit.
  const m = out && out.match(/srgb\(([\d.]+)(%?),([\d.]+)%?,([\d.]+)%?\)/i);
  if (!m) return null;
  const scale = m[2] === '%' ? 2.55 : 1;
  return [m[1], m[3], m[4]].map((v) => Math.round(Number(v) * scale));
}

// "You are logged in on another computer already playing ..." (the account
// idling elsewhere, e.g. an hour booster) is drawn inside Steam's main window,
// centred, with Continue just right of and below the centre. Enter doesn't
// answer it; a click does - but only when it's really there: Continue's blue
// under the click, and the dialog's dark grey to the left of the buttons.
// Clicking blind lands on whatever Steam shows (once, a store page).
async function promptAt() {
  const w = await steamWindow();
  // No usable window: Steam at its default size on the 1600x900 display.
  const cx = w ? w.x + w.width / 2 : 749;
  const cy = w ? w.y + w.height / 2 : 454;
  const x = Math.round(cx + 81);
  const y = Math.round(cy + 63);
  const button = await pixel(x, y);
  const dialog = await pixel(Math.round(cx - 250), y);
  const blue = button && button[2] >= 180 && button[2] - button[0] >= 100 && button[1] >= 90;
  const dark = dialog && Math.max(...dialog) <= 80;
  return { x, y, shown: Boolean(blue && dark), button, dialog };
}

const LAUNCHES = 3; // Steam sometimes ignores a launch just after it starts up
const RELAUNCH_MS = 45 * 1000;

function launch() {
  const steam = spawn('steam', GAME_ARGS, { detached: true, stdio: 'ignore' });
  steam.on('error', (err) => log('error', { err: `steam: ${err.message}` }));
  steam.unref();
}

// Start GMod and wait for it: click through Steam's prompt when it's shown, and
// ask again when nothing at all happened.
async function startGame() {
  if (await gameRunning()) return true;
  log('game_start');
  const logFrom = (await fs.stat(path.join(DIR, 'observer.log')).catch(() => ({ size: 0 }))).size;
  await armLoader();
  // In parallel with the launch below: gone again as soon as the module is in.
  disarmOnceLoaded(logFrom).catch((err) => log('error', { err: `loader: ${err.message}` }));
  launch();
  const started = Date.now();
  let clicks = 0;
  let launches = 1;
  let lastLaunch = started;
  let lastClick = 0;
  while (Date.now() - started < LAUNCH_TIMEOUT_MS) {
    await sleep(3000);
    if (await gameRunning()) {
      log('game_running', { seconds: Math.round((Date.now() - started) / 1000), clicks, launches });
      return true;
    }
    const p = CONFIRM ? await promptAt() : { shown: false };
    if (p.shown && clicks < 4 && Date.now() - lastClick > 8000) {
      clicks++;
      lastClick = Date.now();
      log('confirm_click', { x: p.x, y: p.y });
      await run('xdotool', ['mousemove', String(p.x), String(p.y), 'click', '1']);
    } else if (!p.shown && launches < LAUNCHES && Date.now() - lastLaunch > RELAUNCH_MS) {
      launches++;
      lastLaunch = Date.now();
      log('game_start_again', { launches, button: p.button, dialog: p.dialog });
      launch();
    }
  }
  log('game_start_failed', { clicks, launches });
  return false;
}

async function quitGame() {
  if (!(await gameRunning())) return;
  await run('pkill', ['-TERM', '-x', GAME_PROCESS]);
  for (let i = 0; i < 15 && (await gameRunning()); i++) await sleep(2000);
  if (await gameRunning()) await run('pkill', ['-KILL', '-x', GAME_PROCESS]);
  await disarmLoader();
  log('game_quit');
}

// --- visits ------------------------------------------------------------------

// The console log from a byte offset on (the offset goes back to 0 if the file
// was replaced by something shorter).
async function consoleSize() {
  try {
    return (await fs.stat(CONSOLE_LOG)).size;
  } catch {
    return 0;
  }
}

async function consoleSince(offset) {
  let fh;
  try {
    fh = await fs.open(CONSOLE_LOG, 'r');
    const { size } = await fh.stat();
    const from = size < offset ? 0 : offset;
    const length = Math.min(size - from, 1 << 20);
    const buf = Buffer.alloc(length);
    if (length > 0) await fh.read(buf, 0, length, from);
    return { text: buf.toString('utf8'), next: from + length };
  } catch {
    return { text: '', next: offset };
  } finally {
    await fh?.close();
  }
}

// --- status-only visits ------------------------------------------------------
// The game ran `status` every 30 s; its tables are in the console log. Per
// account (the ids never leave this function): in how many runs, and its ping
// range. Same thresholds as the backend's reports.
const FLAT_PING_MAX = 15;
const FLAT_PING_DRIFT = 1;
const PRESENT_SHARE = 0.8;

export function statusRuns(text) {
  const runs = [];
  let run = null;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (/^#\s*userid\s+name/i.test(line)) {
      run = { bots: 0, rows: new Map() };
      runs.push(run);
      continue;
    }
    if (!run) continue;
    const m = line.match(/^#\s*\d+\s+".*"\s+(\S+)\s*(.*?)\s*$/);
    if (!m) {
      if (line && !line.startsWith('#')) run = null; // the table ended
      continue;
    }
    if (m[1] === 'BOT') {
      run.bots++;
      continue;
    }
    // "connected ping loss state [adr]"; a client still connecting lacks the first three.
    const p = m[2].match(/^(\S+)\s+(\d+)\s+(\d+)\s+([a-z]+)/i);
    run.rows.set(m[1], p ? Number(p[2]) : null);
  }
  return runs.filter((r) => r.rows.size > 0 || r.bots > 0);
}

export function statusResult(runs) {
  const seen = new Map(); // id -> { n, min, max }
  for (const r of runs) {
    for (const [id, ping] of r.rows) {
      const s = seen.get(id) || { n: 0, min: Infinity, max: -Infinity };
      s.n++;
      if (ping != null) {
        s.min = Math.min(s.min, ping);
        s.max = Math.max(s.max, ping);
      }
      seen.set(id, s);
    }
  }
  let judged = 0;
  let flat = 0;
  for (const s of seen.values()) {
    if (s.n < runs.length * PRESENT_SHARE) continue;
    judged++;
    if (s.max > 0 && s.max <= FLAT_PING_MAX && s.max - s.min <= FLAT_PING_DRIFT) flat++;
  }
  const players = runs.reduce((m, r) => Math.max(m, r.rows.size), 0);
  return { mode: 'status', players, judged, flat, samples: runs.length };
}

// --- the server's own A2S answer ---------------------------------------------
// A2S_INFO, as any server browser asks it: players, bots, max. Read just
// before joining (the join probe) and while the observer is in, so the backend
// compares what the server lists with who `status` shows connected.
export function a2sInfo(address, timeoutMs = 3000) {
  const [host, portText] = address.split(':');
  const port = Number(portText);
  const query = Buffer.concat([Buffer.from([0xff, 0xff, 0xff, 0xff, 0x54]), Buffer.from('Source Engine Query\0', 'latin1')]);
  return new Promise((resolve) => {
    const sock = dgram.createSocket('udp4');
    const done = (v) => {
      clearTimeout(timer);
      sock.close();
      resolve(v);
    };
    const timer = setTimeout(() => done(null), timeoutMs);
    sock.on('error', () => done(null));
    sock.on('message', (msg) => {
      if (msg.length >= 9 && msg.readInt32LE(0) === -1 && msg[4] === 0x41) {
        sock.send(Buffer.concat([query, msg.subarray(5, 9)]), port, host); // challenge
        return;
      }
      done(parseInfo(msg));
    });
    sock.send(query, port, host);
  });
}

export function parseInfo(msg) {
  if (msg.length < 6 || msg.readInt32LE(0) !== -1 || msg[4] !== 0x49) return null;
  let at = 6; // header, protocol
  for (let i = 0; i < 4; i++) {
    // name, map, folder, game
    const end = msg.indexOf(0, at);
    if (end < 0) return null;
    at = end + 1;
  }
  at += 2; // app id
  if (at + 3 > msg.length) return null;
  return { players: msg[at], max: msg[at + 1], bots: msg[at + 2] };
}

const median = (xs) => {
  if (!xs.length) return undefined;
  const s = [...xs].sort((a, b) => a - b);
  return s[Math.floor(s.length / 2)];
};

// The last few meaningful console lines (no shader dumps), for a failure's log line.
function lastLines(text, n = 5) {
  return text
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter((l) => l && !/^(r\d|pd\d|pc\[|o[CT]\d|#|\{|\}|IDirect3D|GL_)/.test(l))
    .slice(-n);
}

// Why the game closed by itself: a fatal line in its output (which goes to
// Steam's service log, not console.log), or a crash dump written since `since`.
async function whyClosed(since, obsLogFrom = 0) {
  const out = await run('journalctl', ['-u', 'observer-desktop', '--since', `@${Math.floor(since / 1000)}`, '-n', '2000', '-o', 'cat', '--no-pager']);
  const fatal = (out || '').match(/^.*(FATAL|Out of memory|Segmentation fault|Aborted|core dumped|Assertion).*$/im);
  let dumps = 0;
  try {
    for (const f of await fs.readdir('/tmp/dumps')) {
      if ((await fs.stat(path.join('/tmp/dumps', f))).mtimeMs >= since && f.endsWith('.dmp')) dumps++;
    }
  } catch {}
  if (fatal) return `the game crashed: ${fatal[0].replace(/^\[[^\]]*\]\s*/, '').trim().slice(0, 150)}`;
  if (dumps) return 'the game crashed (crash dump written)';
  // A clean exit once the sampler was in the server's game: its anticheat, most
  // likely (the backend then sends status-only visits there). The sampler
  // reports into the module's memory, so the menu's own log says whether it had
  // started: "sampler started" since this visit began.
  const logBuf = await fs.readFile(path.join(DIR, 'observer.log')).catch(() => Buffer.alloc(0));
  if (logBuf.subarray(obsLogFrom).toString('utf8').includes('sampler started')) {
    return "the game closed during sampling (the server's anticheat?)";
  }
  return 'the game closed (no crash reported)';
}

async function readDone() {
  try {
    return JSON.parse(await fs.readFile(path.join(DIR, 'done.json'), 'utf8'));
  } catch {
    return null;
  }
}

// A visit that couldn't be finished: say so, and tell the backend so it frees
// the server now instead of holding it until its own timeout.
async function failed(job, error, consoleFrom = null) {
  await fs.rm(path.join(DIR, 'job.json'), { force: true });
  const extra = consoleFrom === null ? {} : { console: lastLines((await consoleSince(consoleFrom)).text) };
  log('visit_failed', { address: job.address, error, ...extra });
  try {
    if (!job.local) await api('POST', '/failed', { id: job.id, address: job.address, error });
  } catch (err) {
    log('error', { err: err.message });
  }
}

// Servers whose map only comes inside a Workshop addon the "mapsonly" download
// filter skips: they're joined with every download allowed.
const FULL_DOWNLOADS = path.join(DIR, 'full-downloads.json');
async function fullDownloads() {
  try {
    return new Set(JSON.parse(await fs.readFile(FULL_DOWNLOADS, 'utf8')));
  } catch {
    return new Set();
  }
}

// The A2S fields of a result: the median answer while we were in, and the
// count before and right after we joined (a real list rises by one: us).
export function a2sFields(samples, joinBefore) {
  const out = {};
  if (samples.length >= 3) {
    out.a2sPlayers = median(samples.map((s) => s.players));
    out.a2sBots = Math.min(median(samples.map((s) => s.bots)), out.a2sPlayers);
  }
  if (Number.isInteger(joinBefore) && samples.length) {
    out.joinBefore = joinBefore;
    out.joinAfter = samples[0].players;
  }
  return out;
}

async function visit(job) {
  const full = await fullDownloads();
  if (full.has(job.address)) job = { ...job, filter: 'all' };
  await fs.rm(path.join(DIR, 'done.json'), { force: true });
  // The console log holds earlier visits' `status` tables (other servers'
  // players' names and SteamIDs), and any server's client Lua can read files
  // in the game folder: start each visit with it empty.
  await fs.truncate(CONSOLE_LOG, 0).catch(() => {});
  // The same for garrysmod/data: earlier servers' addons leave their folders
  // there, and earlier builds left the sampler's own. The observer keeps
  // nothing in it between visits, so each visit starts with it empty.
  await emptyData();
  const visitConsole = await consoleSize();
  const obsLogFrom = (await fs.stat(path.join(DIR, 'observer.log')).catch(() => ({ size: 0 }))).size;
  let consoleAt = visitConsole;
  const started = Date.now();
  await fs.writeFile(path.join(DIR, 'job.json'), JSON.stringify(job));
  log('visit', { address: job.address });
  // The server's own A2S count just before we join, and its answers while we're
  // in: the list it shows everyone, set against the roster we see in game.
  const joinBefore = (await a2sInfo(job.address))?.players;
  const a2s = []; // { players, bots } every ~30 s once in game
  let inGame = false;
  let lastA2s = 0;
  // The module reads job.json when it loads, so the game starts after it's written.
  if (MANAGE_GAME && !(await startGame())) return failed(job, 'the game did not start (Steam never launched it)');
  let checks = 0;
  while (Date.now() - started < VISIT_TIMEOUT_MS) {
    await sleep(2000);
    if (!inGame) {
      const buf = await fs.readFile(path.join(DIR, 'observer.log')).catch(() => Buffer.alloc(0));
      inGame = buf.length > obsLogFrom && /\bin game \(/.test(buf.subarray(obsLogFrom).toString('utf8'));
    }
    if (inGame && Date.now() - lastA2s >= 30000) {
      lastA2s = Date.now();
      const info = await a2sInfo(job.address);
      if (info) a2s.push(info);
    }
    const done = await readDone();
    if (!done || done.id !== job.id) {
      // Kicked or refused: the console says so (and why) right away.
      const { text, next } = await consoleSince(consoleAt);
      consoleAt = next;
      // (Not our own "disconnect" when a visit ends: that's "Disconnect by user".)
      const kicked = [...text.matchAll(DISCONNECT)].find((m) => !/by user/i.test(m[1]));
      // An engine error while loading (a missing map...) is our side failing, not
      // the server turning us away: the backend mustn't take it as a refusal.
      if (kicked && /Host_?Error|Host Error|Missing map/i.test(kicked[1])) {
        await failed(job, `failed to load: ${kicked[1].replace(/^Disconnected:\s*/i, '').trim()}`.slice(0, 200), visitConsole);
        // The map was filtered out: once, try again with every download allowed.
        if (/Missing map/i.test(kicked[1]) && job.filter !== 'all') {
          const full = await fullDownloads();
          full.add(job.address);
          await fs.writeFile(FULL_DOWNLOADS, JSON.stringify([...full]));
          log('full_downloads', { address: job.address });
          try {
            await api('POST', '/queue', { address: job.address, ...(job.mode ? { mode: job.mode } : {}) });
          } catch (err) {
            log('error', { err: err.message });
          }
        }
        return;
      }
      if (kicked) return failed(job, `kicked: ${kicked[1].trim()}`.slice(0, 200), visitConsole);
      // The game closed mid-visit: say whether it crashed, and why.
      if (MANAGE_GAME && ++checks % 5 === 0 && (await gameGone())) return failed(job, await whyClosed(started, obsLogFrom), visitConsole);
      continue;
    }
    await fs.rm(path.join(DIR, 'done.json'), { force: true });
    if (done.error) return failed(job, String(done.error).slice(0, 200), visitConsole);
    if (done.statusOnly) {
      const runs = statusRuns((await consoleSince(visitConsole)).text);
      if (runs.length < 10) return failed(job, `status only: ${runs.length} of ${done.runs} status tables found in the console`, visitConsole);
      const result = { id: job.id, address: job.address, ...statusResult(runs) };
      // Positions read natively (once the module's reads passed their check).
      const n = done.native;
      if (n && [n.observed, n.still, n.visible].every(Number.isInteger) && n.still <= n.observed) {
        Object.assign(result, { observed: n.observed, still: n.still, visible: n.visible });
        // Only when every read was proven fresh (the menu leaves it out otherwise).
        if (Number.isInteger(n.moved) && n.moved + n.still <= n.visible) result.moved = n.moved;
      }
      Object.assign(result, a2sFields(a2s, joinBefore));
      if (!job.local) await api('POST', '/result', result);
      log('visit_done', { ...result, ...(n && n.check ? { nativeCheck: n.check } : {}) });
      return;
    }
    const result = { id: job.id, address: job.address };
    for (const k of ['players', 'observed', 'still', 'samples', 'visible', 'selfAlive', 'judged', 'active', 'quiet', 'flat', 'idle']) {
      if (Number.isInteger(done[k])) result[k] = done[k];
    }
    if (!job.local) await api('POST', '/result', result);
    log('visit_done', result);
    return;
  }
  return failed(job, `no result after ${VISIT_TIMEOUT_MS / 60000} min`, visitConsole);
}

// observer-queue --local <ip:port> writes local-job.json: a visit the backend
// doesn't know about (for testing, e.g. on one of our own servers). Its result
// is logged, never sent.
async function takeLocalJob() {
  const file = path.join(DIR, 'local-job.json');
  let job;
  try {
    job = JSON.parse(await fs.readFile(file, 'utf8'));
  } catch {
    return null;
  } finally {
    await fs.rm(file, { force: true });
  }
  if (!job || typeof job.address !== 'string' || !/^\d{1,3}(\.\d{1,3}){3}:\d{1,5}$/.test(job.address)) return null;
  const id = Array.from({ length: 16 }, () => '0123456789abcdef'[Math.floor(Math.random() * 16)]).join('');
  log('local_visit', { address: job.address });
  // Status-only unless asked for the Lua sampler by hand, as the backend does.
  return { id, address: job.address, local: true, mode: job.mode === 'lua' ? 'lua' : 'status' };
}

async function main() {
  await fs.mkdir(DIR, { recursive: true });
  // A job left by an earlier run would send the game somewhere nobody waits on.
  await fs.rm(path.join(DIR, 'job.json'), { force: true });
  // No token yet (set "Observer token" in the panel): say so and wait, don't crash-loop.
  while (!TOKEN) {
    log('waiting_for_token', { note: 'set the Observer token variable and restart' });
    await sleep(10 * 60 * 1000);
  }
  log('ready', { dir: DIR, api: API, manageGame: MANAGE_GAME, confirm: CONFIRM });
  if (MANAGE_GAME) {
    await quitGame(); // start from a closed game, like after every round
    await disarmLoader(); // and with nothing of the observer in the game's folders
  }
  for (;;) {
    // A round: visit while the backend has suspects, then close the game.
    try {
      for (;;) {
        // A local test visit first (observer-queue --local): not asked of the
        // backend, and its result only logged.
        const job = (await takeLocalJob()) || (await api('GET', '/next'));
        if (!job || !job.id || !job.address) break;
        await visit(job);
      }
    } catch (err) {
      log('error', { err: err.message });
    }
    if (MANAGE_GAME) await quitGame();
    await sleep(POLL_MS);
  }
}

// Run when started directly (not when a test imports the parsers).
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().catch((err) => {
    log('fatal', { err: err.message });
    process.exit(1);
  });
}
