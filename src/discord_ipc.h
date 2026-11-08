#pragma once

// Discord Rich Presence over Discord's local IPC pipe (\\.\pipe\discord-ipc-N).
// Everything that touches the pipe runs on one worker thread, so a slow or
// missing Discord never stalls the game. The game thread only swaps strings
// under a mutex.

#include <string>

namespace discord
{
	// Starts the worker for this Discord application (unless the player turned
	// Discord status off: then it's only remembered). Safe to call again with
	// the same id; a different id reconnects.
	void Start(const std::string& clientId);

	// Queues an activity, as the JSON object Discord expects for
	// SET_ACTIVITY's "activity" field. "null" clears it. Only the latest
	// queued value is sent, at most once per MIN_UPDATE_INTERVAL.
	void SetActivity(const std::string& activityJson);

	// Clears the activity and stops the worker, and forgets that a server asked
	// for it (the client realm closed). Blocks briefly.
	void Shutdown();

	// The player's switch (the menu setting meowy_discord), game thread only.
	// Off: what Discord shows is cleared at once (the worker stops; blocks
	// briefly), and Start and SetActivity only remember what was asked, so
	// nothing reaches Discord. Back on: the status a server asked for since the
	// client realm opened is sent again.
	void SetAllowed(bool on);
	bool Allowed();

	// Whether menu_settings.txt's text (the pack's settings, "name=value" lines,
	// the first line for a name counting) turns Discord status off: a number
	// that is 0, as the menu reads it. For a start without the menu state.
	bool OffInMenuSettings(const std::string& text);

	// "unsupported", "stopped", "connecting" or "ready".
	const char* State();

	// Raw JSON of Discord's last error or close frame, or "" if none.
	std::string LastError();
}
