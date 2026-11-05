#include "notify_toast.h"

#ifndef _WIN32

namespace notify_toast
{
	void Configure(const std::wstring&) {}
	bool Post(int) { return false; }
	bool GameFocused() { return true; }
	void Shutdown() {}
	void SetTargetWindowForTest(void*) {}
	void ForceToastPathForTest(bool) {}
	bool ShortcutHasAppId(const std::wstring&) { return false; }
	std::string LastResult() { return "unsupported"; }
}

#else

#include "notify.h"
#include "resource.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propidl.h>
#include <propsys.h>

#include <roapi.h>
#include <windows.data.xml.dom.h>
#include <windows.foundation.h>
#include <windows.ui.notifications.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <mutex>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
namespace WinNotify = ABI::Windows::UI::Notifications;
namespace WinXml = ABI::Windows::Data::Xml::Dom;
namespace WinFoundation = ABI::Windows::Foundation;

namespace
{
	// System.AppUserModel.ID, without linking propsys for PKEY_AppUserModel_ID.
	const PROPERTYKEY kAppIdKey = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5 };

	constexpr UINT WM_TRAY = WM_APP + 1;        // the tray icon's callback
	constexpr UINT WM_CLICKED = WM_APP + 2;     // a toast was clicked (from a WinRT thread)
	constexpr UINT_PTR TIMER_TRAY = 1;
	constexpr UINT kTrayTimeoutMs = 30 * 1000;  // the tray icon goes after this at the latest
	constexpr DWORD kShutdownWaitMs = 2000;
	constexpr size_t kKeepToasts = 16;          // shown toasts whose click handler stays registered
	constexpr long long kToastLifetime = 30LL * 60 * 10000000; // 30 minutes, in 100 ns units

	// Shared between the game thread and the worker; guarded by g_mutex.
	std::mutex g_mutex;
	std::deque<int> g_queue;
	std::wstring g_dataDir;
	std::string g_lastResult = "nothing yet";

	// The running worker's own stop flag and wake event: a worker that didn't
	// stop in time keeps its own (leaked) copy, so a new one can't confuse it.
	struct Control
	{
		HANDLE wake = nullptr;
		std::atomic<bool> stop{ false };
	};
	HANDLE g_thread = nullptr;     // game thread only
	Control* g_control = nullptr;  // game thread only

	std::atomic<HWND> g_window{ nullptr };      // the worker's hidden window
	std::atomic<HWND> g_testTarget{ nullptr };
	std::atomic<bool> g_forceToast{ false };

	HMODULE Self()
	{
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&Self), &self);
		return self;
	}

	void SetResult(const std::string& s)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_lastResult = s;
	}

	void AddResult(const char* s)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_lastResult.size() < 512)
			g_lastResult += s;
	}

	std::string Hr(const char* what, HRESULT hr)
	{
		char buf[96];
		std::snprintf(buf, sizeof(buf), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
		return buf;
	}

	BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM out)
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd))
			return TRUE;
		wchar_t cls[64];
		if (GetClassNameW(hwnd, cls, 64) && std::wcscmp(cls, L"Valve001") == 0)
		{
			*reinterpret_cast<HWND*>(out) = hwnd;
			return FALSE;
		}
		return TRUE;
	}

	HWND Target()
	{
		if (HWND test = g_testTarget.load())
			return test;
		HWND game = nullptr;
		EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&game));
		return game;
	}

	bool TargetInFront()
	{
		HWND fg = GetForegroundWindow();
		if (!fg)
			return false;
		if (HWND test = g_testTarget.load())
			return fg == test;
		DWORD pid = 0;
		GetWindowThreadProcessId(fg, &pid);
		return pid == GetCurrentProcessId();
	}

	// Any thread. Only asynchronous calls into the game's window, so this never
	// waits on the game thread (which may itself be waiting in Shutdown).
	void BringToFront()
	{
		HWND target = Target();
		if (!target)
			return;
		if (IsIconic(target))
			ShowWindowAsync(target, SW_RESTORE);
		if (SetForegroundWindow(target))
			return;
		// Windows lets a click on our toast hand us the foreground; if it
		// didn't, share input with the window in front for one call.
		HWND fg = GetForegroundWindow();
		DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
		DWORD me = GetCurrentThreadId();
		bool attached = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
		BringWindowToTop(target);
		SetForegroundWindow(target);
		if (attached)
			AttachThreadInput(me, fgThread, FALSE);
	}

	std::wstring Utf8ToWide(const char* s)
	{
		int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
		if (n <= 0)
			return std::wstring();
		std::wstring out(static_cast<size_t>(n - 1), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
		return out;
	}

	void AppendXmlEscaped(std::wstring& out, const wchar_t* s)
	{
		for (; *s; ++s)
		{
			switch (*s)
			{
			case L'&': out += L"&amp;"; break;
			case L'<': out += L"&lt;"; break;
			case L'>': out += L"&gt;"; break;
			case L'"': out += L"&quot;"; break;
			case L'\'': out += L"&apos;"; break;
			default: out += *s; break;
			}
		}
	}

	// file:///C:/Users/.../notify_icon.png, percent-encoded (user names can
	// have spaces, '#' or non-ASCII letters).
	std::wstring FileUri(const std::wstring& path)
	{
		int n = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (n <= 0)
			return std::wstring();
		std::string utf8(static_cast<size_t>(n - 1), '\0');
		WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, &utf8[0], n, nullptr, nullptr);
		std::wstring uri = L"file:///";
		for (unsigned char c : utf8)
		{
			if (c == '\\')
				uri += L'/';
			else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
				c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c == ':')
				uri += static_cast<wchar_t>(c);
			else
			{
				wchar_t buf[4];
				std::swprintf(buf, 4, L"%%%02X", c);
				uri += buf;
			}
		}
		return uri;
	}

	// --- Worker-thread state ---------------------------------------------------

	struct ShownToast
	{
		ComPtr<WinNotify::IToastNotification> toast;
		EventRegistrationToken token;
	};

	struct Worker
	{
		HWND window = nullptr;
		HICON smallIcon = nullptr, largeIcon = nullptr;
		ComPtr<ITaskbarList3> taskbar;
		HWND badged = nullptr;          // the window carrying our badge, if any
		bool trayAdded = false;
		int shortcut = -1;              // -1 not checked yet, 0 missing, 1 carries our AUMID
		std::wstring iconUri;           // the toast's logo, or empty
		ComPtr<WinNotify::IToastNotifier> notifier;
		std::vector<ShownToast> toasts;
	};
	Worker* g_worker = nullptr;          // worker thread only

	// Does the Start menu have our shortcut, carrying our AUMID? Without it,
	// Windows takes a toast and silently drops it.
	bool ShortcutRegistered()
	{
		PWSTR programs = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs)))
			return false;
		std::wstring path = std::wstring(programs) + L"\\" + notify::kShortcutName;
		CoTaskMemFree(programs);
		return notify_toast::ShortcutHasAppId(path);
	}
}

