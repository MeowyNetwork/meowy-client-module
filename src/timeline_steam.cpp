#include "timeline_steam.h"
#include "timeline.h"

#include <cstdint>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
#ifdef _WIN32
	// Steam's flat API is __cdecl (S_CALLTYPE) on 32-bit; on 64-bit there's
	// only one convention. ISteamTimeline* is opaque here.
	using GetTimelineFn = void* (__cdecl*)();
	using AddEventFn = uint64_t (__cdecl*)(void* self, const char* title, const char* description,
		const char* icon, uint32_t priority, float startOffsetSeconds, int possibleClip);
	using SetModeFn = void (__cdecl*)(void* self, int mode);

#ifdef _WIN64
	constexpr const wchar_t* kSteamApi = L"steam_api64.dll";
#else
	constexpr const wchar_t* kSteamApi = L"steam_api.dll";
#endif

	GetTimelineFn g_get = nullptr;
	AddEventFn g_add = nullptr;
	SetModeFn g_mode = nullptr;
	bool g_resolved = false;   // the three exports were found
	bool g_broken = false;     // Steam's code faulted once: off for this session

	// Finds the exports once Garry's Mod's steam_api is in the process (it
	// always is by the time Lua runs; never loaded from here).
	bool Resolve()
	{
		if (g_broken)
			return false;
		if (g_resolved)
			return true;
		HMODULE steam = GetModuleHandleW(kSteamApi);
		if (!steam)
			return false;
		g_get = reinterpret_cast<GetTimelineFn>(GetProcAddress(steam, "SteamAPI_SteamTimeline_v004"));
		g_add = reinterpret_cast<AddEventFn>(GetProcAddress(steam, "SteamAPI_ISteamTimeline_AddInstantaneousTimelineEvent"));
		g_mode = reinterpret_cast<SetModeFn>(GetProcAddress(steam, "SteamAPI_ISteamTimeline_SetTimelineGameMode"));
		if (!g_get || !g_add || !g_mode)
		{
			// An older steam_api: these won't appear later in this session.
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

	// The interface, or null (no Steam client support, or a fault). No C++
	// objects in these: they use structured exception handling.
	void* GetTimeline()
	{
		__try
		{
			return g_get();
		}
		__except (OnFault(GetExceptionCode()))
		{
			g_broken = true;
			return nullptr;
		}
	}

	bool CallAdd(void* self, const timeline::Kind& k)
	{
		__try
		{
			g_add(self, k.title, k.description, k.icon, k.priority, 0.0f, static_cast<int>(k.clip));
			return true;
		}
		__except (OnFault(GetExceptionCode()))
		{
			g_broken = true;
			return false;
		}
	}

	bool CallSetMode(void* self, int mode)
	{
		__try
		{
			g_mode(self, mode);
			return true;
		}
		__except (OnFault(GetExceptionCode()))
		{
			g_broken = true;
			return false;
		}
	}

	void* Timeline()
	{
		if (!Resolve())
			return nullptr;
		// steam_api caches the interface itself; asking each time costs a lookup.
		return GetTimeline();
	}
#endif
}

namespace timeline_steam
{
#ifdef _WIN32
	bool Available()
	{
		return Timeline() != nullptr;
	}

	bool Add(int index)
	{
		size_t count = 0;
		const timeline::Kind* kinds = timeline::Kinds(&count);
		if (index < 0 || static_cast<size_t>(index) >= count)
			return false;
		void* self = Timeline();
		return self && CallAdd(self, kinds[index]);
	}

	bool SetMode(Mode mode)
	{
		void* self = Timeline();
		return self && CallSetMode(self, static_cast<int>(mode));
	}
#else
	bool Available() { return false; }
	bool Add(int) { return false; }
	bool SetMode(Mode) { return false; }
#endif
}
