#pragma once

// Small, opt-in fixes to the running game. Each one finds the code it changes
// by an exact byte pattern, refuses to touch anything that doesn't match, and
// only ever changes the game in memory (no files on disk).

#include <string>

namespace engine_patches
{
	// At the end of client Lua start-up, GMod throws away every loaded material
	// and loads them all again (IMaterialSystem::UncacheAllMaterials, then
	// CacheUsedMaterials). On a big map that is 6-17 s of every join, for
	// materials that were loaded seconds earlier during the map load.
	//
	// This keeps the "cache used materials" call, so anything new still loads,
	// and skips only the "uncache all" before it. It must run before client
	// Lua start-up finishes, i.e. from a client autorun file.
	//
	// Returns true if the patch is in place (now or already); otherwise false
	// and `why` says what didn't match.
	bool SkipMaterialReload(std::string& why);

	// Puts the original bytes back. Returns false if the patch wasn't applied.
	bool RestoreMaterialReload();

	// "patched", "not patched" or "unsupported".
	const char* MaterialReloadState();
}
