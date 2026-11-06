#pragma once
// Connection resilience, native half (Windows). The rules are Lua
// (src/lua/resilience_logic.lua, tested offline); this only gathers what the
// engine says and keeps the pack's own files.
//
// - The engine's console lines (from connect_watch's tier0 logging listener),
//   the last few seconds of them, so the menu can read why a connection ended:
//   "Server connection timed out." or "Disconnect: <reason>.". Kept in memory only.
// - %LOCALAPPDATA%\MeowyClientPack\resilience.txt: whether the player was asked,
//   their answer, and the cl_timeout they had before (key=value lines).
// - %LOCALAPPDATA%\MeowyClientPack\on_server.txt: "<address> <unix time>" while
//   the game is connected to one of our servers (the address from the engine's
//   own connection, server_gate). Still there at the next start means the game
//   ended on our server; with a crash report written since, the menu offers a rejoin.

#include <string>
#include <vector>

namespace resilience
{
	// From the logging listener (any thread).
	void OnConsoleLine(const char* message);
	// Lines printed in the last `withinMs`, oldest first.
	std::vector<std::string> RecentLines(unsigned withinMs);

	// resilience.txt in `dir` ("" if missing); writes at most 512 bytes.
	std::string ReadSettings(const std::wstring& dir);
	bool WriteSettings(const std::wstring& dir, const std::string& text);

	// on_server.txt: written with our server's address, or removed.
	void MarkSession(const std::wstring& dir, const std::string& address);
	void ClearSession(const std::wstring& dir);
	// Once per start: the address in a leftover on_server.txt when it's one of
	// ours and the game wrote a crash report after it; "" otherwise. Removes the file.
	std::string CrashedSession(const std::wstring& dir);
}
