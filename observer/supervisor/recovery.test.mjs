// node --test observer/supervisor
// A Steam that stopped taking launches: restarted once, then visits pause.
import test from 'node:test';
import assert from 'node:assert/strict';
import { restartSteam, startGameOrRecover, launchFailuresInARow, resetLaunchFailures, keepDiskFree } from './supervisor.mjs';

const GiB = 1024 ** 3;

test('enough free space: nothing is pruned', quiet(async () => {
  let pruned = 0;
  let quit = 0;
  const ok = await keepDiskFree({ free: async () => 30 * GiB, prune: async () => pruned++, quit: async () => quit++ });
  assert.equal(ok, true);
  assert.equal(pruned, 0);
  assert.equal(quit, 0);
}));

test('a nearly full disk: the game is closed, the cache pruned, visits go on', quiet(async () => {
  const frees = [3 * GiB, 40 * GiB];
  const order = [];
  const ok = await keepDiskFree({ free: async () => frees.shift(), prune: async () => order.push('prune'), quit: async () => order.push('quit') });
  assert.equal(ok, true);
  assert.deepEqual(order, ['quit', 'prune']);
}));

test('still full after pruning: no visit', quiet(async () => {
  const frees = [1 * GiB, 2 * GiB];
  const ok = await keepDiskFree({ free: async () => frees.shift(), prune: async () => {}, quit: async () => {} });
  assert.equal(ok, false);
}));

test('free space unknown: visits are not blocked', quiet(async () => {
  let pruned = 0;
  const ok = await keepDiskFree({ free: async () => null, prune: async () => pruned++, quit: async () => {} });
  assert.equal(ok, true);
  assert.equal(pruned, 0);
}));

function quiet(fn) {
  return async (...a) => {
    const orig = console.log;
    console.log = () => {};
    try {
      return await fn(...a);
    } finally {
      console.log = orig;
    }
  };
}

test('a launch that works needs no restart', quiet(async () => {
  resetLaunchFailures();
  let restarts = 0;
  const ok = await startGameOrRecover({ start: async () => true, restart: async () => (restarts++, true) });
  assert.equal(ok, true);
  assert.equal(restarts, 0);
  assert.equal(launchFailuresInARow(), 0);
}));

test('an unanswered launch restarts Steam and tries once more', quiet(async () => {
  resetLaunchFailures();
  const starts = [false, true];
  let restarts = 0;
  const ok = await startGameOrRecover({ start: async () => starts.shift(), restart: async () => (restarts++, true) });
  assert.equal(ok, true);
  assert.equal(restarts, 1);
  assert.equal(starts.length, 0);
  assert.equal(launchFailuresInARow(), 0);
}));

test('failures even after a restart are counted, and a success clears them', quiet(async () => {
  resetLaunchFailures();
  const never = { start: async () => false, restart: async () => true };
  for (let i = 1; i <= 3; i++) {
    assert.equal(await startGameOrRecover(never), false);
    assert.equal(launchFailuresInARow(), i);
  }
  await startGameOrRecover({ start: async () => true, restart: async () => true });
  assert.equal(launchFailuresInARow(), 0);
}));

test('no second launch when Steam never came back', quiet(async () => {
  resetLaunchFailures();
  let starts = 0;
  const ok = await startGameOrRecover({ start: async () => (starts++, false), restart: async () => false });
  assert.equal(ok, false);
  assert.equal(starts, 1);
  assert.equal(launchFailuresInARow(), 1);
}));

test('restartSteam waits for Steam to come back, then lets it settle', quiet(async () => {
  let t = 0;
  const waits = [];
  let ctl = 0;
  const ups = [false, false, true];
  const back = await restartSteam({
    ctl: async () => (ctl++, true),
    up: async () => ups.shift(),
    wait: async (ms) => {
      waits.push(ms);
      t += ms;
    },
    now: () => t,
  });
  assert.equal(back, true);
  assert.equal(ctl, 1);
  assert.deepEqual(waits, [25000, 5000, 5000, 45000]);
}));

test('restartSteam gives up after four minutes without Steam', quiet(async () => {
  let t = 0;
  const back = await restartSteam({
    ctl: async () => true,
    up: async () => false,
    wait: async (ms) => {
      t += ms;
    },
    now: () => t,
  });
  assert.equal(back, false);
  assert.ok(t >= 4 * 60 * 1000 && t < 4 * 60 * 1000 + 10000);
}));
