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

// What the pack in the running game reports over the pipe (status.pack).
function packView(s) {
  const p = s.pack;
  if (!p) return s.running ? "Running (the pack isn't connected)" : "-";
  if (p.state === "menu") return "Main menu" + (p.version ? " (pack " + p.version + ")" : "");
  return (p.state === "joining" ? "Joining " : "Playing on ") + (p.address || "a server");
}

if (typeof module !== "undefined") module.exports = { serverView, foundView, packView };
