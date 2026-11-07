# Content platform: a server's content ready before you click Join

Roadmap MR2 (`GMODEnhanced\review_2026-11-06\major-reworks.md` §MR2, `join-pipeline.md` A1). Status:
**design agreed 2026-11-07; MVP in progress.**

Decided with Catualus (2026-11-07):
- The advert is the convar **`meowy_content`** (`FCVAR_NOTIFY`, so it's in `A2S_RULES`).
- Meowy's server publishes the **inline Workshop list only** for the MVP; no hosted manifest yet.
- **Consent:** automatic for Meowy servers and servers you've joined before; any other server gets
  a "Get ready" button showing the size; never more than 4 GB per server without a click.
- **Measuring:** Workshop items this PC doesn't have, added to dev temporarily (one stock join, one
  prepared join).

## Why

Downloads are the only part of a first join that takes minutes. Garry's Mod's WorkshopDL fetches a
server's Workshop items inside the loading screen, strictly one at a time at high priority
(join-pipeline §3.2). Measured on dev (138.201.62.52:27015):

| Join | Time to in game |
|---|---|
| First-ever join, 1.28 GB Workshop (modelled at 100 Mbit/s, join-pipeline §6) | ~3.7 min |
| First join of a game start, files already on disk | ~22.5 s |
| Warm rejoin | ~15 s |

The pack already fetches items you have **seen before** at the menu (join pre-warm, `prewarm.h`,
`home.lua`). It can't help a first join, because it only knows a server's list after a join.
This adds the missing half: the server says what it will ask for, before you join.

## The deciding test (2026-11-07): passed

Question: does an item fetched from the menu with `steamworks.DownloadUGC` satisfy the join's
WorkshopDL, or does the join download it again?

1. Two Workshop items that weren't on this PC were downloaded at the main menu with
   `steamworks.DownloadUGC` (no subscription):
   - `3377934709` (new-style, 102 MB): 2.9 s;
   - `326332456` gm_fork (legacy, 66 MB compressed, 199 MB unpacked into `cache/workshop`): 8.3 s.
