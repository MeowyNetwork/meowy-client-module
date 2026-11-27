#pragma once
// The loading screen's web page (Windows): meowynetwork.com/pack-loading.html
// in WebView2, Windows' own Chromium runtime, as a child of the loading screen
// window. Everything here runs on the loading screen's thread, which must be
// COM single-threaded and pump messages. WebView2's processes are separate
// from the game's, so the page costs the game no address space (32-bit).
//
// The page can't leave the origin it was loaded from, can't open windows, and
// has no dev tools, context menu or browser shortcuts. Until it reports ready
// (or if WebView2 is missing, the site is down or it takes too long) the native
// drawing in loading_screen.cpp stays on screen.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

namespace loading_webview
{
	// The page's requests (TakeActions), as bits: several can come between two
	// looks, and each is handled.
	enum : unsigned
	{
		kCancel = 1,   // Cancel
		kHide = 2,     // Game's screen
		kHold = 4,     // hold the fade-out a moment (a game is mid-move)
		kFocus = 8,    // take keyboard focus
	};
	// Starts creating the browser inside `parent`. False if that can't even start.
	// If the browser before it (same profile folder) is still shutting down, the
	// creation waits for it (at most 3 s), made by Poll; true meanwhile.
	bool Start(HWND parent, const std::wstring& url, const std::wstring& userDataDir);
	// Called every turn of the screen's loop: makes a waiting Start's browser
	// once the old one has gone.
	void Poll();
	// A Start is waiting for the old browser to go (Poll must keep being called).
	bool Pending();
	// The page loaded and said it's ready; it is now shown over the native drawing.
	bool Ready();
	// Creation or navigation failed, or the browser's or the page's process
	// died since (also while the page is showing): Stop it and draw natively.
	bool Failed();
	// Sets the page's size; only sent to WebView2 when it differs from the
	// last one (or from what the browser was made at).
	void Resize(int w, int h);
	// Sends a JSON object to the page (window.chrome.webview "message" event).
	void Post(const std::wstring& json);
	// The page's requests since the last call (kCancel | kHide | ...), 0 none,
	// and forgets them. Only a page on screen can make them; Start, Stop and
	// Reload drop any not taken yet.
	unsigned TakeActions();
	// Gives the page keyboard focus (after the window itself was activated).
	void Focus();
	// Device pixels per CSS pixel the page is drawn at (the display's scaling),
	// or 0 if WebView2 isn't running. Kept by WebView2's own event, so cheap.
	double RasterScale();
	// Shows or hides the page (hidden, WebView2 throttles it); only sent when it changes.
	void SetVisible(bool visible);
	// Reloads the page; Ready() is false until it reports ready again.
	void Reload();
	// Development aid: saves what the page shows as a PNG (asynchronously).
	void Capture(const std::wstring& path);
	// Closes the browser. It shuts down on its own; the next Start waits for it.
	void Stop();
}
#endif
