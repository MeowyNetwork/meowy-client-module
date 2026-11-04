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
	// Starts creating the browser inside `parent`. False if that can't even start.
	bool Start(HWND parent, const std::wstring& url, const std::wstring& userDataDir);
	// The page loaded and said it's ready; it is now shown over the native drawing.
	bool Ready();
	// Creation or navigation failed; the native drawing stays.
	bool Failed();
	void Resize(int w, int h);
	// Sends a JSON object to the page (window.chrome.webview "message" event).
	void Post(const std::wstring& json);
	// The page's last request: 'c' Cancel, 'h' Game's screen, 'k' hold the
	// fade-out a moment (a game is mid-move), 'f' take keyboard focus; 0 none.
	char TakeAction();
	// Gives the page keyboard focus (after the window itself was activated).
	void Focus();
	// Device pixels per CSS pixel the page is drawn at (the display's scaling),
	// or 0 if WebView2 isn't running.
	double RasterScale();
	// Shows or hides the page (hidden, WebView2 throttles it).
	void SetVisible(bool visible);
	// Reloads the page; Ready() is false until it reports ready again.
	void Reload();
	// Development aid: saves what the page shows as a PNG (asynchronously).
	void Capture(const std::wstring& path);
	void Stop();
}
#endif
