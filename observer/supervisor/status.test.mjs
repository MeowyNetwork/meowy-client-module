// node --test observer/supervisor/status.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { statusRuns, statusResult } from './supervisor.mjs';

// One `status` table as the GMod client prints it, with these pings for four
// accounts, a bot, and a fifth account only in some runs.
function table(pings, withLate) {
  const rows = [
    `#      2 "Idle One"          STEAM_0:1:111   2:03:04     ${pings[0]}    0 active`,
    `#      3 "Idle \\"Two\\""     STEAM_0:0:222   2:03:04     ${pings[1]}    0 active`,
    `#      4 "Real"              STEAM_0:1:333     45:10     ${pings[2]}    0 active`,
    `#      5 "Far"               STEAM_0:0:444     12:00     ${pings[3]}    2 active`,
    `#      6 "Bot01"             BOT                                     active`,
  ];
  if (withLate) rows.push('#      7 "Late"              STEAM_0:1:555      0:30     40    0 active');
  return ['hostname: Some Server', 'players : 5 humans, 1 bots (32 max)', '# userid name                uniqueid            connected ping loss state', ...rows, 'Lua Started!'].join('\n');
}

test('status tables: accounts present in most runs are judged; flat low pings counted', () => {
  const text = Array.from({ length: 20 }, (_, i) => table([5, 6 + (i % 2), 30 + (i % 9), 9 + (i % 4)], i > 14)).join('\nsome other line\n');
  const runs = statusRuns(text);
  assert.equal(runs.length, 20);
  assert.equal(runs[0].bots, 1);
  assert.deepEqual(statusResult(runs), { mode: 'status', players: 5, judged: 4, flat: 2, samples: 20 });
});

test('a client still connecting (no ping yet) is counted present but never flat', () => {
  const text = Array.from({ length: 10 }, () => '# userid name uniqueid connected ping loss state\n#  9 "New" STEAM_0:1:9 spawning').join('\n');
  assert.deepEqual(statusResult(statusRuns(text)), { mode: 'status', players: 1, judged: 1, flat: 0, samples: 10 });
});
