#include "mouse_confine.h"

#include <atomic>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cwchar>

#include "thread_util.h"
#endif

namespace mouse_confine
{
	namespace detail
	{
		bool ShouldClip(bool on, bool gameInFront, bool minimised, bool hung, bool cursorShowing)
		{
			return on && gameInFront && !minimised && !hung && !cursorShowing;
		}
	}

	namespace
	{
		std::atomic<bool> g_on{ false };
		std::atomic<bool> g_started{ false };
		std::atomic<bool> g_clipping{ false };
		std::atomic<unsigned> g_clips{ 0 };   // times a clip was put on
	}

#ifdef _WIN32
	namespace
	{
		// The game's own window: this process's, top-level, Source's window class.
		bool GameWindow(HWND hwnd)
		{
			if (!hwnd)
				return false;
			DWORD pid = 0;
			GetWindowThreadProcessId(hwnd, &pid);
			if (pid != GetCurrentProcessId() || GetAncestor(hwnd, GA_ROOT) != hwnd)
				return false;
			wchar_t cls[32];
			return GetClassNameW(hwnd, cls, 32) > 0 && std::wcscmp(cls, L"Valve001") == 0;
		}

		void Release()
		{
			if (g_clipping.exchange(false))
				ClipCursor(nullptr);
		}

		// Signalled by Set: the thread sleeps on it while the feature is off.
		HANDLE g_changed = nullptr;

		void Watch()
		{
			DWORD wait = 25;
			for (;;)
			{
				// Off: nothing to watch until Set changes that (no wake-ups at all).
				// With another window in front, Windows has dropped any clip itself,
				// so a slower look is enough; with the game in front, every 25 ms.
				if (!g_on.load())
				{
					Release();
					WaitForSingleObject(g_changed, INFINITE);
					continue;
				}
				Sleep(wait);
				HWND front = GetForegroundWindow();
				const bool game = GameWindow(front);
				wait = game ? 25 : 250;
				CURSORINFO ci = {};
				ci.cbSize = sizeof ci;
				// Hidden: not showing, or showing no cursor at all (the game sets none while you aim).
				const bool showing = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor != nullptr;
				if (!detail::ShouldClip(g_on.load(), game, game && IsIconic(front), game && IsHungAppWindow(front), showing))
				{
					Release();
					continue;
				}
				RECT client;
				if (!GetClientRect(front, &client) || client.right <= client.left || client.bottom <= client.top)
				{
					Release();
					continue;
				}
				MapWindowPoints(front, nullptr, reinterpret_cast<POINT*>(&client), 2);
				RECT now;
				// Re-applied when it's gone or different (the window moved, or Windows
				// dropped it when something else came to the front for a moment).
				if (!g_clipping.load() || !GetClipCursor(&now) || !EqualRect(&now, &client))
				{
					if (ClipCursor(&client))
					{
						if (!g_clipping.exchange(true))
							++g_clips;
					}
				}
			}
		}

		// The module unloading (the game closing): never leave the cursor clipped.
		struct ReleaseAtUnload
		{
			~ReleaseAtUnload() { Release(); }
		} g_releaseAtUnload;
	}

	void Set(bool on)
	{
		g_on = on;
		if (!on)
			Release();   // also from here, at once; the thread agrees within 25 ms
		if (on && !g_started.exchange(true))
		{
			// A thread that couldn't start is tried again on the next Set(true).
			if (!g_changed)
				g_changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!g_changed || !thread_util::Spawn(L"Meowy mouse", Watch, thread_util::Priority::BelowNormal))
				g_started = false;
		}
		if (g_changed)
			SetEvent(g_changed);
	}
#else
	void Set(bool on) { g_on = on; }
#endif

	std::string Describe()
	{
		if (!g_on.load())
			return "off";
		return std::string(g_clipping.load() ? "on, holding the mouse now" : "on") + "; held " + std::to_string(g_clips.load()) + " times";
	}
}
