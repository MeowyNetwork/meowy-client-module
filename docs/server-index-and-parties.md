# Discovery index and parties (roadmap MR3)

Find servers fills in about a second from Meowy's own index of every Garry's Mod server, with honest
counts, and then refreshes live from Steam. Parties let friends move between servers together.

Research: `GMODEnhanced/review_2026-11-06/major-reworks.md` §MR3, `startup-menu-speed.md` #6,
`ROADMAP.md` Tier 3 #3.

## 1. What exists today

- **Pack.** Find servers (`src/menu/browser.js`) is fed by `src/lua/menu_page.lua` from Steam's
  matchmaking list (`src/server_query.*`): servers with players first, then the empty ones. The players
  stage takes ~20 s, because Steam pings at most ~5,000 servers a minute.
- **Honest list.** `meowy.ServerTag` combines the backend's verdicts (`GET /api/pack/servers`), this
  PC's own count checks and the AFK share.
- **Backend** (`MeowyNetwork/website`, `backend/`, Pelican `e0ae610a`, behind nginx on
  meowynetwork.com). `serverRep/crawler.js` already calls Steam's
  `IGameServersService/GetServerList` (`\appid\4000`, limit 20,000) every 15 min, then A2S-checks the
  top 400 servers with 10+ players. The Steam key and the verdicts are already in that process.
- **Steam.** GMod's bundled `steam_api(64).dll` exports everything parties need, on both
  architectures: `SteamMatchmaking_v009` (lobbies), `SteamFriends_v018` (Rich Presence, invites),
  `SteamUtils_v010` (call results), `SteamAPI_RegisterCallback`. GMod itself uses no lobbies (no
  `connect_lobby` in its binaries), so the pack owns lobby invites. `client.dll` handles Rich Presence
  join requests itself.

## 2. The index service

**Where it runs.** A new module `backend/src/serverIndex/` in the existing website backend, with the
route `GET /api/pack/index`. It reuses the Steam key, the crawler's verdicts (same process), nginx and
TLS, and the existing deploy path. There's no new server to run.

**How it's built.**

| Job | How often | Cost |
|---|---|---|
| Steam's master list, `GetServerList` `\appid\4000`, limit 20,000 | every 60 s | 1,440 calls a day (the key's limit is 100,000); ~2 MB JSON |
| Same list with `\password\0` to learn which servers need a password | every 5 min | 288 calls a day |
| A2S_INFO sweep: the game description ("DarkRP"), bots, whether it answers | servers with players: every 10 min, empty ones: every 60 min | ~3 queries/s; one at a time per /24, as the crawler does |

The crawler's own 15-minute fetch reads the index's latest list instead of calling Steam again.

**What it serves.** One snapshot per minute, built once and gzipped once, with an ETag:

```
GET /api/pack/index   (Accept-Encoding: gzip; If-None-Match)
200 { "v": 1, "at": <unix s>, "cols": ["a","n","d","m","g","w","c","l","p","x","b","v","pw","h","r"],
      "s": [[...], ...] }
```

| Col | Meaning | Source |
|---|---|---|
| `a` | `ip:gameport` (public IPv4 only, `parsePublicAddress`) | Steam |
| `n`, `m` | name (≤ 96 chars), map | Steam |
| `d` | game description, the gamemode's title | A2S sweep ("" until swept) |
| `g`, `w`, `c`, `l` | gamemode folder, its Workshop ID, category, location | Steam tags `gm:` `gmws:` `gmc:` `loc:` |
| `p`, `x`, `b` | players, max, bots | Steam (bots also from A2S) |
| `v` | network version as an int, as the pack's own query reads it | Steam `version` |
| `pw` | needs a password (1/0) | `\password\0` difference |
| `h`, `r` | verdict (`fake`/`verified`/`unverified`) and real count of a fake | `serverRep.verdictFor` |

It lists every server Steam lists, empty ones included: the pack shows the ones with players first,
just as it does now. That's an estimated 4,800 rows, ~700 KB of JSON, ~200 KB gzipped.
`Cache-Control: public, max-age=30`; a 304 when nothing changed.

**Ping hints** (decided: both). The index never pings anything for the player, because ping is about the
player's own network. Until Steam has measured a server, the pack shows:
- the ping it measured on that server's network (/24) before. These are kept in
  `%LOCALAPPDATA%\MeowyClientPack\pings.json` (30 days, 4,000 networks), not in `garrysmod/data` or
  the game's cookies, which any server's Lua can read;
- else an estimate from the distance between the player's coarse location (`X-Meowy-Geo`) and the
  server's (`la`/`lo`): 10 ms + 0.014 ms per km;
- else "…", ranked as 150 ms.

Hints are shown as "~45", greyed. The status line credits DB-IP (CC BY 4.0).

**Privacy.**
- No player names and no SteamIDs. A2S_PLAYER isn't part of the index; the crawler's existing rules
  stay as they are.
- Requests aren't logged with their IP. The rate limiter keeps IPs in memory for its window only.
- Everything served is what the servers publish to Steam anyway.

**Abuse limits.**
- 20 requests a minute per IP (a browser open for an hour needs 60 an hour). The body is shared and
  pre-compressed, so a flood costs a buffer copy.
- Owners can't submit anything. The index holds only what Steam lists and what our own crawler
  measured. Verdicts keep their existing rules (several networks, crawler-verified counts).
- The index never decides what a player sees. The pack keeps GMod's own blacklist
  (`IsServerBlacklisted`), its ranking formula and the honest-list penalty. The index only supplies
  rows.
