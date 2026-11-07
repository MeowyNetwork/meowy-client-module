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
		std::string fault;     // "module.dll+0xOFFSET (0xCODE)" from the .dmp, or ""
		std::string faultModule;              // file name only; "" when no module holds the address
		unsigned long long faultOffset = 0;   // offset into faultModule (raw address if none)
		unsigned long faultCode = 0;          // exception code, e.g. 0xC0000005
	};

	// Reports written after `since` (unix time), newest first, at most `max`.
	std::vector<Report> Since(long long since, size_t max);

	// The same, for an explicit game folder (the one holding crashes/). For tests.
	std::vector<Report> SinceIn(const std::wstring& root, long long since, size_t max);

	// `text` with every absolute path ("C:\...", "C:/...", "\\host\...") replaced
	// by "<path>", up to a line end, a quote or the ':' before a line number or
	// message. What leaves the PC in `luaTrace` has been through this.
	std::string MaskPaths(const std::string& text);
}
