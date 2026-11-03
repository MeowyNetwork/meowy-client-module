#define _CRT_SECURE_NO_WARNINGS   // plain fopen/fscanf on our own small files
#include "loading_screen.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include "loading_webview.h"
#undef small   // rpcndr.h (via objbase.h) defines it; it's a font name below

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

namespace
{
	enum Stage { CONNECT, CONTENT, CITY, SCRIPTS, READY, STAGES };
	const wchar_t* const kStageNames[STAGES] = { L"Connect", L"Content", L"City", L"Scripts", L"Ready" };
	const wchar_t* const kStageLines[STAGES] = {
		L"Connecting to the server",
		L"Checking the city's content",
		L"Loading the city",
		L"Starting the city's scripts",
		L"Almost there",
	};
	// Seconds per stage on a warm join (2026-11-03 profiles), until this PC
	// has its own times in data/meowy/join_times.txt.
	const double kDefaultSecs[STAGES] = { 6, 4, 22, 30, 4 };
	// Fade-in is short: while it runs, GMod's own loading page shows through.
	constexpr ULONGLONG kFadeInMs = 60, kFadeOutMs = 600, kQuickFadeMs = 150;
	// After the loading panel closes, wait this long for the client Lua's Done
	// before fading out anyway.
	constexpr ULONGLONG kInGameTimeoutMs = 15000;
	// How long the page may hold the fade-out for a game in progress (once).
	constexpr ULONGLONG kHoldMs = 5000;
	// The page asked for keyboard focus (a game uses Space): the window may be
	// activated, and gives activation back to the game when it goes.
	bool g_focusMode = false;

	std::wstring Widen(const std::string& s)
	{
		if (s.empty())
			return L"";
		int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
		std::wstring out(n, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n);
		return out;
	}

	struct State
	{
		std::mutex m;
		bool running = false;           // the window thread is alive
		bool finishing = false;         // fading out; the thread exits when done
		bool quick = false;             // fade out fast (cancel, failure, hide)
		ULONGLONG finishAt = 0;
		bool inGame = false, done = false;
		int stage = CONNECT;
		ULONGLONG begin = 0, inGameAt = 0, doneAt = 0;
		ULONGLONG holdUntil = 0;        // the page asked to wait a moment before fading out
		ULONGLONG stageStart[STAGES] = {};
		double expected[STAGES] = {};
		std::wstring server = L"Meowy Network", map, status, steamid;
		std::wstring dataDir;
		FILE* log = nullptr;            // data/meowy/last_join.txt: what this join went through
	};
	State g;
	std::atomic<bool> g_cancel{ false };

	// --- Stage times --------------------------------------------------------

