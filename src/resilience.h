#pragma once
// Connection resilience, native half (Windows). The rules are Lua
// (src/lua/resilience_logic.lua, tested offline); this keeps the pack's own
// files and our server's word that it's about to restart. The engine's view of
// the connection itself comes from server_gate (Check, NetHealth).
//
// - "Restart expected": set from the client realm by meowy.ExpectRestart while
//   connected to our server (gated like every other call). If the connection
//   ends before it runs out, the menu treats it as a restart: wait for the
//   server to answer, then rejoin. Memory only; the DLL is shared by both realms.
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
	// Our server says it restarts within `seconds` (clamped to 30..600).
	// Returns the seconds used.
	int ExpectRestart(double seconds);
	// Whether that is still in force (not used and not run out).
	bool RestartExpected();
	// Used, or the player left: forget it.
	void ClearRestart();

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
