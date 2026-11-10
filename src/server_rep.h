#pragma once

// Honest server list: the native half (the menu Lua in module.cpp does the
// checks and draws the result).
//
//   * The verdicts list from meowynetwork.com/api/pack/servers, fetched on a
//     worker thread. The host and paths are fixed here; Lua only says when.
//   * Reports from inside another server: how many clients the game's own
//     `status` listed, how many were bots, and their SteamIDs (the backend
//     classifies the accounts and keeps only the per-server tally), with a
//     Steam ticket for Meowy Network so it counts once per account
//     (steam_ticket.h).
//   * Capturing `status` output from the console (connect_watch's listener
//     feeds Capture()).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace server_rep
{
	// Starts fetching the list unless a fetch is running. Non-blocking. (The
	// HTTPS requests go through net_http's shared session.)
	void FetchList();
	// The list's JSON once a fetch finished since the last call, else "".
	std::string TakeList();
	// POSTs a report (JSON built by the menu Lua) on a worker thread, with a
	// Steam ticket added when Steam gives one within 10 s. Game thread (it asks
	// Steam for the ticket).
	void SendReport(const std::string& json);

	// "a.b.c.d:port" on the public internet (not LAN, loopback, CGNAT or
	// multicast): the only servers the pack queries or reports on.
	bool IsPublicAddress(const std::string& address);

	// A2S_PLAYER of "a.b.c.d:port" on a worker thread (public IPv4 only).
	// GMod's own serverlist.PlayerList hands back nothing when a reply doesn't
	// add up (e.g. a header saying 86 players over 85 entries), which some
	// servers use to hide their list; this reads what is really there.
	// False when not asked (too many in flight, not a public address, or no
	// thread could be started).
	bool QueryPlayers(const std::string& address);

	// One finished A2S_PLAYER query.
	struct PlayerResult
	{
		std::string address;
		bool ok = false;         // a player list came back
		bool answered = false;   // the server replied at all (ok false and answered: it hides its list)
		int declared = 0;        // the count the reply's header gives
		bool truncated = false;  // more than 255 entries: the rest were dropped
		struct P
		{
			std::string name;    // at most 64 bytes before cleaning, valid UTF-8 (bad bytes became U+FFFD)
			int score = 0;
			double time = 0;     // seconds connected, 0 when the reply's value was nonsense
		};
		std::vector<P> players;  // at most 255
	};
	// Finished queries since the last call (Take: each result is handed out once,
	// by this or by TakePlayers).
	std::vector<PlayerResult> TakePlayerResults();
	// The same, as a JSON array (until the menu Lua uses TakePlayerResults):
	// [{"address":..,"ok":bool,"declared":n,"players":[{"name":..,"score":n,"time":s}]}]
	// (ok false: "answered" says whether the server replied at all)
	std::string TakePlayers();

	// One A2S request of `kind` ('V' rules, 'U' players) to a public
	// "a.b.c.d:port", with the challenge rounds newer servers ask for: the
	// reply with split packets joined, or "" (`answered` says whether the server
	// replied at all). Blocks for up to ~8 s: call it on a worker thread, after
	// WSAStartup.
	std::string Exchange(const std::string& address, char kind, bool& answered);

	// Console capture for `status`: Begin clears and starts, End returns what
	// was printed in between (at most 64 KB) and stops.
	void CaptureBegin();
	std::string CaptureEnd();
	// Called by connect_watch's logging listener for every console message.
	void Capture(const char* message);
	// A line in %LOCALAPPDATA%\MeowyClientPack\serverlist.log (no personal data).
	void Note(const std::string& line);

	namespace detail
	{
		// Joins one A2S reply's split packets (Source's format: -2, a 4-byte
		// split ID, the part count, the part's number, a 2-byte size). The first
		// part sets the reply's split ID and part count; parts with another ID
		// or count (a late reply to an earlier ask, while the asking is repeated)
		// are dropped, so two replies are never mixed.
		class SplitJoin
		{
		public:
			static constexpr int kMaxParts = 32;
			// One datagram: 1 the reply is whole (in `out`; a single -1 packet is
			// whole at once), 0 more parts needed (or this one was dropped), -1
			// nonsense (give up).
			int Add(const char* data, size_t n, std::string& out);

		private:
			std::string m_parts[kMaxParts];
			int32_t m_id = 0;
			int m_total = 0, m_have = 0;
		};

		// An A2S_PLAYER reply (split packets joined, "" for none) as a result:
		// at most 255 players, names cut to 64 bytes and cleaned to UTF-8, times
		// that aren't numbers or are out of range 0. `answered`: the server
		// replied at all.
		PlayerResult ParsePlayers(const std::string& address, const std::string& reply, bool answered);
		// One result as TakePlayers' JSON object.
		std::string PlayersJson(const PlayerResult& r);
	}
}
