# The Meowy Network observer

If you run a Garry's Mod server and found a player that turned out to be us, this page is for you.

## What it is

Some servers show more players in the server list than are really playing: bots made to look like
people, or accounts that join and idle around the clock. The Meowy Client Pack's server list marks
those servers, so players can see the real count before they join. Most of that comes from public
data (the server list itself, and what each server answers to standard queries). When those numbers
look suspicious, the observer, a normal Garry's Mod client on our own machine, joins the server
for about ten minutes and counts how many players show any sign of a person.

It's the same idea as a mystery shopper: it doesn't announce itself in game, because a server
that fakes its players could otherwise act differently while it's there. It isn't secret, though.
This page says who it is, so anyone who wants to can check.

## How to recognise it

| | |
|---|---|
| Address it connects from | `138.201.62.52` |
| Steam account(s) | _to be filled in by Meowy Network_ |

## When it visits

- Only servers the public checks flagged, or that we queued by hand to check a report.
- At most once every three hours per server, for about ten minutes, plus its download time.
- A server that refuses it (a kick, a VPN or region block, a lobby-only server) isn't visited
  again for a day. A refusal about our address counts for every server on that machine.

## What it does on your server

- It joins as a player and stands still. It doesn't chat, move, buy or use anything.
- On most servers it runs a small Lua script in its own game that counts, every five seconds:
  - players who moved or turned (it can only see those near it, as with any player);
  - players whose ping, score, DarkRP job or money changed, or who chatted or spoke.

  This is everything any client receives.
- If your server's anticheat stops that script, later visits run no code in your server's Lua at
  all. They run the game's `status` command, which gives the scoreboard: pings and connection times.
  They also read the player positions the engine already holds for its view, from its own module.
- It downloads your content like any player, with low texture quality, and skips models,
  materials and sounds unless your map needs them.

## What it keeps

Counts only. For each visit: how many players were there, how many it could see, how many stood
still, how many showed any activity, and how many kept a flat ping under 15 ms the whole visit.
No names, SteamIDs, chat, or positions leave the observer's machine. While a visit runs, the
console log, which holds the `status` tables, stays on that machine, and it is cleared before the next visit.

A server is marked only on clear evidence: at least 6 players, and at least 30% of those it could
judge, either standing still for ten minutes or idle on a flat ping that never moved. What the
observer measures is in this repository's `observer/`; the thresholds are in Meowy Network's
website backend.

## Opting out

On [meowynetwork.com/observer](https://meowynetwork.com/observer): enter your server's address, put
the code it gives you in your server's name for a moment, and press check. Only whoever runs a
server can change its name, so that's the proof it's yours. We keep only the address and the
date. From then on the observer never visits or queues it, even by hand, and you can opt back in
the same way. One address at a time: many servers share a machine. Your server can still be
marked from public data and from reports by players who use the Client Pack.

## Source

Everything the observer runs is in this repository's `observer/` folder: the game module
(`src/`), the Lua it runs (`lua/`), the supervisor that takes visits from our backend
(`supervisor/`), and the machine it runs on (`vm/`).