namespace notify_toast
{
	bool ShortcutHasAppId(const std::wstring& path)
	{
		if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
			return false;

		ComPtr<IShellLinkW> link;
		ComPtr<IPersistFile> file;
		ComPtr<IPropertyStore> store;
		if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
			FAILED(link.As(&file)) || FAILED(file->Load(path.c_str(), STGM_READ)) || FAILED(link.As(&store)))
			return false;
		PROPVARIANT value;
		PropVariantInit(&value);
		bool ok = SUCCEEDED(store->GetValue(kAppIdKey, &value)) && value.vt == VT_LPWSTR && value.pwszVal &&
			std::wcscmp(value.pwszVal, notify::kAppId) == 0;
		PropVariantClear(&value);
		return ok;
	}
}

namespace
{
	// The toast's logo, from the DLL's resources into the pack's folder.
	std::wstring WriteIcon()
	{
		std::wstring dir;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			dir = g_dataDir;
		}
		if (dir.empty())
			return std::wstring();
		HMODULE self = Self();
		HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(IDR_NOTIFY_PNG), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
		HGLOBAL data = res ? LoadResource(self, res) : nullptr;
		const void* bytes = data ? LockResource(data) : nullptr;
		DWORD size = res ? SizeofResource(self, res) : 0;
		if (!bytes || !size)
			return std::wstring();

