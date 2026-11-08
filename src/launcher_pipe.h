#pragma once
// The pack's end of the Meowy Launcher's pipe (launcher/DESIGN.md §10).
//
// The launcher (a tray app outside the game) listens on
//   \\.\pipe\meowy-launcher-<the user's SID>
// open to that user only and to local clients only. In the menu state the
// pack connects to it from a thread of its own, every 5 s while it isn't
// there, and only keeps the connection when the pipe's server process is the
// installed launcher (%LOCALAPPDATA%\MeowyClientPack\launcher\MeowyLauncher.exe).
//
// Lines of JSON, LF-ended, at most 512 bytes each:
//   pack -> launcher   {"t":"hello","v":"1.7.0"}
//                      {"t":"state","s":"menu"}
//                      {"t":"state","s":"joining"|"ingame","a":"a.b.c.d:port"}
//   launcher -> pack   {"t":"join","a":"a.b.c.d:port"}
// Nothing else is ever taken from the launcher: no Lua, no console commands,
// no passwords. A join must be exactly that line, with an address that is ours
// or public; the menu Lua then runs `connect` with it. The address in a state
// line is the engine's own (server_gate), never anything a server's Lua set.
//
// One thread does all of the pipe's I/O (writes, and PeekNamedPipe before
// each read), so a read never blocks a write.

#include <string>

namespace launcher_pipe
{
	// Starts the thread (once). `version`: the pack's version for hello.
	void Start(const char* version);

	// The menu Lua's view: "menu", "joining" or "ingame", with the engine's
	// own address (server_gate::Check, on the main thread). Sent when it
	// changes, and again after a reconnect.
	void SetState(const std::string& state, const std::string& address);

	// A join the launcher asked for and this side checked, or "" (taken once).
	std::string TakeJoin();

	// Whether the launcher is connected now.
	bool Connected();

	// The menu Lua that drives it, run with the pack's other menu chunks.
	extern const char* const kLua;

	namespace detail
	{
		// `line` (no LF) is exactly {"t":"join","a":"<address>"} with a public
		// or our address: true and the address.
		bool ParseJoin(const std::string& line, std::string& address);
		// The state line for `state` and `address` ("" for none); "" if the
		// state isn't one of the three.
		std::string StateLine(const std::string& state, const std::string& address);
		std::string HelloLine(const std::string& version);
		// Whether `image` (a full path) is `expected`, ignoring case and / vs \.
		bool SamePath(const std::wstring& image, const std::wstring& expected);
		// Whether two paths name the same existing file (volume and file ID), so
		// a profile behind a junction matches the real path Windows reports.
		bool SameFile(const std::wstring& a, const std::wstring& b);
	}
}
