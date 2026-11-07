// The launcher's card window text and buttons (launcher/ui/card_view.js):
//   node tests/launcher_card_test.js
"use strict";
const assert = require("assert");
const path = require("path");
const { cardView } = require(path.join(__dirname, "..", "launcher", "ui", "card_view.js"));

let n = 0;
function check(name, fn) {
  fn();
  n++;
}

const actions = (v) => v.buttons.map((b) => b.action);

check("crash with a report: Rejoin is primary, Details shown", () => {
  const v = cardView({ kind: "crash", address: "138.201.62.52:27015", name: "Meowy Network", report: true });
  assert.strictEqual(v.text, "You were on Meowy Network (138.201.62.52:27015). Jump back in?");
  assert.deepStrictEqual(actions(v), ["dismiss", "details", "join"]);
  assert.strictEqual(v.buttons.find((b) => b.style === "primary").label, "Rejoin");
});

check("crash without a report (ended in Task Manager): no Details", () => {
  const v = cardView({ kind: "crash", address: "1.2.3.4:27015", name: "", report: false });
  assert.strictEqual(v.text, "You were on 1.2.3.4:27015. Jump back in?");
  assert.deepStrictEqual(actions(v), ["dismiss", "join"]);
  assert.match(v.extra, /No crash report/);
});

check("a link to another server asks first", () => {
  const v = cardView({ kind: "confirm", address: "1.2.3.4:27015" });
  assert.match(v.text, /isn't a Meowy Network server/);
  assert.deepStrictEqual(actions(v), ["dismiss", "join"]);
});

check("notice: one OK", () => {
  const v = cardView({ kind: "notice", title: "Link not followed", text: "x" });
  assert.deepStrictEqual(actions(v), ["dismiss"]);
  assert.strictEqual(v.title, "Link not followed");
});

check("no card or an unknown kind: only Close", () => {
  assert.deepStrictEqual(actions(cardView(null)), ["dismiss"]);
  assert.deepStrictEqual(actions(cardView({ kind: "something-new" })), ["dismiss"]);
});

check("text is shown as text: markup in a server name stays literal", () => {
  const v = cardView({ kind: "crash", address: "1.2.3.4:1", name: "<img src=x onerror=alert(1)>", report: false });
  assert.ok(v.text.includes("<img"), "returned as a string for textContent, not parsed");
});

check("back up: Join or Not now, with the server's own name", () => {
  const v = cardView({ kind: "back_up", address: "138.201.62.52:27015", name: "Meowy Network | DarkRP" });
  assert.strictEqual(v.title, "Meowy Network is back up");
  assert.strictEqual(v.text, "Meowy Network | DarkRP answers again.");
  assert.deepStrictEqual(actions(v), ["dismiss", "join"]);
  assert.strictEqual(cardView({ kind: "back_up", address: "x", name: "" }).text, "The server answers again.");
});

const { serverView } = require(path.join(__dirname, "..", "launcher", "ui", "servers_view.js"));
const info = (o) => Object.assign({ name: "Meowy Network", map: "rp_downtown_meowy", gamemode: "DarkRP", app_id: 4000,
  players: 12, max_players: 31, bots: 2, password: false, vac: true, ping_ms: 22 }, o);

check("server row: humans only, map, ping; Join", () => {
  const v = serverView({ address: "138.201.62.52:27015", ours: true, favorite: false, info: info({}) });
  assert.strictEqual(v.title, "Meowy Network");
  assert.strictEqual(v.detail, "Meowy Network · rp_downtown_meowy · 10/31 players · 22 ms");
  assert.ok(v.join && v.up);
});

check("server row: not answering, no Join", () => {
  const v = serverView({ address: "1.2.3.4:27015", ours: false, favorite: true, info: null });
  assert.deepStrictEqual(v, { title: "1.2.3.4:27015", detail: "Favourite · not answering", join: false, up: false });
});

check("server row: a password server can't be joined from the launcher (it never sends passwords)", () => {
  const v = serverView({ address: "1.2.3.4:27015", ours: false, favorite: false, info: info({ name: "", password: true }) });
  assert.strictEqual(v.title, "1.2.3.4:27015", "falls back to the address");
  assert.ok(v.detail.endsWith("· password") && !v.join);
});

const { foundView } = require(path.join(__dirname, "..", "launcher", "ui", "servers_view.js"));
const found = (o) => Object.assign({ address: "1.2.3.4:27015", name: "City RP", gamemode: "DarkRP", map: "rp_x",
  players: 40, max_players: 128, bots: 3, password: 0, honest: "", real: -1, location: "gb" }, o);

check("index row: Steam's count as is (it already leaves bots out), location", () => {
  const v = foundView(found({}));
  assert.strictEqual(v.detail, "DarkRP · rp_x · 40/128 players · GB");
  assert.ok(v.join);
});

check("index row: a fake shows its real count and what it claims", () => {
  const v = foundView(found({ honest: "fake", players: 128, real: 3 }));
  assert.strictEqual(v.detail, "DarkRP · rp_x · 3/128 players · FAKE: 128 shown · GB");
});

check("index row: verified mark; a password server has no Join", () => {
  assert.ok(foundView(found({ honest: "verified" })).detail.includes("verified"));
  const v = foundView(found({ password: 1, name: "" }));
  assert.ok(!v.join && v.title === "1.2.3.4:27015" && v.detail.endsWith("password"));
});

console.log(n + " checks passed");
