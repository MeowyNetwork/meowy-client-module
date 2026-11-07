"use strict";
// The card window: a crash on a server (Rejoin), a link to a server that
// isn't Meowy's (Join?), or a notice. Buttons send only the action's name;
// the launcher uses the address it holds itself.

const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;

async function render() {
  const view = cardView(await invoke("card"));
  document.getElementById("title").textContent = view.title;
  document.getElementById("text").textContent = view.text;
  document.getElementById("extra").textContent = view.extra;
  const box = document.getElementById("buttons");
  box.replaceChildren();
  for (const b of view.buttons) {
    const el = document.createElement("button");
    el.textContent = b.label;
    if (b.style) el.className = b.style;
    el.onclick = async () => {
      el.disabled = true;
      try {
        await invoke("card_answer", { action: b.action });
      } catch (e) {
        document.getElementById("extra").textContent = String(e);
      } finally {
        el.disabled = false;
      }
    };
    box.appendChild(el);
  }
  const primary = box.querySelector(".primary");
  if (primary) primary.focus();
}

document.addEventListener("keydown", (e) => {
  if (e.key === "Escape") invoke("card_answer", { action: "dismiss" });
});
listen("card", render);
render();
