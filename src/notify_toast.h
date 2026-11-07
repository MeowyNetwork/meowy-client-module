#pragma once
// Shows the notifications notify.h lets through (Windows): a Windows toast,
// a badge on the game's taskbar button and a flashing button until the game
// is in front again.
//
// Everything runs on one worker thread (with its own COM apartment), so the
// game thread only appends a kind index to a queue. The worker starts with the
// first notification and stops with Shutdown. The worker holds its own
// reference to this DLL until its last instruction, and once a toast's click
// handler has been registered the DLL is pinned for the rest of the game, so
// the client realm's FreeLibrary never unloads code that still runs.
//
//  - Toast: a WinRT ToastNotification under the AppUserModelID
//    MeowyNetwork.ClientPack (notify.h kAppId). An unpackaged program can only
//    show toasts under an AUMID that a Start-menu shortcut carries; the
//    installer makes "Meowy Client Pack" for that. Title "Meowy Network", the
//    kind's text from notify.cpp, and the pack's own icon (written next to the
//    pack's other files; toasts from unpackaged programs can't load web
//    images). Clicking it brings the game to the front, through an in-process
//    Activated handler (so no COM server is registered). Windows' Do Not
//    Disturb and the player's per-app setting apply as to any toast.
//  - Without the shortcut, or if WinRT fails: a balloon from a temporary
//    tray icon (Windows 10 and 11 show it as a toast), removed afterwards.
//  - Taskbar: ITaskbarList3::SetOverlayIcon and FlashWindowEx on the game's
//    window, both cleared once the game is in front again.

#include <string>

namespace notify_toast
{
	// Where the toast's icon is written (%LOCALAPPDATA%\MeowyClientPack). Call
	// before the first Post.
	void Configure(const std::wstring& dataDir);

	// Queues the notification for kind `index` (notify::Kinds()); starts the
	// worker if needed. False if the worker couldn't be started.
	bool Post(int index);

	// Whether the foreground window belongs to this process (the game, or one
	// of the pack's own windows over it).
	bool GameFocused();

	// Clears the badge, removes the tray icon and the toasts' click handlers,
	// and stops the worker. Waits at most ~2 s; never hangs the game. A worker
	// still stuck in Windows after that finishes on its own later, touching
	// only its own state (a newer worker started by the next Post is separate).
	void Shutdown();

	// Whether the shortcut at `path` carries our AppUserModelID (the worker
	// checks the Start menu's "Meowy Client Pack" with this; COM must be
	// initialised on the calling thread).
	bool ShortcutHasAppId(const std::wstring& path);

	// For tests/notify_manual.cpp: the window to badge and bring forward
	// instead of the game's (nullptr: find the game's window).
	void SetTargetWindowForTest(void* hwnd);
	// For tests/notify_manual.cpp: try the WinRT toast even without the
	// Start-menu shortcut (to exercise that code; Windows may drop the toast).
	void ForceToastPathForTest(bool force);

	// The last thing the worker did, for the manual test and the console:
	// "toast", "toast suppressed by Windows settings", "balloon", "skipped:
	// focused" or an error with its HRESULT.
	std::string LastResult();
}
