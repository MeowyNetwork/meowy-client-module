#pragma once
// Our own loading screen for joins to Meowy Network (Windows).
//
// GMod draws its loading screen on the game's main thread, which stops for
// most of a join (the map load and the 20-35 s Lua start-up), so whatever it
// shows freezes. This one is a separate window over the game's, drawn by its
// own thread: it keeps animating, counting and taking clicks while the game
// is busy.
//
// Driven from two Lua realms of the same DLL:
//   menu state  : Begin when a join to our server starts (checked against the
//                 engine's real connection), Status for each load status,
//                 Details from GameDetails, InGame / End when loading stops
//   client state: ClientLuaStarted when require("meowy") runs (the start of the
//                 Lua phase, which the menu can't see because it's frozen),
//                 Done once the player has spawned and frames are smooth
// Nothing here runs unless Begin is called.
#include <string>

namespace loading_screen
{
	// `dataDir` is %LOCALAPPDATA%\MeowyClientPack (for the last join's stage times);
	// `panelOpenMs` is how long GMod's own loading panel was up before this (logged).
	void Begin(const std::wstring& dataDir, double panelOpenMs = -1);
	// At the menu: load the screen's web page now, hidden, so a join can show
	// it straight away instead of the native drawing while it loads.
	void Prewarm(const std::wstring& dataDir);
	// In game, or at a menu left in the background: unload the page (WebView2's
	// processes) once no join is on screen. A join still starts it on its own
	// (the native drawing shows until it's up); Prewarm loads it again.
	void Park();
	// From GameDetails; the SteamID64 lets the web page greet the player.
	void Details(const std::string& serverName, const std::string& map, const std::string& steamid);
	void Status(const std::string& status);
	void ClientLuaStarted();
	// The loading panel closed and the game is in: hold until Done (or a
	// time-out), then fade out.
	void InGame();
	void Done();
	// Loading stopped without getting in game (cancel, kick, failure).
	void End();
	// The Cancel button was clicked; the menu Lua disconnects when it can.
	bool CancelRequested();
	// Adds a line to this join's log (last_join.txt in the pack data folder).
	void Note(const char* what, const std::string& detail);
	bool Active();
}
