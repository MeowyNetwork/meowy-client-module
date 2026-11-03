#pragma once

// Reads Garry's Mod's own crash reports (GarrysMod/crashes/*.txt, written by
// the game when it crashes). Lua can't see that folder. Only the
// "-Lua Stack Traces-" section is returned: the rest names the executable
// path, which can contain the player's Windows user name.

#include <string>
#include <vector>

namespace crashes
{
	struct Report
	{
		long long time;        // unix time the report was written
		std::string luaTrace;  // the Lua stack trace section, trimmed
	};

	// Reports written after `since` (unix time), newest first, at most `max`.
	std::vector<Report> Since(long long since, size_t max);
}
