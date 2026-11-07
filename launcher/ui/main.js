"use strict";
// The main window: status, Play / Join / Repair, settings. Everything is
// asked of the launcher's own commands; the page holds no state of its own.

const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;
const $ = (id) => document.getElementById(id);

function set(id, text, cls) {
  const el = $(id);
  el.textContent = text;
  el.className = "v" + (cls ? " " + cls : "");
}

function say(text, cls) {
  $("message").textContent = text || "";
  $("message").className = cls || "";
}

async function refresh() {
  const s = await invoke("status");
  $("version").textContent = "Version " + s.version;
  if (!s.steam) set("game", "Steam wasn't found", "bad");
  else if (!s.game_found) set("game", "Not found in Steam's libraries", "bad");
  else set("game", (s.running ? "Running" : "Installed") + (s.build_id ? " (build " + s.build_id + ")" : "") +
    (s.steam_busy ? ", Steam is updating it" : ""), s.steam_busy ? "warn" : "ok");
  const line = {
    present: ["Yes", "ok"],
    off: ["Off (Meowy settings)", "warn"],
    missing: ["No: Steam removed the line (Repair puts it back)", "bad"],
    unknown: ["-", ""],
  }[s.line];
  set("line", line[0], line[1]);
  if (!s.game_found) set("modules", "-");
  else if (s.missing_modules.length) set("modules", "Missing: run MeowyClientPack.exe again", "bad");
  else set("modules", "Installed", "ok");
  set("last", s.last_repair || "-");
  $("lua-refresh").checked = s.disable_lua_refresh;
  $("sign-in").checked = s.start_at_sign_in;
  $("updates").textContent = s.updates;
  $("play").disabled = !s.steam;
  $("join").textContent = s.running ? "Join Meowy Network now" : "Join Meowy Network";
}

async function run(button, fn) {
  button.disabled = true;
  try {
    say(await fn());
  } catch (e) {
    say(String(e), "bad");
  } finally {
    button.disabled = false;
    refresh();
  }
}

$("play").onclick = () => run($("play"), () => invoke("play", { joinMeowy: false }));
$("join").onclick = () => run($("join"), () => invoke("play", { joinMeowy: true }));
$("repair").onclick = () => run($("repair"), () => invoke("repair_now"));
$("lua-refresh").onchange = (e) =>
  invoke("set_lua_refresh", { on: e.target.checked }).catch((err) => say(String(err), "bad")).finally(refresh);
$("sign-in").onchange = (e) =>
  invoke("set_sign_in", { on: e.target.checked }).catch((err) => say(String(err), "bad")).finally(refresh);

listen("status", refresh);
refresh();
