#define _CRT_SECURE_NO_WARNINGS   // plain fopen/fscanf on our own small files
#include "loading_screen.h"

#ifdef _WIN32
#define NOMINMAX   // std::min / std::max below
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include "loading_webview.h"
#undef small   // rpcndr.h (via objbase.h) defines it; it's a font name below

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
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
	// has its own times in %LOCALAPPDATA%\MeowyClientPack\join_times.txt.
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
		bool running = false;           // the window thread is alive (for the session, once started)
		bool joining = false;           // a join is on screen
		unsigned joinSeq = 0;           // counts joins, so the thread notices a new one
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
		FILE* log = nullptr;            // last_join.txt in the pack data folder: what this join went through
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

	// Fonts by pixel size and weight, made once per window height.
	struct FontKey
	{
		int px, weight;
	};
	HFONT FontFor(int px, int weight)
	{
		static std::vector<std::pair<FontKey, HFONT>> cache;
		for (auto& f : cache)
			if (f.first.px == px && f.first.weight == weight)
				return f.second;
		HFONT f = CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
			CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
		cache.push_back({ { px, weight }, f });
		return f;
	}

	// Device pixels per CSS pixel, as WebView2 draws the page (the display's
	// scaling); the fallback is laid out in the same units. 1 without WebView2.
	double g_cssScale = 1.0;

	// The same layout and colours as the web page's Home page (Website
	// pack-loading.css), in its CSS pixels, so the switch to the page when it's
	// ready is barely visible: top bar, server name, the game cards, the strip.
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
		SetBkMode(dc, TRANSPARENT);
		// Work in the page's CSS pixels: cw x ch is its viewport.
		const double k = g_cssScale > 0.5 ? g_cssScale : 1.0;
		auto S = [&](double px) { return static_cast<int>(px * k + 0.5); };
		const double cw = w / k;
		auto clampd = [](double lo, double x, double hi) { return std::max(lo, std::min(x, hi)); };

		const COLORREF white = RGB(255, 255, 255), dim = RGB(190, 188, 196), faint = RGB(128, 126, 134),
			rose = RGB(225, 29, 72), rose2 = RGB(251, 113, 133), glass = RGB(22, 21, 27), glass2 = RGB(30, 28, 36),
			line = RGB(40, 39, 46), track = RGB(44, 43, 50), done = RGB(205, 204, 210), ready = RGB(34, 197, 94);

		// Background: near black with a faint rose tint at the top.
		TRIVERTEX vert[2] = {
			{ 0, 0, 0x1C00, 0x0F00, 0x1400, 0xFF00 },
			{ w, h, 0x0A00, 0x0A00, 0x0C00, 0xFF00 },
		};
		GRADIENT_RECT gr = { 0, 1 };
		GradientFill(dc, vert, 2, &gr, 1, GRADIENT_FILL_RECT_V);

		const int pad = S(clampd(20, cw * 0.032, 56));   // --pl-pad: clamp(20px, 3.2vw, 56px)
		const int topH = S(68), stripH = S(84);

		// ─── Top bar: brand and the page buttons (the page makes them work) ───
		int cy = topH / 2;
		Fill(dc, { pad, cy - S(14), pad + S(28), cy + S(14) }, rose, S(10));
		Text(dc, FontFor(S(15), FW_BOLD), white, L"MEOWY NETWORK", { pad + S(40), 0, pad + S(260), topH }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
		int navX = pad + S(40) + S(178) + S(clampd(16, cw * 0.03, 48));
		Fill(dc, { navX, cy - S(21), navX + S(4) + 3 * S(76) + S(4), cy + S(21) }, RGB(24, 23, 28), S(42));
		const wchar_t* tabs[] = { L"Home", L"Play", L"City" };
		for (int i = 0; i < 3; ++i)
		{
			RECT t = { navX + S(4) + i * S(76), cy - S(17), navX + S(4) + i * S(76) + S(76), cy + S(17) };
			if (i == 0)
				Fill(dc, t, rose, S(36));
			Text(dc, FontFor(S(15), FW_SEMIBOLD), i == 0 ? white : dim, tabs[i], t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		}

		// ─── Hero (left) ───────────────────────────────────────────────────────
		int pageTop = topH, pageBottom = h - stripH;
		int colGap = S(clampd(20, cw * 0.04, 72));
		int leftW = (w - 2 * pad - colGap) * 11 / 21;
		const int titlePx = S(clampd(34, cw * 0.042, 76));
		int heroY = pageTop + (pageBottom - pageTop) / 2 - titlePx * 2;
		std::wstring eyebrow = L"JOINING " + (map.empty() ? std::wstring(L"DOWNTOWN") : map);
		for (auto& c : eyebrow)
			c = static_cast<wchar_t>(towupper(c));
		Text(dc, FontFor(S(13), FW_BOLD), rose2, eyebrow, { pad, heroY, pad + leftW, heroY + S(20) }, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
		RECT title = { pad, heroY + S(26), pad + leftW, heroY + S(26) + titlePx * 7 / 2 };   // up to three lines
		Text(dc, FontFor(titlePx, FW_HEAVY), white, server, title, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);

		// ─── Game cards (right) ────────────────────────────────────────────────
		int rx = pad + leftW + colGap, rw = w - pad - rx;
		int cardH = S(108), cardGap = S(12);
		int cardsTop = pageTop + (pageBottom - pageTop - (S(30) + 3 * cardH + 2 * cardGap)) / 2;
		Text(dc, FontFor(S(clampd(13, cw * 0.0095, 16)), FW_BOLD), faint, L"SOMETHING TO DO WHILE YOU WAIT", { rx, cardsTop, rx + rw, cardsTop + S(24) }, DT_LEFT | DT_SINGLELINE);
		const wchar_t* names[] = { L"Where in Downtown?", L"Rooftop run", L"The city cat" };
		const wchar_t* blurbs[] = {
			L"A photo from somewhere in the city. Find it on the map.",
			L"Leap across the real rooftops of Downtown. You arrive when the game is ready.",
			L"Pet the cat. Head, cheeks, nose, belly or tail: she reacts to all of them.",
		};
		int y0 = cardsTop + S(30);
		for (int i = 0; i < 3; ++i)
		{
			int y = y0 + i * (cardH + cardGap);
			if (y + cardH > pageBottom - S(10))
				break;
			Fill(dc, { rx, y, rx + rw, y + cardH }, glass, S(16));
			RECT art = { rx + S(12), y + S(12), rx + S(12) + S(132), y + S(12) + S(84) };
			Fill(dc, art, glass2, S(12));
			Fill(dc, { art.left, art.bottom - S(4), art.right, art.bottom }, i == 1 ? rose : glass2, 0);
			RECT text = { art.right + S(18), y + S(22), rx + rw - S(12), y + cardH - S(10) };
			int titleCss = static_cast<int>(clampd(16, cw * 0.012, 20));
			Text(dc, FontFor(S(titleCss), FW_BOLD), white, names[i], { text.left, text.top, text.right, text.top + S(titleCss + 8) }, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
			Text(dc, FontFor(S(14), FW_NORMAL), dim, blurbs[i], { text.left, text.top + S(titleCss + 10), text.right, text.bottom }, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
		}

		// ─── Join strip (bottom) ───────────────────────────────────────────────
		RECT strip = { 0, pageBottom, w, h };
		Fill(dc, strip, RGB(10, 10, 13), 0);
		Fill(dc, { 0, pageBottom, w, pageBottom + 1 }, line, 0);
		int sy = pageBottom + stripH / 2;

		// Spinner: a rose arc going round a faint ring.
		int sr = S(15), sx = pad + sr;
		HPEN ringPen = CreatePen(PS_SOLID, S(3), RGB(40, 40, 44));
		HPEN arcPen = CreatePen(PS_SOLID, S(3), stage == READY ? ready : rose);
		HGDIOBJ oldPen = SelectObject(dc, ringPen);
		HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
		Ellipse(dc, sx - sr, sy - sr, sx + sr, sy + sr);
		SelectObject(dc, arcPen);
		double a0 = (now % 900) / 900.0 * 6.2831853;
		Arc(dc, sx - sr, sy - sr, sx + sr, sy + sr, sx + static_cast<int>(sr * std::cos(a0)), sy - static_cast<int>(sr * std::sin(a0)),
			sx + static_cast<int>(sr * std::cos(a0 + 1.6)), sy - static_cast<int>(sr * std::sin(a0 + 1.6)));
		SelectObject(dc, oldPen);
		SelectObject(dc, oldBrush);
		DeleteObject(ringPen);
		DeleteObject(arcPen);

		double elapsed = starts[stage] ? (now - starts[stage]) / 1000.0 : (now - begin) / 1000.0;
		double frac = expected[stage] > 0 ? elapsed / expected[stage] : 1;
		double shown = 0.95 * (1 - std::exp(-2.2 * frac));
		std::wstring detail = status;
		if (stage == SCRIPTS)
			detail = L"Garry’s Mod may say “Not Responding” here. It’s working.";
		else if (elapsed > 3 * expected[stage] && elapsed > 60)
			detail = L"This is taking longer than usual.";
		int gapS = S(clampd(12, cw * 0.016, 28));   // .pl-strip gap: clamp(12px, 1.6vw, 28px)
		int tx = sx + sr + gapS, tw = S(clampd(220, cw * 0.22, 380));
		Text(dc, FontFor(S(17), FW_BOLD), white, stage == READY ? L"You’re in! Spawning…" : kStageLines[stage],
			{ tx, sy - S(22), tx + tw, sy }, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
		Text(dc, FontFor(S(13), FW_NORMAL), dim, detail, { tx, sy + S(2), tx + tw, sy + S(22) }, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

		// Buttons on the right, then the clock, then the segments between.
		int bh = S(44);
		RECT cancel = { w - pad - S(92), sy - bh / 2, w - pad, sy + bh / 2 };
		RECT hide = { cancel.left - gapS - S(126), sy - bh / 2, cancel.left - gapS, sy + bh / 2 };
		v.cancelBtn = cancel;
		v.hideBtn = hide;
		Fill(dc, cancel, rose, S(12));
		Fill(dc, hide, RGB(26, 26, 31), S(12));
		Text(dc, FontFor(S(15), FW_SEMIBOLD), white, L"Cancel", cancel, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		Text(dc, FontFor(S(15), FW_SEMIBOLD), dim, L"Game's screen", hide, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

		double remain = expected[stage] - elapsed;
		if (remain < 2)
			remain = 2;
		for (int i = stage + 1; i < STAGES; ++i)
			remain += expected[i];
		ULONGLONG joining = (now - begin) / 1000;
		wchar_t clock[64], since[64];
		swprintf(clock, 64, stage == READY ? L"Ready now" : L"Ready around %s", Clock(static_cast<ULONGLONG>(remain * 1000)).c_str());
		swprintf(since, 64, L"Joining for %llu:%02llu", joining / 60, joining % 60);
		int etaR = hide.left - gapS, etaL = etaR - S(150);
		Text(dc, FontFor(S(16), FW_BOLD), white, clock, { etaL, sy - S(22), etaR, sy }, DT_RIGHT | DT_SINGLELINE);
		Text(dc, FontFor(S(12), FW_NORMAL), faint, since, { etaL, sy + S(3), etaR, sy + S(22) }, DT_RIGHT | DT_SINGLELINE);

		int segL = tx + tw + gapS, segR = etaL - gapS, gap = S(8);
		int segW = (segR - segL - gap * (STAGES - 1)) / STAGES;
		if (segW > S(20))
		{
			for (int i = 0; i < STAGES; ++i)
			{
				RECT s = { segL + i * (segW + gap), sy - S(10), segL + i * (segW + gap) + segW, sy - S(3) };
				Fill(dc, s, track, S(7));
				double f = i < stage ? 1 : i == stage ? shown : 0;
				if (f > 0)
				{
					RECT full = s;
					full.right = s.left + std::max(static_cast<int>(segW * f), S(7));
					Fill(dc, full, stage == READY ? ready : i == stage ? rose : done, S(7));
				}
				Text(dc, FontFor(S(12), FW_NORMAL), i <= stage ? white : faint, kStageNames[i], { s.left, sy + S(2), s.right, sy + S(20) },
					DT_LEFT | DT_SINGLELINE);
			}
		}
	}

	bool Inside(const RECT& r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

	// --- The web page (loading_webview.h) ---------------------------------------

	const wchar_t* const kPageUrl = L"https://meowynetwork.com/pack-loading.html";

	// "http://localhost:<port>" or "http://127.0.0.1:<port>", then the end or a
	// path. The whole host part must be exactly that: a prefix check would let
	// "http://localhost:1@evil.example/" (userinfo, then another host) through.
	// Printable ASCII only, and no backslash (browsers read it as a slash).
	bool IsLocalDevUrl(const std::wstring& url)
	{
		const std::wstring scheme = L"http://";
		if (url.compare(0, scheme.size(), scheme) != 0)
			return false;
		for (wchar_t c : url)
			if (c <= L' ' || c > L'~' || c == L'\\')
				return false;
		const size_t end = url.find_first_of(L"/?#", scheme.size());
		if (end != std::wstring::npos && url[end] != L'/')
			return false;
		const std::wstring authority = url.substr(scheme.size(), end == std::wstring::npos ? std::wstring::npos : end - scheme.size());
		const size_t colon = authority.find(L':');
		if (colon == std::wstring::npos)
			return false;
		const std::wstring host = authority.substr(0, colon), port = authority.substr(colon + 1);
		if (host != L"localhost" && host != L"127.0.0.1")
			return false;
		if (port.empty() || port.size() > 5 || port.find_first_not_of(L"0123456789") != std::wstring::npos)
			return false;
		const long n = std::wcstol(port.c_str(), nullptr, 10);
		return n >= 1 && n <= 65535;
	}

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
		return IsLocalDevUrl(url) ? url : kPageUrl;
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

	// Development aid: with screenshot_debug present in the pack data folder, the screen
	// saves what it draws to its shots subfolder every few seconds (it's a
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

	// The screen's thread. Started at game start (Prewarm) or at the first join,
	// and then kept for the session: the window and the web page stay loaded,
	// hidden, so a join only has to show them. Between joins the page is
	// hidden (WebView2 throttles it) and reloaded fresh for the next one.
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
			g.joining = false;
			if (SUCCEEDED(com))
				CoUninitialize();
			return 0;
		}
		SetLayeredWindowAttributes(v.hwnd, 0, 0, LWA_ALPHA);

		// The web page. If it isn't ready when a join starts, the native drawing
		// shows until it is (up to 8 s into the join); if it failed, the next join
		// tries again.
		bool webOn = SUCCEEDED(com) && loading_webview::Start(v.hwnd, PageUrl(), WebProfileDir());
		bool webShown = false;

		// The page's scale from last time, so the fallback matches it from the
		// first frame (WebView2 only reports it once its controller is up).
		std::wstring scaleFile;
		{
			std::lock_guard<std::mutex> lock(g.m);
			scaleFile = g.dataDir + L"\\css_scale.txt";
		}
		if (FILE* f = _wfopen(scaleFile.c_str(), L"r"))
		{
			double cached = 0;
			if (std::fscanf(f, "%lf", &cached) == 1 && cached >= 0.5 && cached <= 4)
				g_cssScale = cached;
			std::fclose(f);
		}
		ULONGLONG lastPost = 0;

		RECT last = {};
		std::wstring shotDir;
		ULONGLONG countFrom = GetTickCount64(), lastShot = 0;
		int frames = 0, shotNo = 0;
		bool shown = false;
		ULONGLONG shownAt = 0;
		unsigned seenJoin = 0;
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

			bool joining;
			unsigned join;
			{
				std::lock_guard<std::mutex> lock(g.m);
				joining = g.joining;
				join = g.joinSeq;
			}

			// ─── Between joins: hidden, page loaded and waiting ────────────────
			if (!joining)
			{
				if (shown)
				{
					ShowWindow(v.hwnd, SW_HIDE);
					shown = false;
				}
				Sleep(50);
				continue;
			}

			// ─── A join started ────────────────────────────────────────────────
			if (join != seenJoin)
			{
				seenJoin = join;
				shownAt = 0;
				webShown = false;
				lastPost = 0;
				countFrom = now;
				frames = 0;
				std::lock_guard<std::mutex> lock(g.m);
				shotDir.clear();
				if (GetFileAttributesW((g.dataDir + L"\\screenshot_debug").c_str()) != INVALID_FILE_ATTRIBUTES)
				{
					shotDir = g.dataDir + L"\\shots";
					CreateDirectoryW(shotDir.c_str(), nullptr);
				}
				if (!webOn && SUCCEEDED(com))
				{
					webOn = loading_webview::Start(v.hwnd, PageUrl(), WebProfileDir());
					Log(webOn ? "web start" : "web unavailable");
				}
				else
				{
					Log(loading_webview::Ready() ? "web already loaded" : "web loading");
				}
			}

			if (webOn)
			{
				double scale = loading_webview::RasterScale();
				if (scale >= 0.5 && scale <= 4 && std::fabs(scale - g_cssScale) > 0.001)
				{
					g_cssScale = scale;
					if (FILE* f = _wfopen(scaleFile.c_str(), L"w"))
					{
						std::fprintf(f, "%.3f\n", scale);
						std::fclose(f);
					}
				}
				if (!webShown && loading_webview::Ready())
				{
					webShown = true;
					loading_webview::SetVisible(true);
					std::lock_guard<std::mutex> lock(g.m);
					Log("web shown");
				}
				else if (!webShown && loading_webview::Failed())
				{
					loading_webview::Stop();
					webOn = false;
					std::lock_guard<std::mutex> lock(g.m);
					Log("web failed");
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

			// Fading, and the end of the join.
			double alpha;
			bool ended = false;
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
						g.joining = false;
						g.finishing = false;
						CloseLog();
						ended = true;
					}
					alpha = t >= len ? 0.0 : 1.0 - static_cast<double>(t) / len;
				}
				else
				{
					alpha = shown && shownAt ? (now - shownAt >= kFadeInMs ? 1.0 : static_cast<double>(now - shownAt) / kFadeInMs) : 0.0;
				}
			}
			if (ended)
			{
				// Keyboard focus goes back to the game if this screen had it.
				if (g_focusMode)
				{
					if (v.game && GetForegroundWindow() == v.hwnd)
						SetForegroundWindow(v.game);
					SetWindowLongPtrW(v.hwnd, GWL_EXSTYLE, GetWindowLongPtrW(v.hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
					g_focusMode = false;
				}
				ShowWindow(v.hwnd, SW_HIDE);
				SetLayeredWindowAttributes(v.hwnd, 0, 0, LWA_ALPHA);
				shown = false;
				if (webOn)
					loading_webview::Reload();   // fresh for the next join
				continue;
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
						last = pos;
					}
					if (webOn)
						loading_webview::Resize(rc.right, rc.bottom);
					HWND fg = GetForegroundWindow();
					if (fg == v.game || fg == v.hwnd)
						SetWindowPos(v.hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

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
					if (!shown)
					{
						ShowWindow(v.hwnd, SW_SHOWNOACTIVATE);
						shown = true;
						if (!shownAt)
						{
							shownAt = now;
							std::lock_guard<std::mutex> lock(g.m);
							Log(webShown ? "shown (web page)" : "shown (native)");
						}
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
			Sleep(webShown ? 16 : 33);
		}
	}
}

namespace
{
	void StartThreadLocked()
	{
		if (g.running)
			return;
		HANDLE t = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr);
		if (t)
		{
			g.running = true;
			CloseHandle(t);
		}
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

		g.joining = true;
		++g.joinSeq;
		StartThreadLocked();
	}

	void Prewarm(const std::wstring& dataDir)
	{
		std::lock_guard<std::mutex> lock(g.m);
		if (g.dataDir.empty())
			g.dataDir = dataDir;
		StartThreadLocked();
	}

	void Details(const std::string& serverName, const std::string& map, const std::string& steamid)
	{
		std::lock_guard<std::mutex> lock(g.m);
		if (!g.joining)
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
		if (!g.joining || status.empty())
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
		if (!g.joining)
			return;
		Advance(SCRIPTS, GetTickCount64());
		Log("client lua");
	}

	void InGame()
	{
		std::lock_guard<std::mutex> lock(g.m);
		if (!g.joining || g.inGame)
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
		if (!g.joining || g.done)
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
		if (!g.joining)
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
		if (g.joining)
			Log(what, detail);
	}

	bool Active()
	{
		std::lock_guard<std::mutex> lock(g.m);
		return g.joining && !g.finishing;
	}
}

#else

namespace loading_screen
{
	void Begin(const std::wstring&, double) {}
	void Prewarm(const std::wstring&) {}
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
