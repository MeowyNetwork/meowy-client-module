#pragma once

// Fast file.Find on the client's "LUA" path, for any server.
//
// While a server's client Lua starts, addon loaders call file.Find(..., "LUA")
// a few hundred times. For each datapack search path, client.dll walks the
// whole client_lua_files string table: it copies every entry into a new
// std::string (a heap allocation for nearly every path), tests it against the
// wildcard pattern (search path + the Lua pattern), and frees it. Measured
// 2026-11-07 on our server: 12.6-14 ms per call, even for a folder that
// doesn't exist (file.Find on "GAME": 2.9 ms).
//
// An entry can only pass that wildcard test if it starts with the pattern's
// literal part (everything before the first '*' or '?'): the game's matcher
// compares those bytes one for one, case-sensitively, and fails at the first
// difference or if the entry ends first. So the loop is patched to go straight
// to the next entry that starts with that literal part (from a sorted copy of
// the table's entries), in the same order, and the game's own code still does
// everything else for each one. The result is exactly the game's own.
//
// It refuses (and leaves the game's own loop in place) unless:
//   * the loop's start, its step, and its wildcard test are found in
//     client.dll byte for byte, once each, and agree with each other;
//   * the test's matcher is the expected one (literal bytes compared one for
//     one, '*' and '?' as wildcards);
//   * the table is an engine.dll CNetworkStringTable (its RTTI says so).
// The sorted copy follows the table the way lua_lookup does: a table is known
// by its address, its first entry's pointer and its count, entries added since
// are read and merged in, and every entry is compared with the copy again only
// when that identity changes or a join (or changelevel) starts: NewJoin, which
// the menu calls at every loading start, since a map change makes a new table
// that can land at the old one's address with the same count. The copy is made
// again when an entry differs. In memory only.

#include <cstddef>
#include <cstdint>
#include <string>

namespace lua_find
{
	// Puts the patch in place. Call it on the main thread while no client Lua
	// is starting (the menu state at game start, or between servers). Returns
	// true if it is in place (now or already); otherwise false and `why` says
	// what didn't match.
	bool Install(std::string& why);

	// Puts the game's own loop back. Returns false if it wasn't installed.
	bool Uninstall();

	bool Installed();

	// A join or changelevel is starting: the next table's entries are all
	// checked against the copy again before it's used.
	void NewJoin();

	// One line for the status command: on/off, finds, and table entries skipped.
	std::string Describe();

	namespace detail
	{
		// The candidate the patched loop goes to next: low 32 bits = the first
		// index after `after` whose entry starts with the literal part of
		// `pattern`, or -1 (no more); high 32 bits = the table's entry count.
		// kLeaveToGame = run the game's own loop instead.
		constexpr int32_t kLeaveToGame = -2;
		int64_t Next(void* table, const char* pattern, int after) noexcept;

		// The literal part of a wildcard pattern: up to the first '*' or '?'.
		std::string LiteralPart(const char* pattern);

		// Tests: accept tables whose vtable lives in this range instead of
		// engine.dll's, and start over (index, stats, refusals).
		void SetEngineRangeForTest(uintptr_t base, size_t size);
		void ResetForTest();
		// Table entries read so far to check or (re)build the copy.
		long long CheckedForTest();

		// Finds the loop in a code section (as loaded, or as read from the file:
		// only relative branches are followed). Returns the start's offset (and
		// the step's in `step`), or -1 if anything isn't there exactly once or
		// doesn't agree; `why` says what.
		long long FindSites(const uint8_t* text, size_t size, size_t& step, std::string& why);

		// The two stubs for sites found by FindSites, built to run at `stubAt`.
		// Empty if the sites don't decode. For the offline disassembly check.
		std::string BuildStubsForTest(const uint8_t* text, size_t size, uintptr_t textAt, size_t start, size_t step,
			uintptr_t stubAt, uintptr_t nextAt);
	}
}
