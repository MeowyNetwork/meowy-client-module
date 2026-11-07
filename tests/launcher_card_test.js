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

console.log(n + " checks passed");