	void LoadExpected()
	{
		for (int i = 0; i < STAGES; ++i)
			g.expected[i] = kDefaultSecs[i];
		FILE* f = _wfopen((g.dataDir + L"\\join_times.txt").c_str(), L"r");
		if (!f)
			return;
		double v[STAGES];
		if (std::fscanf(f, "%lf %lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3], &v[4]) == STAGES)
			for (int i = 0; i < STAGES; ++i)
				if (v[i] > 0.5 && v[i] < 600)
					g.expected[i] = v[i];
		std::fclose(f);
	}

	// Called with g.m held, once the player is in. Stages that were seen get
	// this join's time; the others keep what they had.
	void SaveTimes(ULONGLONG end)
	{
		double out[STAGES];
		for (int i = 0; i < STAGES; ++i)
		{
			out[i] = g.expected[i];
			if (!g.stageStart[i])
				continue;
			ULONGLONG next = end;
			for (int j = i + 1; j < STAGES; ++j)
				if (g.stageStart[j]) { next = g.stageStart[j]; break; }
			out[i] = (next - g.stageStart[i]) / 1000.0;
		}
		CreateDirectoryW(g.dataDir.c_str(), nullptr);
		FILE* f = _wfopen((g.dataDir + L"\\join_times.txt").c_str(), L"w");
		if (!f)
			return;
		std::fprintf(f, "%.1f %.1f %.1f %.1f %.1f\n", out[0], out[1], out[2], out[3], out[4]);
		std::fclose(f);
	}

	void Log(const char* what, const std::string& detail = "")
	{
		if (!g.log)
			return;
		std::fprintf(g.log, "%.2f\t%s\t%s\n", (GetTickCount64() - g.begin) / 1000.0, what, detail.c_str());
		std::fflush(g.log);
	}

	void CloseLog()
	{
		if (g.log)
			std::fclose(g.log);
		g.log = nullptr;
	}

	// With g.m held. Stages only move forward.
	void Advance(int stage, ULONGLONG now)
	{
		if (stage <= g.stage)
			return;
		for (int i = g.stage + 1; i <= stage; ++i)
			if (!g.stageStart[i])
				g.stageStart[i] = now;
		g.stage = stage;
	}

	// GMod's load status strings (lua/menu/loading.lua shows the same ones),
	// 2026-11-03: "Authenticating with Steam...", "Retrieving server info...",
	// "Retrieving string tables...", "3/30 (1.2 GB total) - Checking for
	// updates", "Mounting Workshop Addons", "57/58 - <file> is downloading",
	// "Loading the map...", "Loading server cache content...", "Sending client
	// info...", "Starting Lua...", "Lua Started!", "Fully connected!", "Ready to
	// play!". Anything else keeps the current stage.
	int StageFor(const std::string& s)
	{
		auto has = [&](const char* x) { return s.find(x) != std::string::npos; };
		if (has("Authenticating") || has("Retrieving") || has("Connecting"))
			return CONNECT;
		if (has("Loading the map") || has("server cache") || has("client info"))
			return CITY;
		if (has(" total)") || has("orkshop") || has("ownload") || has("Mounting") || has("Checking for updates"))
			return CONTENT;
		if (has("Lua"))
			return SCRIPTS;
		if (has("Fully connected") || has("Ready to play"))
			return READY;
		return -1;
	}

	void FinishLocked(bool quick)
	{
		if (g.finishing)
			return;
		g.finishing = true;
		g.quick = quick;
		g.finishAt = GetTickCount64();
	}

	// --- Window ---------------------------------------------------------------

	struct View
	{
		HWND hwnd = nullptr;
		HWND game = nullptr;
		RECT cancelBtn{}, hideBtn{};
		int fontH = 0;
		HFONT title = nullptr, sub = nullptr, body = nullptr, small = nullptr;
	};
	View v;

	BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM out)
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || hwnd == v.hwnd)
			return TRUE;
		wchar_t cls[64];
		if (GetClassNameW(hwnd, cls, 64) && std::wcscmp(cls, L"Valve001") == 0)
		{
			*reinterpret_cast<HWND*>(out) = hwnd;
			return FALSE;
		}
		return TRUE;
	}

	void MakeFonts(int h)
	{
		if (h == v.fontH)
			return;
		for (HFONT f : { v.title, v.sub, v.body, v.small })
			if (f)
				DeleteObject(f);
		auto font = [](int px, int weight) {
			return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
				CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
		};
		v.title = font(h * 7 / 100, FW_SEMIBOLD);
		v.sub = font(h * 26 / 1000, FW_NORMAL);
		v.body = font(h * 22 / 1000, FW_NORMAL);
		v.small = font(h * 17 / 1000, FW_NORMAL);
		v.fontH = h;
	}

	void Text(HDC dc, HFONT f, COLORREF c, const std::wstring& s, RECT r, UINT flags)
	{
		SelectObject(dc, f);
		SetTextColor(dc, c);
		DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, flags | DT_NOPREFIX);
	}

	void Fill(HDC dc, RECT r, COLORREF c, int radius)
	{
		HBRUSH b = CreateSolidBrush(c);
		HGDIOBJ oldB = SelectObject(dc, b);
		HGDIOBJ oldP = SelectObject(dc, GetStockObject(NULL_PEN));
		RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, radius, radius);
		SelectObject(dc, oldP);
		SelectObject(dc, oldB);
		DeleteObject(b);
	}

	std::wstring Clock(ULONGLONG inMs)
	{
		FILETIME ft;
		GetSystemTimeAsFileTime(&ft);
		ULARGE_INTEGER t;
		t.LowPart = ft.dwLowDateTime;
		t.HighPart = ft.dwHighDateTime;
		t.QuadPart += inMs * 10000ULL;
		ft.dwLowDateTime = t.LowPart;
		ft.dwHighDateTime = t.HighPart;
		FILETIME local;
		SYSTEMTIME st;
		FileTimeToLocalFileTime(&ft, &local);
		FileTimeToSystemTime(&local, &st);
		wchar_t buf[16];
		swprintf(buf, 16, L"%02u:%02u", st.wHour, st.wMinute);
		return buf;
	}

	void Draw(HDC dc, int w, int h)
	{
		ULONGLONG now = GetTickCount64();
		int stage;
		double expected[STAGES];
		ULONGLONG starts[STAGES], begin;
		std::wstring server, map, status;
		{
			std::lock_guard<std::mutex> lock(g.m);
			stage = g.stage;
			begin = g.begin;
			for (int i = 0; i < STAGES; ++i)
			{
				expected[i] = g.expected[i];
				starts[i] = g.stageStart[i];
			}
			server = g.server;
			map = g.map;
			status = g.status;
		}
		MakeFonts(h);
		SetBkMode(dc, TRANSPARENT);

		// Background: a dark violet gradient.
		TRIVERTEX vert[2] = {
			{ 0, 0, 0x1000, 0x1200, 0x2400, 0xFF00 },
			{ w, h, 0x2400, 0x1400, 0x3400, 0xFF00 },
		};
		GRADIENT_RECT gr = { 0, 1 };
		GradientFill(dc, vert, 2, &gr, 1, GRADIENT_FILL_RECT_V);

		const COLORREF white = RGB(245, 242, 255), dim = RGB(170, 160, 200), accent = RGB(190, 140, 255),
			track = RGB(52, 42, 78), pink = RGB(255, 150, 205);
		const int pad = w / 16;

		Text(dc, v.small, accent, L"MEOWY NETWORK  ·  CLIENT PACK", { pad, h * 10 / 100, w - pad, h * 14 / 100 }, DT_LEFT | DT_SINGLELINE);
		Text(dc, v.title, white, server, { pad, h * 14 / 100, w - pad, h * 26 / 100 }, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
		if (!map.empty())
			Text(dc, v.sub, dim, L"Joining " + map, { pad, h * 26 / 100, w - pad, h * 31 / 100 }, DT_LEFT | DT_SINGLELINE);

		// Progress through the current stage, eased and held below 95% until
		// the next stage really starts.
		double elapsed = starts[stage] ? (now - starts[stage]) / 1000.0 : (now - begin) / 1000.0;
		double frac = expected[stage] > 0 ? elapsed / expected[stage] : 1;
		double shown = 0.95 * (1 - std::exp(-2.2 * frac));

		// Stage line with a spinner in front of it.
		int lineY = h * 62 / 100;
		int r = h / 60, cx = pad + r * 2, cy = lineY + h / 50;
		for (int i = 0; i < 12; ++i)
		{
			double a = i * 3.14159265 / 6;
			int k = static_cast<int>((now / 80 + 12 - i) % 12);
			int lum = 255 - k * 16;
			HBRUSH b = CreateSolidBrush(RGB(lum * 190 / 255, lum * 140 / 255, lum));
			HGDIOBJ oldB = SelectObject(dc, b);
			HGDIOBJ oldP = SelectObject(dc, GetStockObject(NULL_PEN));
			int x = cx + static_cast<int>(std::cos(a) * r * 1.6), y = cy + static_cast<int>(std::sin(a) * r * 1.6);
			int d = r / 3 + 1;
			Ellipse(dc, x - d, y - d, x + d, y + d);
			SelectObject(dc, oldP);
			SelectObject(dc, oldB);
			DeleteObject(b);
		}
		Text(dc, v.sub, white, kStageLines[stage], { pad + r * 5, lineY, w - pad, lineY + h / 22 }, DT_LEFT | DT_SINGLELINE);
		std::wstring detail = status;
		if (stage == SCRIPTS)
			detail = L"Garry's Mod may say \"Not Responding\" for a while here. It's working; this screen keeps going.";
		else if (elapsed > 3 * expected[stage] && elapsed > 60)
			detail = L"This is taking longer than usual.";
		Text(dc, v.body, dim, detail, { pad + r * 5, lineY + h / 22, w - pad, lineY + h * 9 / 100 }, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

		// Remaining time: what's left of this stage plus the stages after it.
		double remain = expected[stage] - elapsed;
		if (remain < 2)
			remain = 2;
		for (int i = stage + 1; i < STAGES; ++i)
			remain += expected[i];
		wchar_t eta[96];
		ULONGLONG joining = (now - begin) / 1000;
		swprintf(eta, 96, L"Ready around %s   ·   joining for %llu:%02llu", Clock(static_cast<ULONGLONG>(remain * 1000)).c_str(),
			joining / 60, joining % 60);
		Text(dc, v.body, dim, eta, { pad, lineY, w - pad, lineY + h / 22 }, DT_RIGHT | DT_SINGLELINE);

		// Five segments.
		int segY = h * 74 / 100, segH = h / 90 + 2, gap = w / 120;
		int segW = (w - 2 * pad - gap * (STAGES - 1)) / STAGES;
		for (int i = 0; i < STAGES; ++i)
		{
			RECT s = { pad + i * (segW + gap), segY, pad + i * (segW + gap) + segW, segY + segH };
			Fill(dc, s, track, segH);
			double f = i < stage ? 1 : i == stage ? shown : 0;
			if (f > 0)
			{
				RECT full = s;
				full.right = s.left + static_cast<int>(segW * f);
				Fill(dc, full, i == stage ? pink : accent, segH);
			}
			Text(dc, v.small, i <= stage ? white : dim, kStageNames[i], { s.left, segY + segH + h / 120, s.right, segY + segH + h / 25 },
				DT_LEFT | DT_SINGLELINE);
		}

		// Buttons, bottom right.
		int bh = h / 18, bw = w / 9, by = h - pad / 2 - bh;
		v.cancelBtn = { w - pad - bw, by, w - pad, by + bh };
		v.hideBtn = { w - pad - bw * 2 - gap * 2, by, w - pad - bw - gap * 2, by + bh };
		Fill(dc, v.cancelBtn, RGB(70, 50, 100), bh / 3);
		Fill(dc, v.hideBtn, RGB(45, 36, 66), bh / 3);
		Text(dc, v.body, white, L"Cancel", v.cancelBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		Text(dc, v.body, dim, L"Game's screen", v.hideBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		Text(dc, v.small, dim, L"Shown by the Meowy Client Pack, only when you join Meowy Network.",
			{ pad, by, v.hideBtn.left - gap, by + bh }, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	}

	bool Inside(const RECT& r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

	// --- The web page (loading_webview.h) ---------------------------------------

	const wchar_t* const kPageUrl = L"https://meowynetwork.com/pack-loading.html";

	// The page, or for developing it a local dev server given at launch with
	// -meowy_loading_url http://localhost:5173/pack-loading.html (only
	// localhost addresses are accepted there).
	std::wstring PageUrl()
	{
		const wchar_t* cmd = GetCommandLineW();
		const wchar_t* at = std::wcsstr(cmd, L"-meowy_loading_url ");
		if (!at)
			return kPageUrl;
		at += std::wcslen(L"-meowy_loading_url ");
		std::wstring url;
		while (*at && *at != L' ' && *at != L'"')
			url += *at++;
		if (url.rfind(L"http://localhost:", 0) == 0 || url.rfind(L"http://127.0.0.1:", 0) == 0)
			return url;
		return kPageUrl;
	}

	// WebView2's profile (cache, cookies of our site only): %LOCALAPPDATA%\MeowyClientPack\WebView2.
	std::wstring WebProfileDir()
	{
		wchar_t base[MAX_PATH];
		DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
		if (n == 0 || n >= MAX_PATH)
			return L"";
		std::wstring dir = std::wstring(base) + L"\\MeowyClientPack";
		CreateDirectoryW(dir.c_str(), nullptr);
		return dir + L"\\WebView2";
	}

	void JsonString(std::wstring& out, const std::wstring& s)
	{
		out += L'"';
		for (wchar_t c : s)
		{
			if (c == L'"' || c == L'\\')
			{
				out += L'\\';
				out += c;
			}
			else if (c < 0x20)
			{
				wchar_t buf[8];
				swprintf(buf, 8, L"\\u%04x", static_cast<unsigned>(c));
				out += buf;
			}
			else
			{
				out += c;
			}
		}
		out += L'"';
	}

	// What the page draws from; see Website/src/pack-loading/main.ts.
	std::wstring StateJson(ULONGLONG now)
	{
		std::lock_guard<std::mutex> lock(g.m);
		wchar_t num[64];
		std::wstring j = L"{\"type\":\"state\",\"stage\":" + std::to_wstring(g.stage);
		ULONGLONG start = g.stageStart[g.stage] ? g.stageStart[g.stage] : g.begin;
		swprintf(num, 64, L",\"stageElapsed\":%.2f,\"elapsed\":%.2f,\"expected\":[", (now - start) / 1000.0, (now - g.begin) / 1000.0);
		j += num;
		for (int i = 0; i < STAGES; ++i)
		{
			swprintf(num, 64, i ? L",%.1f" : L"%.1f", g.expected[i]);
			j += num;
		}
		j += L"],\"status\":";
		JsonString(j, g.status);
		j += L",\"server\":";
		JsonString(j, g.server);
		j += L",\"map\":";
		JsonString(j, g.map);
		j += L",\"steamid\":";
		JsonString(j, g.steamid);
		j += L"}";
		return j;
	}

	// Development aid: with data/meowy/screenshot_debug present, the screen
	// saves what it draws to data/meowy/shots/ every few seconds (it's a
	// separate window, so the game's own screenshots don't include it).
	void SaveBmp(HDC mem, HBITMAP bmp, int w, int h, const std::wstring& path)
	{
		BITMAPINFOHEADER bi = {};
		bi.biSize = sizeof(bi);
		bi.biWidth = w;
		bi.biHeight = h;
		bi.biPlanes = 1;
		bi.biBitCount = 24;
		bi.biCompression = BI_RGB;
		DWORD stride = ((w * 3 + 3) & ~3u);
		std::vector<unsigned char> pixels(static_cast<size_t>(stride) * h);
		if (!GetDIBits(mem, bmp, 0, h, pixels.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS))
			return;
		BITMAPFILEHEADER fh = {};
		fh.bfType = 0x4D42;
		fh.bfOffBits = sizeof(fh) + sizeof(bi);
		fh.bfSize = fh.bfOffBits + static_cast<DWORD>(pixels.size());
		FILE* f = _wfopen(path.c_str(), L"wb");
		if (!f)
			return;
		std::fwrite(&fh, sizeof(fh), 1, f);
		std::fwrite(&bi, sizeof(bi), 1, f);
		std::fwrite(pixels.data(), 1, pixels.size(), f);
		std::fclose(f);
	}

	LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		switch (msg)
		{
		case WM_MOUSEACTIVATE:
			return g_focusMode ? MA_ACTIVATE : MA_NOACTIVATE;
		case WM_LBUTTONUP:
		{
			int x = static_cast<short>(LOWORD(lp)), y = static_cast<short>(HIWORD(lp));
			std::lock_guard<std::mutex> lock(g.m);
			if (Inside(v.cancelBtn, x, y))
			{
				g_cancel = true;
				Log("cancel");
				FinishLocked(true);
			}
			else if (Inside(v.hideBtn, x, y))
			{
				Log("hide");
				FinishLocked(true);
			}
			return 0;
		}
		case WM_ERASEBKGND:
			return 1;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	DWORD WINAPI Run(void*)
	{
		// WebView2 needs a single-threaded COM apartment with a message loop: this thread.
		HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		HINSTANCE inst = GetModuleHandleW(nullptr);
		WNDCLASSW wc = {};
		wc.lpfnWndProc = WndProc;
		wc.hInstance = inst;
		wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
		wc.lpszClassName = L"MeowyLoadingScreen";
		RegisterClassW(&wc);   // fails harmlessly when already registered
		v.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"Meowy Network",
			WS_POPUP | WS_CLIPCHILDREN, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
		if (!v.hwnd)
		{
			std::lock_guard<std::mutex> lock(g.m);
			g.running = false;
			if (SUCCEEDED(com))
				CoUninitialize();
			return 0;
		}
		SetLayeredWindowAttributes(v.hwnd, 0, 0, LWA_ALPHA);

		// The web page loads behind the native drawing and takes over once it says
		// it's ready. It gets 8 s; after that, or on any failure, the native drawing stays.
		const ULONGLONG webStarted = GetTickCount64();
		bool webOn = SUCCEEDED(com) && loading_webview::Start(v.hwnd, PageUrl(), WebProfileDir());
		bool webShown = false;
		ULONGLONG lastPost = 0;
		{
			std::lock_guard<std::mutex> lock(g.m);
			Log(webOn ? "web start" : "web unavailable");
		}

		RECT last = {};
		std::wstring shotDir;
		{
			std::lock_guard<std::mutex> lock(g.m);
			if (GetFileAttributesW((g.dataDir + L"\\screenshot_debug").c_str()) != INVALID_FILE_ATTRIBUTES)
			{
				shotDir = g.dataDir + L"\\shots";
				CreateDirectoryW(shotDir.c_str(), nullptr);
			}
		}
		ULONGLONG countFrom = GetTickCount64(), lastShot = 0;
		int frames = 0, shotNo = 0;
		bool shown = false;
		ULONGLONG shownAt = 0;
		HDC mem = nullptr;
		HBITMAP bmp = nullptr;
		int bw = 0, bh = 0;

		for (;;)
		{
			MSG msg;
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
			ULONGLONG now = GetTickCount64();

			if (webOn)
			{
				if (!webShown && loading_webview::Ready())
				{
					webShown = true;
					std::lock_guard<std::mutex> lock(g.m);
					Log("web ready");
				}
				else if (!webShown && (loading_webview::Failed() || now - webStarted > 8000))
				{
					loading_webview::Stop();
					webOn = false;
					std::lock_guard<std::mutex> lock(g.m);
					Log(loading_webview::Failed() ? "web failed" : "web too slow");
				}
				char action = loading_webview::TakeAction();
				if (action == 'c' || action == 'h')
				{
					std::lock_guard<std::mutex> lock(g.m);
					if (action == 'c')
						g_cancel = true;
					Log(action == 'c' ? "cancel" : "hide");
					FinishLocked(true);
				}
				else if (action == 'k')
				{
					std::lock_guard<std::mutex> lock(g.m);
					if (!g.holdUntil)
					{
						g.holdUntil = now + kHoldMs;
						Log("hold");
					}
				}
				else if (action == 'f' && !g_focusMode)
				{
					g_focusMode = true;
					SetWindowLongPtrW(v.hwnd, GWL_EXSTYLE, GetWindowLongPtrW(v.hwnd, GWL_EXSTYLE) & ~WS_EX_NOACTIVATE);
					SetForegroundWindow(v.hwnd);
					loading_webview::Focus();
					std::lock_guard<std::mutex> lock(g.m);
					Log("focus");
				}
				if (webShown && now - lastPost >= 200)
				{
					loading_webview::Post(StateJson(now));
					lastPost = now;
				}
			}

			// Fading and exit decisions.
			double alpha;
			{
				std::lock_guard<std::mutex> lock(g.m);
				if (!g.finishing && g.done && now - g.doneAt > 300 && now >= g.holdUntil)
					FinishLocked(false);
				if (!g.finishing && g.inGame && !g.done && now - g.inGameAt > kInGameTimeoutMs)
					FinishLocked(false);
				if (g.finishing)
				{
					ULONGLONG len = g.quick ? kQuickFadeMs : kFadeOutMs;
					ULONGLONG t = now - g.finishAt;
					if (t >= len)
					{
						g.running = false;
						break;
					}
					alpha = 1.0 - static_cast<double>(t) / len;
				}
				else
				{
					alpha = shown ? (now - shownAt >= kFadeInMs ? 1.0 : static_cast<double>(now - shownAt) / kFadeInMs) : 0.0;
				}
			}

			// Cover the game window's client area; follow it, hide when minimised.
			if (!v.game || !IsWindow(v.game))
			{
				v.game = nullptr;
				EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&v.game));
			}
			if (v.game)
			{
				RECT rc;
				GetClientRect(v.game, &rc);
				POINT tl = { 0, 0 };
				ClientToScreen(v.game, &tl);
				RECT pos = { tl.x, tl.y, tl.x + rc.right, tl.y + rc.bottom };
				bool iconic = IsIconic(v.game) != 0 || rc.right < 64 || rc.bottom < 64;
				if (iconic)
				{
					if (shown)
						ShowWindow(v.hwnd, SW_HIDE);
					shown = false;
				}
				else
				{
					if (std::memcmp(&pos, &last, sizeof(pos)) != 0)
					{
						SetWindowPos(v.hwnd, nullptr, pos.left, pos.top, rc.right, rc.bottom, SWP_NOACTIVATE | SWP_NOZORDER);
						if (webOn)
							loading_webview::Resize(rc.right, rc.bottom);
						last = pos;
					}
					HWND fg = GetForegroundWindow();
					if (fg == v.game || fg == v.hwnd)
						SetWindowPos(v.hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
					if (!shown)
					{
						ShowWindow(v.hwnd, SW_SHOWNOACTIVATE);
						shown = true;
						if (!shownAt)
						{
							shownAt = now;
							std::lock_guard<std::mutex> lock(g.m);
							Log("shown");
						}
					}

					int w = rc.right, h = rc.bottom;
					HDC dc = GetDC(v.hwnd);
					if (!mem || w != bw || h != bh)
					{
						if (mem)
						{
							DeleteObject(bmp);
							DeleteDC(mem);
						}
						mem = CreateCompatibleDC(dc);
						bmp = CreateCompatibleBitmap(dc, w, h);
						SelectObject(mem, bmp);
						bw = w;
						bh = h;
					}
					if (!webShown)
					{
						Draw(mem, w, h);
						BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
					}
					++frames;
					if (!shotDir.empty() && now - lastShot >= 4000)
					{
						wchar_t name[32];
						swprintf(name, 32, webShown ? L"\\shot_%02d.png" : L"\\shot_%02d.bmp", shotNo++);
						if (webShown)
							loading_webview::Capture(shotDir + name);
						else
							SaveBmp(mem, bmp, w, h, shotDir + name);
						lastShot = now;
					}
					ReleaseDC(v.hwnd, dc);
					SetLayeredWindowAttributes(v.hwnd, 0, static_cast<BYTE>(alpha * 255), LWA_ALPHA);
				}
			}
			// Proof the screen keeps going while the game is busy: frames drawn per 5 s.
			if (now - countFrom >= 5000)
			{
				std::lock_guard<std::mutex> lock(g.m);
				Log("frames", std::to_string(frames));
				frames = 0;
				countFrom = now;
			}
			Sleep(33);
		}

		// Keyboard focus goes back to the game if this screen had it.
		if (g_focusMode && v.game && GetForegroundWindow() == v.hwnd)
			SetForegroundWindow(v.game);
		g_focusMode = false;
		loading_webview::Stop();
		if (mem)
		{
			DeleteObject(bmp);
			DeleteDC(mem);
		}
		DestroyWindow(v.hwnd);
		v.hwnd = nullptr;
		if (SUCCEEDED(com))
			CoUninitialize();
		{
			std::lock_guard<std::mutex> lock(g.m);
			CloseLog();
		}
		return 0;
	}
}

namespace loading_screen
{
	void Begin(const std::wstring& dataDir, double panelOpenMs)
	{
		static bool ghostingOff = false;
		if (!ghostingOff)
		{
			// Windows otherwise swaps the busy game window for a "(Not
			// Responding)" ghost that offers to close the game.
			DisableProcessWindowsGhosting();
			ghostingOff = true;
		}

		std::lock_guard<std::mutex> lock(g.m);
		ULONGLONG now = GetTickCount64();
		g.dataDir = dataDir;
		g.finishing = g.quick = g.inGame = g.done = false;
		g.holdUntil = 0;
		g.stage = CONNECT;
		g.begin = now;
		for (int i = 0; i < STAGES; ++i)
			g.stageStart[i] = 0;
		g.stageStart[CONNECT] = now;
		g.server = L"Meowy Network";
		g.map.clear();
		g.steamid.clear();
		g.status = L"Connecting…";
		g_cancel = false;
		LoadExpected();
		CloseLog();
		CreateDirectoryW(dataDir.c_str(), nullptr);
		g.log = _wfopen((dataDir + L"\\last_join.txt").c_str(), L"w");
		Log("begin", "GMod's loading panel open " + std::to_string(static_cast<long long>(panelOpenMs)) + " ms");

		if (!g.running)
		{
			HANDLE t = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr);
			if (t)
			{
				g.running = true;
				CloseHandle(t);
			}
		}
	}

	void Details(const std::string& serverName, const std::string& map, const std::string& steamid)
	{
		std::lock_guard<std::mutex> lock(g.m);
		if (!g.running)
			return;
		if (!serverName.empty())
			g.server = Widen(serverName);
		g.map = Widen(map);
		g.steamid = Widen(steamid);
		Log("details", serverName + " / " + map);
	}

	void Status(const std::string& status)
	{
		std::lock_guard<std::mutex> lock(g.m);
		if (!g.running || status.empty())
			return;
		// Workshop lines put the item's title on a second line.
		std::string flat = status;
		for (char& c : flat)
			if (c == '\r' || c == '\n')
				c = ' ';
		g.status = Widen(flat);
		// Ready comes from being in game (InGame), or from these statuses once the
		// scripts stage has started; a stray one can't skip the join ahead.
		int stage = StageFor(status);
		if (stage == READY && g.stage < SCRIPTS)
			stage = -1;
		Advance(stage, GetTickCount64());
		Log("status", status);
	}

	void ClientLuaStarted()
	{
		std::lock_guard<std::mutex> lock(g.m);
		if (!g.running)
			return;
		Advance(SCRIPTS, GetTickCount64());
		Log("client lua");
	}

	void InGame()
	{
		std::lock_guard<std::mutex> lock(g.m);
		if (!g.running || g.inGame)
			return;
		ULONGLONG now = GetTickCount64();
		g.inGame = true;
		g.inGameAt = now;
		Advance(READY, now);
		Log("in game");
	}

	void Done()
	{
		std::lock_guard<std::mutex> lock(g.m);
		if (!g.running || g.done)
			return;
		ULONGLONG now = GetTickCount64();
		g.done = true;
		g.doneAt = now;
		Advance(READY, now);
		if (!g.quick)
			SaveTimes(now);
		Log("done");
	}

	void End()
	{
		std::lock_guard<std::mutex> lock(g.m);
		if (!g.running)
			return;
		Log("end");
		FinishLocked(true);
	}

	bool CancelRequested()
	{
		return g_cancel.exchange(false);
	}

	void Note(const char* what, const std::string& detail)
	{
		std::lock_guard<std::mutex> lock(g.m);
		if (g.running)
			Log(what, detail);
	}

	bool Active()
	{
		std::lock_guard<std::mutex> lock(g.m);
		return g.running && !g.finishing;
	}
}

#else

namespace loading_screen
{
	void Begin(const std::wstring&, double) {}
	void Details(const std::string&, const std::string&, const std::string&) {}
	void Status(const std::string&) {}
	void ClientLuaStarted() {}
	void InGame() {}
	void Done() {}
	void End() {}
	bool CancelRequested() { return false; }
	void Note(const char*, const std::string&) {}
	bool Active() { return false; }
}

#endif
