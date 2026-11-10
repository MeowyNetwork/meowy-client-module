#pragma once
// Multicore rendering, native half (src/lua/multicore.lua has the rules).
//
// - Managed: whether this process's menu realm turned gmod_mcore_test on this
//   start and still has it on (the player's setting on, the crash guard
//   allowing it, mat_queue_mode not their own choice). Process-wide, so the
//   client realm (meowy.MulticoreManaged, our servers only) can tell Meowy
//   Network's FPS Booster to leave multicore to the pack. Set only by the
//   menu realm (meowy.SetMulticoreManaged); false until it says otherwise.
// - The crash guard's `running` marker (multicore_guard.txt) is cleared at a
//   clean exit. The menu state has no dependable "the game is closing" hook,
//   so the menu hands over the text to write then (ArmExitWrite), and the
//   pinned DLL writes it as the process exits (module.cpp's DllMain, process
//   detach). A crash doesn't get there; a crash report the game wrote during
//   this run (crashes/*.txt) also stops it, in case the game's crash handler
//   exits normally after writing it.
// - That write is the first signal, not the only one: a marker left behind
//   counts as a crash only when the game's crash handler wrote a report during
//   that session (CrashSince, meowy.CrashSince in the menu). So a quit that
//   ends the process without the exit write is never taken for a crash.

#include <string>

namespace multicore
{
	void SetManaged(bool on);
	bool Managed();

	// What to write to `path` at a clean exit: `text` (the guard's state with
	// `running` cleared). crashRoot: the folder holding crashes\ ("" to skip
	// that check); since: unix time this process started (a report written
	// after it is this run's). Game thread only; called again, the latest wins.
	void ArmExitWrite(const std::wstring& path, const std::string& text, const std::wstring& crashRoot, long long since);
	void Disarm();

	// The crash guard's evidence: whether Garry's Mod's crash handler wrote a
	// report (crashRoot's crashes\*.txt) after `since` and no later than
	// `until` (unix times: the session's start, and this process's start). A
	// marker left behind without one counts as a clean session (the exit write
	// may not have happened). since <= 0 (unknown) or no root: false.
	bool CrashSince(const std::wstring& crashRoot, long long since, long long until);

	// At process exit (DLL_PROCESS_DETACH with the process ending): writes the
	// armed text unless a crash report was written during this run. True when
	// it wrote. Nothing armed: false.
	bool AtProcessExit();
}
