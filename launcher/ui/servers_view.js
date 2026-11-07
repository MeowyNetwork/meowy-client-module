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

if (typeof module !== "undefined") module.exports = { serverView };
