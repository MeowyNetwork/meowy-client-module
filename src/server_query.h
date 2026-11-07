#pragma once

// Server lists straight from Steam (roadmap U6, N1), through the steam_api
// Garry's Mod already loaded and initialised (never loaded from here):
//   SteamAPI_SteamMatchmakingServers_v002
//   SteamAPI_ISteamMatchmakingServers_Request{Internet,Friends}ServerList
//   SteamAPI_ISteamMatchmakingServers_GetServerDetails / CancelQuery / ReleaseRequest
// Garry's Mod's bundled steam_api exports all of them on both architectures.
//
// What it adds over the menu's serverlist.Query: filters Steam's master server
// applies before anything is pinged (a gamemode category's tag, not full, has
// players, a map, a name), so a filtered list is a fraction of the ~4,000
// servers and arrives in a fraction of the time (Steam pings at most ~5,000 a
// minute), and the friends list. Steam calls back from SteamAPI_RunCallbacks,
// which the game runs every frame on its main thread; Lua takes what has
// arrived with Take, also on the main thread.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace server_query
{
	struct Server
	{
		std::string address;     // a.b.c.d:port (the connection port)
		std::string name, map, gameDir, description, tags;
		int ping = 0, players = 0, maxPlayers = 0, bots = 0;
		bool password = false, secure = false;
		bool anonymous = false;  // an anonymous game server (no Steam account), as the game's list reports
		uint32_t lastPlayed = 0;
		uint64_t steamId = 0;
	};

	enum class List { Internet, Friends };
	using Filters = std::vector<std::pair<std::string, std::string>>;

	// Starts a list for Garry's Mod (app 4000). Returns its id (> 0), or 0 with
	// `why`. Filters are checked against a fixed set of keys (FilterAllowed).
	int Start(List list, const Filters& filters, std::string& why);
	// Moves out the servers that answered since the last Take. `finished` once
	// Steam says the list is complete; `failed` counts servers that didn't answer.
	std::vector<Server> Take(int id, bool* finished, int* failed);
	// Stops a list (and frees it); safe on a finished or unknown id.
	void Cancel(int id);
	// One line for a status command.
	std::string Describe();

	namespace detail
	{
		// Whether `key` is a filter Steam's master server understands that this
		// may pass on, and `value` is short and printable.
		bool FilterAllowed(const std::string& key, const std::string& value);
	}
}
