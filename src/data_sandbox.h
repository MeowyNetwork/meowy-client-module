#pragma once
// A separate garrysmod/data for every server (Windows).
//
// Any server's client Lua can list and read all of garrysmod/data: other
// servers' files (their names alone say where the player has played), and
// whatever else is kept there. So garrysmod/data is a directory junction (a
// link) into a vault outside the game's folders: vault\menu at the main menu,
// vault\<a.b.c.d_port> while on that server. Every file path GMod gives Lua
// (DATA, GAME, BASE_PATH...) reaches data/ through the link, so a server only
// ever sees its own folder; the vault itself is out of reach (Windows resolves
// "data\.." in the path text, before it follows the link).
//
// Switching servers rewrites the link, one atomic file-system call. Nothing
// is moved while the game runs, so a file the game holds open (a .gma a
// server mounted from data/, a log it keeps open) can't make a switch fail
// half-way: the open file simply stays in its own server's folder.
//
// The vault: %LOCALAPPDATA%\MeowyClientPack\vault when that's on the game's
// drive, else steamapps\MeowyClientPackVault. Before 1.6.1 the sandbox moved
// the files themselves in and out of data/; Recover() converts that layout
// once, at game start.

#include <string>

namespace data_sandbox
{
	// At game start, before any server: data/ back to the menu's folder (after a
	// crash on a server it still links to that server's). On the first start
	// with this version, data/ (a plain folder) becomes the link: its files go to
	// the vault folder of whoever they belong to, and nothing is ever overwritten
	// (on a clash both are kept). Returns a line for the console when something
	// happened or went wrong, else "".
	std::string Recover();
	// data/ links to the folder for `address` ("1.2.3.4:27015"); a no-op when it
	// already does. Any other non-empty address (a Steam P2P host, something odd)
	// gets a fixed folder per kind ("p2p", "other"), never the menu's and never a
	// name made from the address.
	void Enter(const std::string& address);
	// On a server the pack can't identify: the fixed "other" folder.
	void EnterUnknown();
	// Back to the menu's folder.
	void Leave();
	// For the console and the join log: where data/ links to, or why the
	// sandbox is off.
	std::string Describe();
}
