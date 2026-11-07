#pragma once
// Keep the mouse inside the game while you're aiming (Windows).
//
// Source re-centres the cursor each frame while you look around, but a fast
// flick can carry it past the window's edge between frames. In windowed or
// borderless mode, or onto a second monitor even in fullscreen, a click in
// that moment lands on another window and the game drops to the background.
// The engine never calls ClipCursor (no import in any of its DLLs).
//
// A thread of its own checks every 25 ms: while the game's window is in front,
// not minimised or hung, and the cursor is hidden (mouse-look), the cursor is
// clipped to the window's client area. Any other moment (a menu, the console
// or the context menu shows the cursor; alt-tab; the Windows key; another
// window in front), it's released at once. Measured on Windows 11: a clip
// outlives the process that set it, but Windows drops it whenever another
// window comes to the front (a crash dialog included); it's also released
// when the module unloads.
#include <string>

namespace mouse_confine
{
	// Starts the thread on the first call; on/off.
	void Set(bool on);
	std::string Describe();

	namespace detail
	{
		bool ShouldClip(bool on, bool gameInFront, bool minimised, bool hung, bool cursorShowing);
	}
}
