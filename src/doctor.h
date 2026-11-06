#pragma once
// PC doctor: read-only facts about this PC that Lua can't see, for the
// settings window's "PC doctor" tab and meowy_doctor (menu state only).
// Nothing here changes a Windows or game setting, and nothing leaves the PC.
//
// - Graphics adapters (DXGI), in Windows' own order of "high performance"
//   and "power saving" (IDXGIFactory6::EnumAdapterByGpuPreference, the same
//   classification as Windows' Graphics settings page).
// - Which adapter this process renders on: the GPU time the kernel has
//   charged to this process on each adapter's engines (D3DKMTQueryStatistics,
//   D3DKMT_QUERYSTATISTICS_PROCESS_NODE: the numbers Task Manager's per-process
//   "GPU engine" column comes from). The adapter with most of it is the one
//   drawing the game. Adapter LUIDs stay in here; Lua only gets names.
// - Power: on battery, battery saver (GetSystemPowerStatus) and Windows'
//   effective power mode (PowerRegisterForEffectivePowerModeNotifications).

#include <string>
#include <vector>

namespace doctor
{
	struct Adapter
	{
		std::string name;          // UTF-8, e.g. "NVIDIA GeForce RTX 3050 Laptop GPU"
		unsigned long long vramMb = 0;
		int perfRank = 0;          // 1 = Windows' first "high performance" choice; 0 = unknown
		int powerRank = 0;         // 1 = Windows' first "power saving" choice; 0 = unknown
		double share = -1;         // this process's share of GPU time on it (0..1), -1 unknown
	};

	struct Facts
	{
		std::vector<Adapter> adapters;
		bool gpuTimeKnown = false;   // the per-process GPU time could be read
		int onBattery = -1;          // 1, 0, or -1 unknown
		int batterySaver = -1;       // 1, 0, or -1 unknown
		std::string powerMode;       // "battery_saver", "better_battery", "balanced",
		                             // "high_performance", "max_performance", "game_mode",
		                             // "mixed_reality", or "" unknown
		std::string arch;            // "x64" or "x86": this game build
	};

	Facts Gather();

	// The few pages the doctor may open, by id: "graphics_settings"
	// (ms-settings:display-advancedgraphics) and "branch_guide" (Facepunch's
	// wiki on switching branches). Anything else is refused.
	bool Open(const std::string& id);
}
