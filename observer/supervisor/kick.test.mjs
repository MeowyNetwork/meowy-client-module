// node --test observer/supervisor
// A refusal the menu didn't notice ends the visit from the console line, not at the join timeout.
import test from 'node:test';
import assert from 'node:assert/strict';
import { refusedWhileJoining, KICK_GRACE_MS } from './supervisor.mjs';

const t = 1_800_000_000_000;

test('a kick line while joining ends the visit once the menu stays silent past the grace', () => {
  assert.equal(refusedWhileJoining({ leftAt: t, inGame: false, now: t + KICK_GRACE_MS - 1 }), false, 'the menu gets its chance first');
  assert.equal(refusedWhileJoining({ leftAt: t, inGame: false, now: t + KICK_GRACE_MS }), true);
});

test('no kick line, or already in game: the menu decides', () => {
  assert.equal(refusedWhileJoining({ leftAt: 0, inGame: false, now: t + 60 * 60 * 1000 }), false);
  // In game, players' chat reaches the console: a line there proves nothing alone.
  assert.equal(refusedWhileJoining({ leftAt: t, inGame: true, now: t + 60 * 60 * 1000 }), false);
});
