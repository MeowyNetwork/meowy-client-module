#pragma once

// What Steam knows about a Workshop item on this PC, through the steam_api
// Garry's Mod already loaded and initialised (never loaded from here):
//   SteamAPI_SteamUGC_v021                 (ISteamUGC*)
//   SteamAPI_ISteamUGC_GetItemState        (EItemState flags)
//   SteamAPI_ISteamUGC_GetItemInstallInfo  (the installed version's time)
// Garry's Mod's bundled steam_api exports all three on both architectures.
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
}
