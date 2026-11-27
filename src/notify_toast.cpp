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
#include <utility>
#include <memory>
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
	constexpr DWORD kShutdownWaitMs = 150;    // Shutdown, on the game thread; a slower worker is let go (WorkerMain)
	constexpr size_t kMaxQueued = 32;
	constexpr size_t kKeepToasts = 16;          // shown toasts whose click handler stays registered
	constexpr long long kToastLifetime = 30LL * 60 * 10000000; // 30 minutes, in 100 ns units

	// One worker's state that others can reach: the game thread (its queue, its
	// stop flag) and the toasts' click handlers (its window). Reference-counted,
	// so whoever lets go last frees it: a worker that outlives Shutdown, or a
	// click that comes in after the worker is gone, only ever sees its own
	// worker's state, never freed memory or a newer worker's.
	struct Shared
	{
		HANDLE wake = nullptr;
		std::atomic<bool> stop{ false };
		std::atomic<HWND> window{ nullptr };   // the worker's hidden window, while it exists
		// Guarded by g_mutex. The Kind is taken on the game thread when queued
		// (notify::Kinds() is game-thread only); its strings are never freed.
		std::deque<std::pair<int, const notify::Kind*>> queue;

		Shared() = default;
		Shared(const Shared&) = delete;
		Shared& operator=(const Shared&) = delete;
		~Shared()
		{
			if (wake)
				CloseHandle(wake);
		}
	};

	std::mutex g_mutex;   // every Shared::queue, g_dataDir and g_lastResult
	std::wstring g_dataDir;
	std::string g_lastResult = "nothing yet";

	HANDLE g_thread = nullptr;              // game thread only: the current worker
	std::shared_ptr<Shared> g_shared;       // game thread only: the current worker's

	std::atomic<HWND> g_testTarget{ nullptr };
	std::atomic<bool> g_forceToast{ false };

	HMODULE Self()
	{
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&Self), &self);
		return self;
	}

	// Once a toast's click handler is registered, Windows can call into this
	// DLL whenever the toast is clicked, also after the client realm that
	// showed it has closed (and FreeLibrary'd the DLL) and after its handler was
	// removed, if the click was already on its way. So from then on the DLL
	// stays loaded for the rest of the game (the menu realm pins it anyway when
	// the pack starts with the game).
	bool PinSelf()
	{
		static std::atomic<bool> pinned{ false };
		if (pinned.load())
			return true;
		HMODULE self = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			reinterpret_cast<LPCWSTR>(&PinSelf), &self))
			return false;
		pinned = true;
		return true;
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

	// Any thread, and never waits on the game's thread, which may be busy
	// loading or blocked in Shutdown waiting for the worker that runs this.
	// Both calls on the game's window are asynchronous: SetForegroundWindow
	// queues the activation to the game's thread. A click on our toast or
	// balloon lets this process take the foreground; if Windows didn't allow it
	// anyway, the taskbar button keeps flashing. (Raising the window by other
	// means, BringWindowToTop/SetWindowPos, waits for the game's thread.)
	void BringToFront()
	{
		HWND target = Target();
		if (!target)
			return;
		if (IsIconic(target))
			ShowWindowAsync(target, SW_RESTORE);
		SetForegroundWindow(target);
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

	// Everything only the worker thread touches. It lives on the worker's
	// stack and is passed along explicitly (its window procedure finds it in
	// the window's user data), so two workers can never share it.
	struct Worker
	{
		std::shared_ptr<Shared> shared;
		std::wstring windowClass;       // unique per worker: an older one may still be stopping
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

	void RemoveTray(Worker& w)
	{
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

	void ClearBadge(Worker& w)
	{
		if (!w.badged)
			return;
		if (w.taskbar)
			w.taskbar->SetOverlayIcon(w.badged, nullptr, nullptr);
		FLASHWINFO fi = { sizeof(fi), w.badged, FLASHW_STOP, 0, 0 };
		FlashWindowEx(&fi);
		w.badged = nullptr;
	}

	void Badge(Worker& w, HWND target)
	{
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
	bool ShowBalloon(Worker& w, const notify::Kind& kind)
	{
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
	HRESULT ShowToast(Worker& w, int index, const notify::Kind& kind)
	{
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
		// is free-threaded): it runs on a WinRT thread and only posts to the
		// worker that showed it, through that worker's own Shared (it may be
		// gone by then: the post then goes nowhere). Its code must outlive the
		// client realm: see PinSelf.
		if (!PinSelf())
		{
			const DWORD error = GetLastError();
			return error ? HRESULT_FROM_WIN32(error) : E_FAIL;   // never S_OK: the balloon shows instead
		}
		std::shared_ptr<Shared> shared = w.shared;
		EventRegistrationToken token = {};
		hr = toast->add_Activated(Callback<WinFoundation::ITypedEventHandler<WinNotify::ToastNotification*, IInspectable*>>(
			[shared](WinNotify::IToastNotification*, IInspectable*) -> HRESULT
			{
				BringToFront();
				if (HWND window = shared->window.load())
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

	void Show(Worker& w, int index, const notify::Kind* kindPtr)
	{
		if (!kindPtr || !kindPtr->id || !kindPtr->text)
			return;
		const notify::Kind& kind = *kindPtr;
		// Checked again here: the player may be back in the game by now.
		if (TargetInFront())
		{
			SetResult("skipped: focused");
			return;
		}

		if (w.shortcut < 0)
			w.shortcut = ShortcutRegistered() || g_forceToast.load() ? 1 : 0;
		std::string result;
		bool shown = false;
		if (w.shortcut == 1)
		{
			HRESULT hr = ShowToast(w, index, kind);
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
			result += ShowBalloon(w, kind) ? "balloon" : "balloon failed";

		if (HWND target = Target())
			Badge(w, target);
		SetResult(result);
	}

	LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		// The worker this window belongs to (set right after it's created,
		// cleared before it's destroyed).
		Worker* w = reinterpret_cast<Worker*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
		if (!w)
			return DefWindowProcW(hwnd, msg, wp, lp);
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
				RemoveTray(*w);
				ClearBadge(*w);
				break;
			case NIN_BALLOONTIMEOUT:
				AddResult("; balloon gone");
				RemoveTray(*w);
				break;
			}
			return 0;
		case WM_CLICKED:
			AddResult("; toast clicked");
			ClearBadge(*w);
			return 0;
		case WM_TIMER:
			if (wp == TIMER_TRAY)
				RemoveTray(*w);
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	// Everything that could call back into this DLL or outlive the game goes:
	// the toasts' click handlers, the tray icon, the badge, the window and its
	// class. Allocates nothing, so it also runs after an exception.
	void Teardown(Worker& w, HMODULE self)
	{
		for (ShownToast& t : w.toasts)
			t.toast->remove_Activated(t.token);
		w.toasts.clear();
		w.notifier.Reset();
		RemoveTray(w);
		ClearBadge(w);
		w.taskbar.Reset();
		w.shared->window = nullptr;
		if (w.window)
		{
			SetWindowLongPtrW(w.window, GWLP_USERDATA, 0);
			DestroyWindow(w.window);
			w.window = nullptr;
		}
		if (!w.windowClass.empty())
			UnregisterClassW(w.windowClass.c_str(), self);
		if (w.smallIcon)
			DestroyIcon(w.smallIcon);
		if (w.largeIcon)
			DestroyIcon(w.largeIcon);
		w.smallIcon = w.largeIcon = nullptr;
	}

	void RunWorker(Worker& w)
	{
		HMODULE self = Self();
		w.smallIcon = static_cast<HICON>(LoadImageW(self, MAKEINTRESOURCEW(IDI_MEOWY), IMAGE_ICON,
			GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
		w.largeIcon = static_cast<HICON>(LoadImageW(self, MAKEINTRESOURCEW(IDI_MEOWY), IMAGE_ICON,
			GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
		// A class of its own: an older worker that Shutdown stopped waiting for
		// unregisters its class when it finally ends, which must not take this
		// worker's class away.
		w.windowClass = L"MeowyClientPackNotify" + std::to_wstring(GetCurrentThreadId());
		WNDCLASSEXW wc = { sizeof(wc) };
		wc.lpfnWndProc = WindowProc;
		wc.hInstance = self;
		wc.lpszClassName = w.windowClass.c_str();
		RegisterClassExW(&wc);
		// A hidden ordinary window (not message-only): the tray needs one to own its icon.
		w.window = CreateWindowExW(WS_EX_TOOLWINDOW, w.windowClass.c_str(), L"Meowy Client Pack notifications", WS_POPUP,
			0, 0, 0, 0, nullptr, nullptr, self, nullptr);
		if (w.window)
			SetWindowLongPtrW(w.window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&w));
		w.shared->window = w.window;
		try
		{
			w.iconUri = WriteIcon();
		}
		catch (...)   // out of memory: toasts without our logo
		{
		}

		Shared& shared = *w.shared;
		while (!shared.stop.load())
		{
			DWORD wait = MsgWaitForMultipleObjects(1, &shared.wake, FALSE, w.badged ? 250 : INFINITE, QS_ALLINPUT);
			(void)wait;
			if (shared.stop.load())
				break;

			std::deque<std::pair<int, const notify::Kind*>> todo;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				todo.swap(shared.queue);
			}
			for (const auto& item : todo)
			{
				if (shared.stop.load())
					break;
				try
				{
					Show(w, item.first, item.second);
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
				ClearBadge(w);
		}
	}

	// What a new worker gets: its Shared, and its own reference to this DLL.
	struct StartParams
	{
		std::shared_ptr<Shared> shared;
		HMODULE module = nullptr;
	};

	// The worker. It holds its own reference to this DLL and drops it as its
	// very last step (FreeLibraryAndExitThread): the client realm's FreeLibrary
	// can't unload the code it's running, even when Shutdown stopped waiting
	// for it (it's stuck somewhere in Windows) and the realm has closed.
	DWORD WINAPI WorkerMain(void* param)
	{
		HMODULE module = nullptr;
		{
			std::unique_ptr<StartParams> params(static_cast<StartParams*>(param));
			module = params->module;
			// STA, with the message loop in RunWorker: the taskbar and shell-link
			// objects are apartment-threaded. Toast click handlers come in on
			// WinRT threads.
			HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
			{
				Worker w;
				w.shared = std::move(params->shared);
				params.reset();
				try
				{
					RunWorker(w);
				}
				catch (...)   // out of memory while starting: stop cleanly instead
				{
				}
				Teardown(w, Self());
			}   // the worker's COM objects are released here, before CoUninitialize
			if (SUCCEEDED(co))
				CoUninitialize();
		}
		FreeLibraryAndExitThread(module, 0);
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
		size_t count = 0;
		const notify::Kind* kinds = notify::Kinds(&count);
		if (index < 0 || static_cast<size_t>(index) >= count)
			return false;
		if (!g_thread)
		{
			std::unique_ptr<StartParams> params(new StartParams());
			params->shared = std::make_shared<Shared>();
			params->shared->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!params->shared->wake)
				return false;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&WorkerMain),
				&params->module))
				return false;
			std::shared_ptr<Shared> shared = params->shared;
			HMODULE module = params->module;
			HANDLE thread = CreateThread(nullptr, 0, WorkerMain, params.get(), 0, nullptr);
			if (!thread)
			{
				FreeLibrary(module);
				return false;
			}
			params.release();   // the worker owns it now
			g_thread = thread;
			g_shared = std::move(shared);
		}
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (g_shared->queue.size() < kMaxQueued)
				g_shared->queue.push_back({ index, &kinds[index] });
		}
		SetEvent(g_shared->wake);
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
			g_shared->queue.clear();
		}
		g_shared->stop = true;
		SetEvent(g_shared->wake);
		// Usually immediate. A worker stuck in Windows (Explorer not answering,
		// say) is let go after this: it keeps its own Shared and its own
		// reference to this DLL, and finishes on its own when it gets back.
		WaitForSingleObject(g_thread, kShutdownWaitMs);
		CloseHandle(g_thread);
		g_thread = nullptr;
		g_shared.reset();
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
