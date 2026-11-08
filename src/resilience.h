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
//   the game is connected to a server, ours or a public one (the address from
//   the engine's own connection, server_gate). Still there at the next start
//   means the game ended on that server; with a crash report written since,
//   the menu offers a rejoin, and on ours the report is noted as Meowy
//   Network's (meowy_crashes.txt), the only kind ever sent to it.

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

	// resilience_net.cpp. Is our server up? One A2S_INFO query over plain UDP
	// (not through Steam, which may be offline after the same drop), on its own
	// thread. StartProbe refuses anything but our addresses.
	bool StartProbe(const std::string& address);
	// 0 still waiting (or none asked), 1 answered, -1 no answer. `locked`: it
	// says it has a password.
	int ProbeResult(bool& locked);
	// Steam's own "logged on" (ISteamUser::BLoggedOn): 1, 0, or -1 unknown.
	int SteamOnline();

	// resilience.txt in `dir` ("" if missing); writes at most 512 bytes.
	std::string ReadSettings(const std::wstring& dir);
	bool WriteSettings(const std::wstring& dir, const std::string& text);

	// on_server.txt: the server the player is on (the engine's address, which
	// the caller has checked is ours or a public one) and its name, or removed.
	void MarkSession(const std::wstring& dir, const std::string& address, const std::string& name);
	void ClearSession(const std::wstring& dir);
	// Once per start: the address (and name) in a leftover on_server.txt when the
	// game wrote a crash report after it (and before this start); "" otherwise.
	// Removes the file. The caller checks the address again before offering
	// anything. When the address is ours, those reports are noted as Meowy
	// Network's (NoteOurCrashes). crashRoot: the folder holding crashes\ (tests;
	// the game's own by default).
	std::string CrashedSession(const std::wstring& dir, std::string* name = nullptr, const std::wstring* crashRoot = nullptr);

	// meowy_crashes.txt in `dir`: the file names of crash reports written while
	// the game was on one of our servers, the newest 50. A report can't say
	// which server the game was on, and one from another server names that
	// server's Lua files: only these are ever sent to Meowy Network
	// (meowy.CrashReports).
	void NoteOurCrashes(const std::wstring& dir, const std::vector<std::string>& names);
	// `names` that are noted as ours (order kept).
	std::vector<std::string> OurCrashes(const std::wstring& dir, const std::vector<std::string>& names);

	// last_server.txt: the last server the player got into the game on (same
	// format and checks as on_server.txt), for the main menu's "Continue". Kept
	// until the next one, or until the player forgets it.
	void MarkLastServer(const std::wstring& dir, const std::string& address, const std::string& name);
	// Its address ("" if none), name and unix time. The caller checks the address
	// again before offering it.
	std::string LastServer(const std::wstring& dir, std::string* name = nullptr, long long* since = nullptr);
	void ForgetLastServer(const std::wstring& dir);

	// A server name to keep in on_server.txt: no control characters or line
	// breaks, at most 64 bytes, cut at a UTF-8 character boundary.
	std::string CleanName(const std::string& name);
}
