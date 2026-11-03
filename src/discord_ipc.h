#pragma once

// Discord Rich Presence over Discord's local IPC pipe (\\.\pipe\discord-ipc-N).
// Everything that touches the pipe runs on one worker thread, so a slow or
// missing Discord never stalls the game. The game thread only swaps strings
// under a mutex.

#include <string>

namespace discord
{
	// Starts the worker for this Discord application. Safe to call again with
	// the same id; a different id reconnects.
	void Start(const std::string& clientId);

	// Queues an activity, as the JSON object Discord expects for
	// SET_ACTIVITY's "activity" field. "null" clears it. Only the latest
	// queued value is sent, at most once per MIN_UPDATE_INTERVAL.
	void SetActivity(const std::string& activityJson);

	// Clears the activity and stops the worker. Blocks briefly.
	void Shutdown();

	// "unsupported", "stopped", "connecting" or "ready".
	const char* State();

	// Raw JSON of Discord's last error or close frame, or "" if none.
	std::string LastError();
}
