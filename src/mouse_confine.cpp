#include "mouse_confine.h"

#include <atomic>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cwchar>
#include <thread>
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

		void Watch()
		{
			for (;;)
			{
				Sleep(25);
				HWND front = GetForegroundWindow();
				const bool game = GameWindow(front);
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
			std::thread(Watch).detach();
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
