#pragma once
// The one call into Steam for the timeline markers timeline.h lets through.
//
// The pack doesn't ship or start Steam's API: Garry's Mod has already loaded
// its own steam_api64.dll (steam_api.dll on 32-bit) and called SteamAPI_Init.
// This only looks the DLL up (GetModuleHandle, so it is never loaded by us) and
// takes Steam's flat API exports from it:
//   SteamAPI_SteamTimeline_v004                          (ISteamTimeline*)
//   SteamAPI_ISteamTimeline_AddInstantaneousTimelineEvent
//   SteamAPI_ISteamTimeline_SetTimelineGameMode
// Garry's Mod's bundled steam_api (both architectures) exports all three. The
// accessor returns null when the Steam client running is too old for the
// interface; then nothing is sent and meowy.TimelineEvent says
// "timeline unavailable".
//
// Calls run on the game thread, like the game's own Steam calls (steamworks.*
// in Lua): each is a short message to the local Steam client that returns at
// once. A fault inside Steam's code is caught and turns the feature off for
// the session rather than taking the game down.

namespace timeline_steam
{
	// ETimelineGameMode's values (isteamtimeline.h).
	enum class Mode : int
	{
		Playing = 1,
		Staging = 2,
		Menus = 3,
		LoadingScreen = 4,
	};

	// Whether Steam's Timeline interface can be reached right now.
	bool Available();

	// Adds a marker at "now" for kind `index` (timeline::Kinds()), with the
	// pack's own title, description and icon. False if Steam can't be reached.
	bool Add(int index);

	// Colours the timeline's bar: the join's loading as LoadingScreen, the rest
	// as Playing. False if Steam can't be reached.
	bool SetMode(Mode mode);
}
