"use strict";
// How a server row reads, as plain data, so tests/launcher_card_test.js can
// check it with Node. The page sets only textContent from this.

function serverView(row) {
  const i = row.info;
  const tag = row.ours ? "Meowy Network" : row.favorite ? "Favourite" : "Recent";
  if (!i) {
    return { title: row.address, detail: tag + " · not answering", join: false, up: false };
  }
  const humans = Math.max(0, i.players - i.bots);
  const parts = [tag, i.map, humans + "/" + i.max_players + " players", i.ping_ms + " ms"];
  if (i.password) parts.push("password");
  return { title: i.name || row.address, detail: parts.join(" · "), join: !i.password, up: true };
}

// A server from Meowy's index (core/src/index.rs Server). Its counts come from
// Steam's list (people only); the honest list's verdict is shown as found.
function foundView(s) {
  const fake = s.honest === "fake" && s.real >= 0;
  const people = fake ? s.real : s.players;
  const parts = [];
  if (s.gamemode) parts.push(s.gamemode);
  if (s.map) parts.push(s.map);
  parts.push(people + "/" + s.max_players + " players");
  if (fake) parts.push("FAKE: " + s.players + " shown");
  else if (s.honest === "verified") parts.push("verified");
  if (s.location) parts.push(s.location.toUpperCase());
  if (s.password === 1) parts.push("password");
  return { title: s.name || s.address, detail: parts.join(" · "), join: s.password !== 1, up: true };
}

// The Updates panel (status.updates, core/src/updater.rs Status).
function updatesView(u) {
  const lines = [];
  lines.push(u.running ? "Pack " + u.running + " (" + u.arch + ")" : "Pack version: not known here (it updates itself in the game)");
  if (u.pending) lines.push(u.pending + " is ready: it starts next time you open Garry's Mod");
  if (u.previous) lines.push("Previous: " + u.previous);
  if (u.bad && u.bad.length) lines.push("Not loaded again (failed to start): " + u.bad.join(", "));
  lines.push("Last check: " + u.last_check + (u.release_seq ? " (release " + u.release_seq + ")" : ""));
  return lines;
}

if (typeof module !== "undefined") module.exports = { serverView, foundView, updatesView };
