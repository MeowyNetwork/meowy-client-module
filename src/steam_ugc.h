#pragma once

// What Steam knows about a Workshop item on this PC, through the steam_api
// Garry's Mod already loaded and initialised (never loaded from here):
//   SteamAPI_SteamUGC_v021                 (ISteamUGC*)
//   SteamAPI_ISteamUGC_GetItemState        (EItemState flags)
//   SteamAPI_ISteamUGC_GetItemInstallInfo  (the installed version's time)
//   SteamAPI_ISteamUGC_GetItemDownloadInfo (a download's progress; optional)
// Garry's Mod's bundled steam_api exports all of them on both architectures.
// Read-only: nothing here asks Steam to download or change anything. Used by
// the join pre-warm to ask Steam only for the items that need it (home.lua).

#include <cstdint>

namespace steam_ugc
{
	struct ItemState
	{
		bool installed = false;     // k_EItemStateInstalled
		bool needsUpdate = false;   // k_EItemStateNeedsUpdate
		bool busy = false;          // k_EItemStateDownloading or DownloadPending
		bool legacy = false;        // k_EItemStateLegacyItem: the game unpacks it to cache\workshop
		uint32_t updated = 0;       // when the installed version was published (unix time), 0 if not installed
	};

	// False when Steam's Workshop interface can't be reached (an old steam_api,
	// no Steam, or a fault in Steam's code, after which it stays off).
	bool State(uint64_t id, ItemState& out);

	// How far an item's download has got (SteamAPI_ISteamUGC_GetItemDownloadInfo):
	// bytes so far and in all. False when Steam has no download for it (or can't
	// be reached). Read-only, for the content platform's progress line.
	bool Progress(uint64_t id, uint64_t& done, uint64_t& total);
}