2. Only then were they added to dev's `resource.AddWorkshop` list (temporary file, restart).
3. Join (pack loading screen's per-status timeline, `last_join.txt`):
   - `32/35 - Downloaded! 'ROBLOX 2007 Player Model'` and `33/35 - Checking for updates 'gm_fork'`,
     0.03 s each;
   - no "Downloading Workshop content", no "Extracting Workshop content" for either;
   - Workshop stage 1.75 s for 35 items; in game at 31.4 s.

Control, the same day (a stock join that met three new items it didn't have, 205 MB):

| Item | In the join |
|---|---|
| 97.8 MB new-style | 2.7 s |
| 16.6 MB legacy | 3.6 s (2.2 s download, 1.4 s unpack) |
| 90.5 MB new-style | 2.8 s |
| Workshop stage | 10.9 s instead of ~1.5 s |

So the menu route works for both kinds of item: a prepared item costs the join ~0.03 s instead of
its download (and, for legacy items, its unpack). No `ISteamUGC` proxy is needed for the main
mechanism; the proxy (join-pipeline #6a) stays a later option for pipelining what wasn't prepared.

Download rate on this PC: ~35 MB/s from Steam's CDN, so 1.28 GB is ~40 s here and ~105 s at
100 Mbit/s. That whole time moves from the loading screen to the menu.

## How it works

```
 server (any)                         client (pack, menu state)
 ────────────                         ─────────────────────────
 resource.AddWorkshop(...) ──┐        hover / pick / Continue / favourite
 (SDK records the list)      │              │
                             ▼              ▼
 convar content_manifest ─── A2S_RULES ──► rules query (native UDP, cached)
   FCVAR_NOTIFY                             │
   "1;ws=id,id,~id;url=https://..#hash"     ▼
                                      plan: Steam state per item (installed, update,
 optional manifest.json (HTTPS) ────►       legacy unpack present, Workshop newer)
   fastdl files + sha256, lua bundle         │
                                            ▼
                                      prepare: steamworks.DownloadUGC queue
                                            │ (stops asking when a join starts;
                                            ▼  Steam finishes what's in flight)
                                      join: GMod's own WorkshopDL/FastDL, unchanged
                                            │
                                            ▼
                                      learn: items the join mounted (existing
                                             prewarm record) = manifest for next time
```

### 1. The advert: one convar in A2S_RULES

- `FCVAR_NOTIFY` puts a convar in `A2S_RULES` (wiki `Enums/FCVAR`). Measured on dev: the rules
  query answers 162 rules, 3.7 KB in split packets, in 46 ms, and Lua-created convars are in it
  (`meowy_medic_enabled`, `stacker_*`).
- **Value, v1:** `1;ws=<id>,<id>,~<id>` (and, in v2, `;url=<https URL>#<16 hex>`)
  - `ws=`: the Workshop IDs the server registered with `resource.AddWorkshop`, plus the
    gamemode's and map's items when known. Decimal only. `~` marks a deferred item (§5).
  - `url=` (optional): a full manifest (§2) for what doesn't fit, with the first 16 hex digits of
    its SHA-256, so a cached copy is reused and a changed or tampered one is rejected.
  - About 11 bytes per item: 30 items ≈ 350 bytes, 150 items ≈ 1.7 KB. Beyond ~2 KB, `ws=` is
    left out and the URL carries the list.
- **Name:** `meowy_content`. Any server can still publish it; the pack reads it on every server.

### 2. Full manifest v1 (optional, HTTPS, ≤ 256 KB)

```json
{
  "v": 1,
  "map": "rp_downtown_meowy",
  "gamemode": "darkrp",
  "workshop": [ { "id": "3791005669" }, { "id": "3657646236", "deferred": true } ],
  "fastdl": { "base": "https://meowynetwork.com/fastdl/",
              "files": [ { "path": "resource/fonts/rubik.ttf", "size": 123456, "sha256": "…" } ] },
  "lua": null
}
```

Sizes and versions of Workshop items aren't in it: the client asks Steam (the same details query
WorkshopDL makes), so a stale manifest can't lie about them. `lua` is reserved for the v2 bundle
prefill (§6).

### 3. Server side (Lua, any server)

One server file (Meowy: its own `meowy_*` addon):
- wraps `resource.AddWorkshop` (and, for v2, `resource.AddFile` / `AddSingleFile`) to record what
  the server registers, from the moment it loads;
- a few seconds after `InitPostEntity`, sets the convar with `FCVAR_NOTIFY`, before players are in;
- for the URL form: writes `data/content_manifest.json` with SHA-256 of each FastDL file
  (`util.SHA256`), for the owner's web host to serve.
- **Meowy, MVP:** inline list only. The full manifest comes with v2 (FastDL prefetch, Lua bundle).

### 4. Client side (pack)

- **When:** the rules query runs for the same moments that warm a server today (`home.lua`
  `warm()`): the Continue server 3 s after the menu shows, a row the pointer rests on for 0.4 s
  (home card, Find servers), a server picked in Find servers. Plus, new, servers never joined
  (today `warm()` needs a recorded join). At most once per server per 10 minutes; the answer is
  cached in `%LOCALAPPDATA%\MeowyClientPack\content\<address>.txt`.
- **Query:** native, plain UDP like `resilience_net.cpp`'s A2S_INFO (challenge round, split
  replies), public IPv4 `a.b.c.d:port` only, on a worker thread; Lua polls for the answer.
- **Fetch (URL form):** WinHTTP like `server_rep.cpp`, HTTPS only, ≤ 256 KB, 5 s timeout,
  SHA-256 (BCrypt) checked against the advert's hash before Lua sees it. Parsed with
  `util.JSONToTable` and validated (decimal IDs, relative paths without `..`, an extension
  allow-list matching what FastDL may send).
- **Plan:** the list from the advert, merged with what the last join learned. Per item, the same
  rule `home.lua` uses today: needed when Steam says not installed / update known / mid-download,
  a legacy item's unpacked `cache/workshop/<id>.gma` is gone, or `steamworks.FileInfo` says the
  Workshop's version is newer. The total size comes from `steamworks.FileInfo`.
- **Prepare:** the existing `steamworks.DownloadUGC` queue, one item at a time (the menu's
  DownloadUGC is single-flight: "DownloadUGCManager::SetActive: There's already a download!"),
  with the server's row showing "Getting ready: 412 MB of 1.3 GB". It stops asking for new items
  when a join starts; Steam finishes the one in flight, and the join uses it.
- **Consent** (§7): automatic, or a "Get ready" button.
- **Join:** untouched. Whatever wasn't prepared downloads in the loading screen, as today.

Not in the MVP: native `ISteamUGC::DownloadItem(id, false)` for several items at once. While a
join runs, GMod's WorkshopDL gets every `DownloadItemResult_t`, and it logs "Received wrong
downloaded item?" for ones it didn't ask for; what it then does needs its own test.

### 5. Progressive join (v2, server SDK)

A server marks content-only items deferred (`~id`). The SDK leaves them out of
`resource.AddWorkshop`, and after spawn the client realm downloads and mounts them
(`steamworks.DownloadUGC` + `game.MountGMA`). Error models reload on mount, error materials don't
(wiki `game.MountGMA`), so only items whose materials the map doesn't use qualify. Pack users get
deferred items prepared like the rest, so for them nothing changes. First candidate: Meowy's
Weaponry item (join-pipeline #7).

### 6. Later (v2)

- FastDL prefetch from the manifest: worker threads, keep-alive, `Range` resume into
  `download/<path>.part`, SHA-256 check, rename; never over an existing file (GMod's own rule: an
  existing file wins), and only into a per-server `download/` once CQ3 exists.
- Client Lua bundle prefill into `cache/lua` (join-pipeline §3.5 format), opt-in per server.
- Crowdsourced manifests (MR3), preparing while GMod is closed (MR1 warm start), storage caps
  (CQ5).

### 7. Safety and fallback

- GMod's own download paths are never changed: no proxy, no patch. A wrong or hostile advert can
  only make the pack download Workshop items (Steam-verified content) that the join then ignores.
- The advert is data: decimal IDs only, a capped count (200) and size; a URL must be HTTPS.
- Disk and bandwidth: prepared items are Steam's unsubscribed Workshop cache, exactly what the
  join would have created; GMod's own cache rules apply to them.
- **Consent:** automatic for Meowy servers and servers you've joined before; for others a
  "Get ready · 1.3 GB" button (the home card's row, and the picked server's panel in Find
  servers); never more than 4 GB per server without a click.

## Measuring

Before/after on dev with Workshop items this PC doesn't have, added to dev temporarily:
- **before:** a stock join meets set A (≈ 1 GB) → the Workshop stage's time;
- **after:** the pack reads the advert, prepares set B (≈ 1 GB, similar sizes), then joins.

Same PC, same server, same day; the server's own 29 items stay installed in both runs.

## Milestones

1. **MVP (this PR series):** rules query + advert parse + plan + DownloadUGC prepare for servers
   never joined + row status + `meowy_content_status`; tests for the advert parser and planner.
2. **Meowy server:** the advert from `resource.AddWorkshop` (its own change in the DarkRP repo).
3. Full manifest + FastDL prefetch.
4. Progressive join SDK; Lua bundle prefill.
