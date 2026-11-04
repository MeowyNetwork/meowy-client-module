#pragma once
// A separate garrysmod/data for every server (Windows).
//
// Any server's client Lua can list and read all of garrysmod/data: other
// servers' files (their names alone say where the player has played), and
// whatever else is kept there. While the player is on a server, data/ holds
// only that server's own files; everything else waits in a vault outside the
// game's folders (out of reach of every file path GMod gives Lua, BASE_PATH
// included). At the main menu, data/ holds the menu's own files.
//
// Moves are renames on one drive, so a switch is instant whatever the size.
// The vault's "active" file says whose files are in data/ right now, so a
// crash mid-session is put right at the next start (Recover).

#include <string>

namespace data_sandbox
{
	// At start-up: if the game went down while on a server, put that server's
	// files back in the vault and the menu's in data/.
	void Recover();
	// Make data/ belong to `address` ("1.2.3.4:27015"); no-op if it already does.
	void Enter(const std::string& address);
	// Back to the menu's files.
	void Leave();
	// For the join log: whose files are in data/, and the vault folder.
	std::string Describe();
}