		std::wstring path = dir + L"\\notify_icon.png";
		WIN32_FILE_ATTRIBUTE_DATA attr;
		bool current = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr) &&
			attr.nFileSizeHigh == 0 && attr.nFileSizeLow == size;
		if (!current)
		{
			HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE)
				return std::wstring();
			DWORD written = 0;
			BOOL ok = WriteFile(h, bytes, size, &written, nullptr);
			CloseHandle(h);
			if (!ok || written != size)
				return std::wstring();
		}
		return FileUri(path);
	}

	void RemoveTray()
	{
		Worker& w = *g_worker;
		if (!w.trayAdded)
			return;
		NOTIFYICONDATAW nid = {};
		nid.cbSize = sizeof(nid);
		nid.hWnd = w.window;
		nid.uID = 1;
		Shell_NotifyIconW(NIM_DELETE, &nid);
		KillTimer(w.window, TIMER_TRAY);
		w.trayAdded = false;
	}

	void ClearBadge()
	{
		Worker& w = *g_worker;
		if (!w.badged)
			return;
		if (w.taskbar)
			w.taskbar->SetOverlayIcon(w.badged, nullptr, nullptr);
		FLASHWINFO fi = { sizeof(fi), w.badged, FLASHW_STOP, 0, 0 };
		FlashWindowEx(&fi);
		w.badged = nullptr;
	}

	void Badge(HWND target)
	{
		Worker& w = *g_worker;
		if (!w.taskbar)
		{
			ComPtr<ITaskbarList3> taskbar;
			if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar))) &&
				SUCCEEDED(taskbar->HrInit()))
				w.taskbar = taskbar;
		}
		if (w.taskbar && w.smallIcon)
			w.taskbar->SetOverlayIcon(target, w.smallIcon, L"Meowy Network notification");
		// Flashes the taskbar button until the window comes to the front.
		FLASHWINFO fi = { sizeof(fi), target, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0 };
		FlashWindowEx(&fi);
		w.badged = target;
	}

	// The fallback: a balloon from a tray icon of our own, removed when the
	// balloon goes, is clicked, or after kTrayTimeoutMs.
	bool ShowBalloon(const notify::Kind& kind)
	{
		Worker& w = *g_worker;
		NOTIFYICONDATAW nid = {};
		nid.cbSize = sizeof(nid);
		nid.hWnd = w.window;
		nid.uID = 1;
		nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_INFO | NIF_SHOWTIP;
		nid.uCallbackMessage = WM_TRAY;
		nid.hIcon = w.smallIcon;
		wcsncpy_s(nid.szTip, notify::kTitle, _TRUNCATE);
		wcsncpy_s(nid.szInfoTitle, notify::kTitle, _TRUNCATE);
		wcsncpy_s(nid.szInfo, kind.text, _TRUNCATE);
		nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
		nid.hBalloonIcon = w.largeIcon;
		bool ok;
		if (w.trayAdded)
			ok = Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
		else
		{
			ok = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
			if (ok)
			{
				nid.uVersion = NOTIFYICON_VERSION_4;
				Shell_NotifyIconW(NIM_SETVERSION, &nid);
				w.trayAdded = true;
			}
		}
		if (ok)
			SetTimer(w.window, TIMER_TRAY, kTrayTimeoutMs, nullptr);
		return ok;
	}

	// The toast. S_FALSE: Windows has notifications from us turned off (the
	// player's choice, so no balloon either).
	HRESULT ShowToast(int index, const notify::Kind& kind)
	{
		Worker& w = *g_worker;
		HRESULT hr;
		if (!w.notifier)
		{
			ComPtr<WinNotify::IToastNotificationManagerStatics> manager;
			hr = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(),
				IID_PPV_ARGS(&manager));
			if (FAILED(hr))
				return hr;
			hr = manager->CreateToastNotifierWithId(HStringReference(notify::kAppId).Get(), &w.notifier);
			if (FAILED(hr))
				return hr;
		}
		WinNotify::NotificationSetting setting;
		if (SUCCEEDED(w.notifier->get_Setting(&setting)) && setting != WinNotify::NotificationSetting_Enabled)
			return S_FALSE;

		std::wstring xml = L"<toast activationType=\"foreground\" launch=\"meowy\"><visual><binding template=\"ToastGeneric\"><text hint-maxLines=\"1\">";
		AppendXmlEscaped(xml, notify::kTitle);
		xml += L"</text><text>";
		AppendXmlEscaped(xml, kind.text);
		xml += L"</text>";
		if (!w.iconUri.empty())
		{
			xml += L"<image placement=\"appLogoOverride\" src=\"";
			AppendXmlEscaped(xml, w.iconUri.c_str());
			xml += L"\"/>";
		}
		xml += L"</binding></visual></toast>";

		ComPtr<IInspectable> inspectable;
		hr = RoActivateInstance(HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(), &inspectable);
		if (FAILED(hr))
			return hr;
		ComPtr<WinXml::IXmlDocumentIO> io;
		ComPtr<WinXml::IXmlDocument> doc;
		if (FAILED(hr = inspectable.As(&io)) || FAILED(hr = inspectable.As(&doc)))
			return hr;
		hr = io->LoadXml(HStringReference(xml.c_str(), static_cast<unsigned int>(xml.size())).Get());
		if (FAILED(hr))
			return hr;

		ComPtr<WinNotify::IToastNotificationFactory> factory;
		hr = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotification).Get(),
			IID_PPV_ARGS(&factory));
		if (FAILED(hr))
			return hr;
		ComPtr<WinNotify::IToastNotification> toast;
		hr = factory->CreateToastNotification(doc.Get(), &toast);
		if (FAILED(hr))
			return hr;

		// One toast per kind in the notification centre: a newer one replaces it.
		ComPtr<WinNotify::IToastNotification2> toast2;
		if (SUCCEEDED(toast.As(&toast2)))
		{
			std::wstring tag = Utf8ToWide(kind.id);
			if (tag.size() > 16)   // Windows' limit for a tag (Windows 10 1607+ allows 64)
				tag = L"k" + std::to_wstring(index);
			toast2->put_Tag(HStringReference(tag.c_str(), static_cast<unsigned int>(tag.size())).Get());
			toast2->put_Group(HStringReference(L"meowy").Get());
		}

		// Gone from the notification centre after half an hour: by then it's old news.
		ComPtr<WinFoundation::IPropertyValueStatics> values;
		if (SUCCEEDED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Foundation_PropertyValue).Get(),
			IID_PPV_ARGS(&values))))
		{
			FILETIME now;
			GetSystemTimeAsFileTime(&now);
			WinFoundation::DateTime expires;
			expires.UniversalTime = static_cast<INT64>((static_cast<unsigned long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime) +
				kToastLifetime;
			ComPtr<IInspectable> boxed;
			ComPtr<WinFoundation::IReference<WinFoundation::DateTime>> ref;
			if (SUCCEEDED(values->CreateDateTime(expires, &boxed)) && SUCCEEDED(boxed.As(&ref)))
				toast->put_ExpirationTime(ref.Get());
		}

		// A click brings the game forward. The handler is agile (WRL's Callback
		// is free-threaded): it runs on a WinRT thread and only posts to us.
		EventRegistrationToken token = {};
		hr = toast->add_Activated(Callback<WinFoundation::ITypedEventHandler<WinNotify::ToastNotification*, IInspectable*>>(
			[](WinNotify::IToastNotification*, IInspectable*) -> HRESULT
			{
				BringToFront();
				if (HWND window = g_window.load())
					PostMessageW(window, WM_CLICKED, 0, 0);
				return S_OK;
			}).Get(), &token);
		if (FAILED(hr))
			return hr;

		hr = w.notifier->Show(toast.Get());
		if (FAILED(hr))
		{
			toast->remove_Activated(token);
			return hr;
		}
		if (w.toasts.size() >= kKeepToasts)
		{
			w.toasts.front().toast->remove_Activated(w.toasts.front().token);
			w.toasts.erase(w.toasts.begin());
		}
		w.toasts.push_back({ toast, token });
		return S_OK;
	}

	void Show(int index)
	{
		size_t count = 0;
		const notify::Kind* kinds = notify::Kinds(&count);
		if (index < 0 || static_cast<size_t>(index) >= count)
			return;
		const notify::Kind& kind = kinds[index];
		// Checked again here: the player may be back in the game by now.
		if (TargetInFront())
		{
			SetResult("skipped: focused");
			return;
		}

		Worker& w = *g_worker;
		if (w.shortcut < 0)
			w.shortcut = ShortcutRegistered() || g_forceToast.load() ? 1 : 0;
		std::string result;
		bool shown = false;
		if (w.shortcut == 1)
		{
			HRESULT hr = ShowToast(index, kind);
			if (hr == S_OK)
			{
				result = "toast";
				shown = true;
			}
			else if (hr == S_FALSE)
			{
				result = "toast suppressed by Windows settings";
				shown = true;
			}
			else
				result = Hr("toast", hr) + ", ";
		}
		else
			result = "no Start-menu shortcut with our AppUserModelID, ";
		if (!shown)
			result += ShowBalloon(kind) ? "balloon" : "balloon failed";

		if (HWND target = Target())
			Badge(target);
		SetResult(result);
	}

	LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		switch (msg)
		{
		case WM_TRAY:
			// NOTIFYICON_VERSION_4: the event is in LOWORD(lParam).
			switch (LOWORD(lp))
			{
			case NIN_BALLOONUSERCLICK:
			case NIN_SELECT:
			case NIN_KEYSELECT:
				BringToFront();
				AddResult("; balloon clicked");
				if (g_worker) { RemoveTray(); ClearBadge(); }
				break;
			case NIN_BALLOONTIMEOUT:
				AddResult("; balloon gone");
				if (g_worker) RemoveTray();
				break;
			}
			return 0;
		case WM_CLICKED:
			AddResult("; toast clicked");
			if (g_worker) ClearBadge();
			return 0;
		case WM_TIMER:
			if (wp == TIMER_TRAY && g_worker)
				RemoveTray();
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	const wchar_t* const kWindowClass = L"MeowyClientPackNotify";

	DWORD WINAPI WorkerMain(void* param)
	{
		Control* control = static_cast<Control*>(param);
		// STA, with the message loop below: the taskbar and shell-link objects
		// are apartment-threaded. Toast click handlers come in on WinRT threads.
		HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		Worker w;
		g_worker = &w;

		HMODULE self = Self();
		w.smallIcon = static_cast<HICON>(LoadImageW(self, MAKEINTRESOURCEW(IDI_MEOWY), IMAGE_ICON,
			GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
		w.largeIcon = static_cast<HICON>(LoadImageW(self, MAKEINTRESOURCEW(IDI_MEOWY), IMAGE_ICON,
			GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
		WNDCLASSEXW wc = { sizeof(wc) };
		wc.lpfnWndProc = WindowProc;
		wc.hInstance = self;
		wc.lpszClassName = kWindowClass;
		RegisterClassExW(&wc);
		// A hidden ordinary window (not message-only): the tray needs one to own its icon.
		w.window = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"Meowy Client Pack notifications", WS_POPUP,
			0, 0, 0, 0, nullptr, nullptr, self, nullptr);
		g_window = w.window;
		try
		{
			w.iconUri = WriteIcon();
		}
		catch (...)
		{
		}

		while (!control->stop.load())
		{
			DWORD wait = MsgWaitForMultipleObjects(1, &control->wake, FALSE, w.badged ? 250 : INFINITE, QS_ALLINPUT);
			(void)wait;
			if (control->stop.load())
				break;

			std::deque<int> todo;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				todo.swap(g_queue);
			}
			for (int index : todo)
			{
				try
				{
					Show(index);
				}
				catch (...)   // out of memory: drop this one, never take the game down
				{
				}
			}

			MSG msg;
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}

			if (w.badged && TargetInFront())
				ClearBadge();
		}

		// Clean up everything that could call back into this DLL or outlive the game.
		for (ShownToast& t : w.toasts)
			t.toast->remove_Activated(t.token);
		w.toasts.clear();
		w.notifier.Reset();
		RemoveTray();
		ClearBadge();
		w.taskbar.Reset();
		g_window = nullptr;
		if (w.window)
			DestroyWindow(w.window);
		UnregisterClassW(kWindowClass, self);
		if (w.smallIcon) DestroyIcon(w.smallIcon);
		if (w.largeIcon) DestroyIcon(w.largeIcon);
		g_worker = nullptr;
		if (SUCCEEDED(co))
			CoUninitialize();
		return 0;
	}
}

namespace notify_toast
{
	void Configure(const std::wstring& dataDir)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_dataDir = dataDir;
	}

	bool Post(int index)
	{
		if (!g_thread)
		{
			Control* control = new Control();
			control->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!control->wake)
			{
				delete control;
				return false;
			}
			HANDLE thread = CreateThread(nullptr, 0, WorkerMain, control, 0, nullptr);
			if (!thread)
			{
				CloseHandle(control->wake);
				delete control;
				return false;
			}
			g_thread = thread;
			g_control = control;
		}
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (g_queue.size() < 32)
				g_queue.push_back(index);
		}
		SetEvent(g_control->wake);
		return true;
	}

	bool GameFocused()
	{
		return TargetInFront();
	}

	void Shutdown()
	{
		if (!g_thread)
			return;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_queue.clear();
		}
		g_control->stop = true;
		SetEvent(g_control->wake);
		if (WaitForSingleObject(g_thread, kShutdownWaitMs) == WAIT_OBJECT_0)
		{
			CloseHandle(g_control->wake);
			delete g_control;
		}
		// else: the worker is stuck somewhere in Windows. It keeps its own
		// Control (leaked on purpose) and stops when it gets back.
		CloseHandle(g_thread);
		g_thread = nullptr;
		g_control = nullptr;
	}

	void SetTargetWindowForTest(void* hwnd)
	{
		g_testTarget = static_cast<HWND>(hwnd);
	}

	void ForceToastPathForTest(bool force)
	{
		g_forceToast = force;
	}

	std::string LastResult()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_lastResult;
	}
}

#endif
