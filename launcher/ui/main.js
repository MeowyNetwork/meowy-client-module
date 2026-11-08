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
    off: ["Off: you turned it off (run the installer again to turn it on)", "warn"],
    missing: ["No: Steam removed the line (Repair puts it back)", "bad"],
    unknown: ["-", ""],
  }[s.line];
  set("line", line[0], line[1]);
  if (!s.game_found) set("modules", "-");
  else if (s.missing_modules.length) set("modules", "Missing: run MeowyClientPack.exe again", "bad");
  else set("modules", "Installed", "ok");
  set("last", s.last_repair || "-");
  set("pack", packView(s));
  $("lua-refresh").checked = s.disable_lua_refresh;
  $("back-up").checked = s.notify_back_up;
  $("sign-in").checked = s.start_at_sign_in;
  $("updates").replaceChildren(...updatesView(s.updates).map((t) => {
    const d = document.createElement("div");
    d.textContent = t;
    return d;
  }));
  $("notice").textContent = s.notice || "";
  $("notice").hidden = !s.notice;
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
$("back-up").onchange = (e) =>
  invoke("set_back_up", { on: e.target.checked }).catch((err) => say(String(err), "bad")).finally(refresh);

// One server row (text only), with Join when the view allows it.
function serverRow(v, address) {
  const el = document.createElement("div");
  el.className = "server" + (v.up ? "" : " down");
  const text = document.createElement("div");
  const title = document.createElement("div");
  title.className = "name";
  title.textContent = v.title;
  const detail = document.createElement("small");
  detail.textContent = v.detail;
  text.append(title, detail);
  el.appendChild(text);
  if (v.join) {
    const b = document.createElement("button");
    b.textContent = "Join";
    b.onclick = () => run(b, () => invoke("join_server", { address }));
    el.appendChild(b);
  }
  return el;
}

// Servers: Meowy Network, then the player's Steam favourites and recent
// Garry's Mod servers. Asked when the window opens, on Refresh, and every 30 s.
let asking = false;
async function servers() {
  if (asking) return;
  asking = true;
  $("refresh").disabled = true;
  try {
    const rows = await invoke("server_list");
    const box = $("servers");
    box.replaceChildren(...rows.map((row) => serverRow(serverView(row), row.address)));
    if (!rows.length) box.textContent = "No servers to show.";
  } catch (e) {
    $("servers").textContent = String(e);
  } finally {
    asking = false;
    $("refresh").disabled = false;
  }
}
$("refresh").onclick = servers;
setInterval(() => { if (!document.hidden) servers(); }, 30000);

// Find servers: Meowy's server index (every Garry's Mod server Steam lists,
// with the honest list's verdicts), searched as you type.
let findTimer = null, findSeq = 0;
async function find() {
  const seq = ++findSeq;
  try {
    const r = await invoke("find_servers", { query: $("find").value });
    if (seq !== findSeq) return; // a newer search is on its way
    const age = Math.max(0, Math.round(Date.now() / 1000 - r.at));
    $("find-info").textContent = r.servers.length + " shown of " + r.total + " servers · list " +
      (age < 90 ? age + " s" : Math.round(age / 60) + " min") + " old";
    $("found").replaceChildren(...r.servers.map((s) => serverRow(foundView(s), s.address)));
  } catch (e) {
    if (seq === findSeq) $("find-info").textContent = String(e);
  }
}
$("find").oninput = () => { clearTimeout(findTimer); findTimer = setTimeout(find, 250); };

listen("status", refresh);
refresh();
servers();
find();
