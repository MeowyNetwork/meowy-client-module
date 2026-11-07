#pragma once

// A faster font pass on every join, for any server (roadmap startup #7).
//
// At every map load Garry's Mod registers every custom font file again:
// client.dll's AddCustomFonts calls ISurface::AddCustomFontFile for ~400
// files. For each one, vguimatsurface loads gdi32.dll with LoadLibraryA
// (through the Steam overlay's LoadLibraryA hook), looks up
// AddFontResourceExA, registers the file and frees gdi32 again. gdi32 is
// loaded the whole time, so every load/free pair is pure overhead. Measured
// 2026-11-07: the pass took 739-803 ms per join, 384-386 ms without the loads.
//
// In vguimatsurface's import table only (data, not code), LoadLibraryA of
// "gdi32.dll" returns the gdi32 that is already loaded, and the matching
// FreeLibrary is skipped. Every other load and free, and every font
// registration, goes ahead exactly as before.

#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace font_loads
{
	// Hooks vguimatsurface.dll's imports. Main thread, between servers.
	// Returns true if in place (now or already); otherwise false and `why`.
	bool Install(std::string& why);

	// Puts vguimatsurface's imports back. Returns false if not installed, or
	// while a gdi32 handle it handed out hasn't been "freed" yet.
	bool Uninstall();

	// One line for the status command: loads saved, and the last font pass.
	std::string Describe();

	namespace detail
	{
#ifdef _WIN32
		// The same, for any module's imports (tests hook their own).
		bool InstallFor(HMODULE module, std::string& why);
#endif
	}
}
