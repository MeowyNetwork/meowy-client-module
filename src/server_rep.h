#pragma once

// Honest server list: the native half (the menu Lua in module.cpp does the
// checks and draws the result).
//
//   * The verdicts list from meowynetwork.com/api/pack/servers, fetched on a
//     worker thread. The host and paths are fixed here; Lua only says when.
//   * Reports from inside another server: how many clients the game's own
//     `status` listed, how many were bots, and their SteamIDs (the backend
//     classifies the accounts and keeps only the per-server tally).
//   * Capturing `status` output from the console (connect_watch's listener
//     feeds Capture()).

#include <string>

namespace server_rep
{
	// Starts fetching the list unless a fetch is running. Non-blocking.
	void FetchList();
	// The list's JSON once a fetch finished since the last call, else "".
	std::string TakeList();
	// POSTs a report (JSON built by the menu Lua) on a worker thread.
	void SendReport(const std::string& json);

	// A2S_PLAYER of "a.b.c.d:port" on a worker thread (public IPv4 only).
	// GMod's own serverlist.PlayerList hands back nothing when a reply doesn't
	// add up (e.g. a header saying 86 players over 85 entries), which some
	// servers use to hide their list; this reads what is really there.
	void QueryPlayers(const std::string& address);
	// Finished queries since the last call, as a JSON array:
	// [{"address":..,"ok":bool,"declared":n,"players":[{"name":..,"score":n,"time":s}]}]
	std::string TakePlayers();

	// Console capture for `status`: Begin clears and starts, End returns what
	// was printed in between (at most 64 KB) and stops.
	void CaptureBegin();
	std::string CaptureEnd();
	// Called by connect_watch's logging listener for every console message.
	void Capture(const char* message);
	// A line in %LOCALAPPDATA%\MeowyClientPack\serverlist.log (no personal data).
	void Note(const std::string& line);
}
