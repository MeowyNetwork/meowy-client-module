/**
 * The observer's supervisor: asks Meowy Network for the next suspect, hands it
 * to the game (the module's menu Lua reads $MEOWY_OBSERVER_DIR/job.json),
 * waits for done.json and sends the counts back.
 *
 *   MEOWY_OBSERVER_DIR          shared with the game (outside garrysmod)
 *   MEOWY_OBSERVER_API          default https://meowynetwork.com
 *   SERVER_REP_OBSERVER_TOKEN   the same token the backend has
 *
 * One visit at a time. A visit the game doesn't finish in VISIT_TIMEOUT is
 * dropped (the backend gives it up too).
 */

import fs from 'node:fs/promises';
import path from 'node:path';

const DIR = process.env.MEOWY_OBSERVER_DIR || path.join(process.env.HOME || '.', '.meowy-observer');
const API = (process.env.MEOWY_OBSERVER_API || 'https://meowynetwork.com').replace(/\/+$/, '');
const TOKEN = process.env.SERVER_REP_OBSERVER_TOKEN || '';
const POLL_MS = 60 * 1000;
const VISIT_TIMEOUT_MS = 25 * 60 * 1000;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (msg, extra = {}) => console.log(JSON.stringify({ ts: new Date().toISOString(), msg, ...extra }));

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

async function readDone() {
  try {
    return JSON.parse(await fs.readFile(path.join(DIR, 'done.json'), 'utf8'));
  } catch {
    return null;
  }
}

async function visit(job) {
  await fs.rm(path.join(DIR, 'done.json'), { force: true });
  await fs.writeFile(path.join(DIR, 'job.json'), JSON.stringify(job));
  log('visit', { address: job.address });
  const started = Date.now();
  while (Date.now() - started < VISIT_TIMEOUT_MS) {
    await sleep(10000);
    const done = await readDone();
    if (!done || done.id !== job.id) continue;
    await fs.rm(path.join(DIR, 'done.json'), { force: true });
    if (done.error) {
      log('visit_failed', { address: job.address, error: String(done.error).slice(0, 200) });
      return;
    }
    const result = { id: job.id, address: job.address, players: done.players, observed: done.observed, still: done.still, samples: done.samples };
    await api('POST', '/result', result);
    log('visit_done', result);
    return;
  }
  log('visit_timeout', { address: job.address });
}

async function main() {
  if (!TOKEN) throw new Error('SERVER_REP_OBSERVER_TOKEN is not set');
  await fs.mkdir(DIR, { recursive: true });
  log('ready', { dir: DIR, api: API });
  for (;;) {
    try {
      const job = await api('GET', '/next');
      if (job && job.id && job.address) await visit(job);
    } catch (err) {
      log('error', { err: err.message });
    }
    await sleep(POLL_MS);
  }
}

main().catch((err) => {
  log('fatal', { err: err.message });
  process.exit(1);
});
