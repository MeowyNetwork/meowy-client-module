#include "steam_ugc.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
#ifdef _WIN32
	// Steam's flat API is __cdecl (S_CALLTYPE) on 32-bit; on 64-bit there's
	// only one convention. ISteamUGC* is opaque here.
	using GetUgcFn = void* (__cdecl*)();
	using GetItemStateFn = uint32_t (__cdecl*)(void* self, uint64_t id);
	using GetInstallInfoFn = bool (__cdecl*)(void* self, uint64_t id, uint64_t* sizeOnDisk, char* folder,
		uint32_t folderSize, uint32_t* timeStamp);

#ifdef _WIN64
	constexpr const wchar_t* kSteamApi = L"steam_api64.dll";
#else
	constexpr const wchar_t* kSteamApi = L"steam_api.dll";
#endif

	// EItemState (isteamugc.h)
	constexpr uint32_t kLegacyItem = 2, kInstalled = 4, kNeedsUpdate = 8, kDownloading = 16, kDownloadPending = 32;

	GetUgcFn g_get = nullptr;
	GetItemStateFn g_state = nullptr;
	GetInstallInfoFn g_info = nullptr;
	bool g_resolved = false;
	bool g_broken = false;   // missing exports or a fault: off for this session

	bool Resolve()
	{
		if (g_broken)
			return false;
		if (g_resolved)
			return true;
		HMODULE steam = GetModuleHandleW(kSteamApi);
		if (!steam)
			return false;
		g_get = reinterpret_cast<GetUgcFn>(GetProcAddress(steam, "SteamAPI_SteamUGC_v021"));
		g_state = reinterpret_cast<GetItemStateFn>(GetProcAddress(steam, "SteamAPI_ISteamUGC_GetItemState"));
		g_info = reinterpret_cast<GetInstallInfoFn>(GetProcAddress(steam, "SteamAPI_ISteamUGC_GetItemInstallInfo"));
		if (!g_get || !g_state || !g_info)
		{
			g_broken = true;
			return false;
		}
		g_resolved = true;
		return true;
	}

	int OnFault(unsigned long code)
	{
		return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
	}

	// No C++ objects in these: they use structured exception handling.
	bool Query(uint64_t id, uint32_t* flags, uint32_t* stamp, bool* haveInfo)
	{
		__try
		{
			void* ugc = g_get();
			if (!ugc)
				return false;
			*flags = g_state(ugc, id);
			char folder[MAX_PATH];
			uint64_t size = 0;
			*haveInfo = (*flags & kInstalled) && g_info(ugc, id, &size, folder, sizeof(folder), stamp);
			return true;
		}
		__except (OnFault(GetExceptionCode()))
		{
			g_broken = true;
			return false;
		}
	}
#endif
}

namespace steam_ugc
{
	bool State(uint64_t id, ItemState& out)
	{
		out = ItemState();
#ifdef _WIN32
		if (!id || !Resolve())
			return false;
		uint32_t flags = 0, stamp = 0;
		bool haveInfo = false;
		if (!Query(id, &flags, &stamp, &haveInfo))
			return false;
		out.installed = (flags & kInstalled) != 0;
		out.needsUpdate = (flags & kNeedsUpdate) != 0;
		out.busy = (flags & (kDownloading | kDownloadPending)) != 0;
		out.legacy = (flags & kLegacyItem) != 0;
		out.updated = haveInfo ? stamp : 0;
		return true;
#else
		(void)id;
		return false;
#endif
	}
}
