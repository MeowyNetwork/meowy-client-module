#pragma once

// Fast client Lua file lookup (roadmap 3.1), for any server.
//
// Every include(), CompileFile and SENT/SWEP/effect registration on a server
// looks its file up in the client_lua_files string table once per datapack
// search path. client.dll does each of those lookups as a linear scan of the
// whole table with strcmp: ~31 us per path with ~2,300 files, ~4 ms for a
// file found late or not at all, and a join does ~4,000 of them. Measured
// 2026-11-07 on our server: 16.3 s of a warm join was this scan (gluapack
// doesn't avoid it).
//
// The scan is replaced with the table's own index:
// INetworkStringTable::FindStringIndex (public Source SDK interface, the same
// object the scan already uses), confirmed with the scan's exact strcmp. So a
// lookup returns exactly what the scan would: the entry that equals the name
// byte for byte, or none. Nothing is cached, so nothing can go stale when the
// player switches servers.
//
// It refuses (and leaves the game's own scan in place) unless:
//   * the scan in client.dll matches byte for byte, once, and its compare is
//     a plain strcmp;
//   * the table is an engine.dll CNetworkStringTable (its RTTI says so);
//   * FindStringIndex finds every entry of the table at its own index (checked
//     for each new table before it is used).
// In memory only; nothing on disk changes.

#include <cstddef>
#include <cstdint>
#include <string>

namespace lua_lookup
{
	// Puts the fast lookup in place. Call it on the main thread while no client
	// Lua is starting (the menu state at game start, or between servers).
	// Returns true if it is in place (now or already); otherwise false and `why`
	// says what didn't match.
	bool Install(std::string& why);

	// Puts the game's own scan back. Returns false if it wasn't installed.
	bool Uninstall();

	bool Installed();

	// One line for the status command: on/off, and lookups found / not found /
	// left to the game since the start.
	std::string Describe();

	namespace detail
	{
		// What the patched scan does for one name, with the counts it needs:
		// low 32 bits = index of the entry equal to `name` or -1 (none);
		// high 32 bits = the table's entry count. kLeaveToGame = run the game's
		// own scan instead.
		constexpr int32_t kLeaveToGame = -2;
		int64_t Resolve(void* table, const char* name) noexcept;

		// Tests: accept tables whose vtable lives in this range instead of
		// engine.dll's, and start over (stats, checked tables, refusals).
		void SetEngineRangeForTest(uintptr_t base, size_t size);
		void ResetForTest();

		// The stub the patch jumps to, built for a stub at `stubAt`, from the
		// patch site's bytes (the matched pattern) at `site`. Empty if the site
		// doesn't decode. For the offline disassembly check.
		std::string BuildStubForTest(const uint8_t* site, uintptr_t siteAt, uintptr_t stubAt, uintptr_t resolveAt);

		// Finds the scan in a code section (as loaded, or as read from the file:
		// only relative branches are followed). Returns its offset, or -1 if it
		// isn't there exactly once or its compare isn't strcmp; `why` says which.
		long long FindSite(const uint8_t* text, size_t size, std::string& why);
	}
}
