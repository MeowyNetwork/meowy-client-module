#pragma once

// The console's lines, for the pack's console window (roadmap U8, N6). Hooked
// into tier0's output the documented ways, whichever this build exports:
//   LoggingSystem_RegisterLoggingListener  (a listener; Source's logging system)
//   GetSpewOutputFunc + SpewOutputFunc     (Source 2013's spew function, chained:
//                                           the previous one still runs first-hand)
// Lines are assembled from the pieces Msg/MsgC send (a line ends at "\n"), kept
// with the time, severity (0 message, 1 warning, 2 assert, 3 error) and colour,
// in a ring of the last kMaxLines, and no more than kMaxTextBytes of text in
// all (a server printing long lines in a loop can't fill a 32-bit client's
// memory). Text is cleaned to valid UTF-8 (a server name cut mid-character is
// common). Thread-safe: the engine logs from several threads; the menu reads.
// The lock is a critical section, never freed: a thread killed while holding
// it as the game exits doesn't hang the exit (a slim reader/writer lock would).

#include <cstdint>
#include <string>
#include <vector>

namespace console_capture
{
	constexpr size_t kMaxLines = 50000;
	constexpr size_t kMaxLineBytes = 4096;
	// The oldest lines go first once the text kept passes this.
	constexpr size_t kMaxTextBytes = sizeof(void*) == 8 ? 16 * 1024 * 1024 : 6 * 1024 * 1024;

	struct Line
	{
		uint64_t id = 0;         // 1, 2, 3...: never reused
		int64_t timeMs = 0;      // unix time in milliseconds
		int severity = 0;
		uint32_t color = 0;      // 0xRRGGBB, or 0xFFFFFFFF when none was given
		std::string text;
	};

	// Hooks tier0 once (later calls report the same). False, with `why`, if
	// neither entry point is exported.
	bool Install(std::string& why);
	bool Installed();

	// Lines after `sinceId`, at most `max` (the newest when there are more);
	// `nextId` is the id the next line will get. With `max` 0, no lines: only
	// `nextId` (the newest line is nextId - 1).
	std::vector<Line> Read(uint64_t sinceId, size_t max, uint64_t* nextId);

	std::string Describe();

	// Runs a line as if typed into the console: IVEngineClient015's
	// ClientCmd_Unrestricted (slot 106 in Source SDK 2013's cdll_int.h, the
	// layout whose slot 72, GetNetChannelInfo, server_gate already relies on).
	// The menu's RunConsoleCommand refuses some commands (echo, for one); the
	// console doesn't. Main thread only. False if the engine can't be reached.
	bool RunLine(const char* line);

	namespace detail
	{
		// What the hooks call: a piece of output (no newline needed).
		void Feed(int severity, const char* text, uint32_t color);
		// Forget everything (tests).
		void Reset();
		// `s` with every byte that isn't part of valid UTF-8 replaced by U+FFFD.
		std::string CleanUtf8(const std::string& s);
	}
}