- Kill switch: the backend answers 503 (`SERVER_INDEX=0`), and the pack goes back to Steam alone. The
  player's own switch is `meowy_server_index 0`.

## 3. The browser with the index

1. **On open (Internet tab):** the pack fetches `/api/pack/index` natively (WinHTTP, gzip, no
   redirects, 3 s timeout, 1 MB cap, as `server_rep.cpp` already does) and starts Steam's query at the
   same moment.
2. **When the index arrives:** every row goes through the same `browseRow` path (blacklist, honest
   tags, row numbers; addresses stay in Lua) in one batch. Status line: "From Meowy's index, 12 s old ·
   checking with Steam…". Pings show as hints (grey, "~45") until Steam answers.
3. **As Steam answers:** a server already listed is **updated in place**: ping, players, map,
   password. Its row keeps its number, so a selection and scroll position survive. A server the index
   missed is added. Nothing is removed.
4. **Live:** while the browser stays open, the index is fetched again every 60 s (usually a 304), and
   counts change in place.
5. **Fallback:** if the index fails (timeout, HTTP error, malformed, older than 10 min, or switched
   off), the list is what it is today: Steam alone. Favourites, History, LAN and Friends always use
   Steam or the game, as now.

The page needs `MeowyBrowse.update(gen, rows)`, the in-place update by row number, and a "source"
line. The row format doesn't change.

## 4. Parties

**Native: `src/party_steam.*`.** It's the same pattern as `timeline_steam.cpp`: exports resolved from
the already-loaded `steam_api`, SEH-guarded, off for the session after a fault.

- **Lobby:** `CreateLobby(type, 8)`. The result is polled through `ISteamUtils::IsAPICallCompleted` /
  `GetAPICallResult`, with no call-result object.
- **Lobby data** (owner): `v=1`, `srv` the target server, `srvname`, `go` `"<seq>|<address>"`.
- **Member data:** `at` (the server you're on, or empty), `pack` (the pack's version).
- **Invites:** `ActivateGameOverlayInviteDialog(lobby)` (Steam's own dialog), or
  `InviteUserToLobby` for a friend picked in our list. Friends playing GMod come first.
- **Joining:**
  - The invite callback `GameLobbyJoinRequested_t` (id 333), through `SteamAPI_RegisterCallback`.
  - `+connect_lobby <id>` on the command line, when Steam starts the game from an invite.
- **Rich Presence:**
  - `steam_player_group` set to the lobby ID and `steam_player_group_size` set to the member count, so
    Steam's friends list shows the party together;
  - `meowy_pack` set to the pack's version, so a friend's pack can see who has the pack.
- **State is polled once a second** from menu-state Lua (`GetLobbyData`, members). Lobby data updates
  are pushed into Steam's local cache, so polling costs nothing, and only one callback is registered.

**Lua and page.**
- A party card on the home screen: members with where each is, Invite, Leave. Every server's details
  panel gets "Take the party here" (for the leader).
- **Moving:** the leader's "Take the party here" joins the leader to that server and bumps `go`. Each
  member gets a 10-second "Following <leader> to <server> · Stay" countdown. See open point 4 for in
  game. A member who was already there does nothing.
- **Content:** each member's pack warms the target as soon as `srv` changes
  (`MeowyClientPackWarm`, already there). MR2 can hook in later.
- **Friends without the pack:** "Invite to this server" uses `InviteUserToGame(friend, "+connect
  <address>")`, which stock GMod already handles. They come along, but don't follow later moves.

**Safety.**
- A follow always needs the countdown with Stay or a click. A `go` whose address fails
  `publicAddress` is ignored.
- Only the lobby's owner's `go` counts.
- At most one follow every 30 s, so a hostile leader can't bounce people around.
- Leaving the party, or `meowy_party 0`, stops all of it.

## 5. Decided (2026-11-07)

- **Index host:** the website backend (`e0ae610a`); deployed 2026-11-07 from MeowyNetwork/website#24.
- **Ping hints:** remembered pings first, then the GeoIP estimate (DB-IP City Lite).
- **Party type:** friends-only lobby, up to 8 people.
- **In-game move:** a 10-second countdown, then the member follows, unless they choose Stay. Stay is on the
  Escape menu's card and in the home's party card, and `meowy_party_stay` does the same. There's also a
  banner over the game, and at most one follow every 30 s.

Live numbers (2026-11-07): 5,979 servers, 1,328 with players (17,840 players); 208 KB gzipped; 0.2–0.4 s to
fetch from the Netherlands. Steam's `\password\0` list hasn't contributed yet; passwords come from the
A2S sweep.

## 6. Milestones (one PR each)

1. **Index:** the backend module and route (website repo PR). It's deployed to `e0ae610a` only after
   approval.
2. **Browser on the index:** the native fetch, the merge and live refresh, and the fallback
   (pack PR).
3. **Parties:** native lobby and Rich Presence, the party card, and follow (pack PR).

**Verification:**
- Counts and versions from the index match Steam's list in the live client for 50 servers.
- Find servers is first painted in ≤ 1.5 s from opening, cold.
- With the index blocked, Find servers behaves exactly as today.
- Two accounts: invite, see where the other is, and move together. This needs a second Steam account
  for the last step.
- After any deploy, server and client consoles are checked for errors.

**Later (not in these PRs):** server history (uptime, real-player curve), MR2 content sizes, MR7
grades, and a public web page that reads the same index.
