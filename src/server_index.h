#pragma once

// Meowy's server index (roadmap MR3, docs/server-index-and-parties.md): every
// Garry's Mod server Steam lists, refreshed each minute by the backend, so
// Find servers fills in about a second instead of waiting ~20 s for Steam's
// pings. One HTTPS GET of https://meowynetwork.com/api/pack/index on a worker
// thread: gzip, If-None-Match (a 304 when nothing changed), no redirects, a
// 4 MB cap on the unpacked body. menu_page.lua decodes and checks every row;
// nothing here trusts the body beyond its size.
//
// Ping hints are kept here too, because garrysmod/data and the game's cookies
// can be read by any server's Lua: the pings this PC measured, per server
// network, live in %LOCALAPPDATA%\MeowyClientPack\pings.json.

#include <string>

namespace server_index
{
	// Starts a fetch unless one is running. False when one is.
	bool Fetch();

	struct Result
	{
		bool ready = false;      // a fetch finished since the last Take
		std::string status;      // "ok", "same" (304: the last body still holds) or "error: ..."
		std::string body;        // the JSON for "ok"
		std::string geo;         // X-Meowy-Geo: "lat,lon" of this PC's address, or ""
		double seconds = 0;      // how long the fetch took
	};
	// The finished fetch, once; ready = false while one runs or none was started.
	Result Take();

	// One line for a status command.
	std::string Describe();

	// The ping memory: the whole file (JSON), "" when there is none.
	std::string LoadPings(const std::wstring& dataDir);
	// Replaces it. False for anything but a JSON object of at most 256 KB.
	bool SavePings(const std::wstring& dataDir, const std::string& json);
}
