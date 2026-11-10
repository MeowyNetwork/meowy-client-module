#define _CRT_SECURE_NO_WARNINGS   // plain fopen/fscanf on our own small files
#include "loading_screen.h"

#ifdef _WIN32
#define NOMINMAX   // std::min / std::max below
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include "loading_webview.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <string>
#include <utility>
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
	// Another server (BeginOther): the same stages in general words.
	const wchar_t* const kOtherStageNames[STAGES] = { L"Connect", L"Content", L"Map", L"Scripts", L"Ready" };
	const wchar_t* const kOtherStageLines[STAGES] = {
		L"Connecting to the server",
		L"Getting the server's content",
		L"Loading the map",
		L"Starting the server's scripts",
		L"Almost there",
	};
	// Seconds per stage on a warm join (2026-11-03 profiles), until this PC
	// has its own times in %LOCALAPPDATA%\MeowyClientPack\join_times.txt.
	const double kDefaultSecs[STAGES] = { 6, 4, 22, 30, 4 };
	// Other servers' own times, by address, newest first (join_times_servers.txt).
	constexpr size_t kMaxServerTimes = 50;
	// Fade-in is short: while it runs, GMod's own loading page shows through.
	constexpr ULONGLONG kFadeInMs = 60, kFadeOutMs = 600, kQuickFadeMs = 150;
	// After the loading panel closes, wait this long for the client Lua's Done
	// before fading out anyway. Another server never sends Done: a moment, then out.
	constexpr ULONGLONG kInGameTimeoutMs = 15000, kInGameOtherMs = 1500;
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

	// File work for the screen's thread, which owns all of the screen's files.
	// Queued in order under g.m by whoever asks (the game's thread too, which
	// must never wait on a disk), and done by the screen's thread outside g.m
	// on its next turn.
	struct FileOp
	{
		enum Kind { Line, OpenLog, CloseLog, LoadTimes, SaveTimes } kind = Line;
		std::string text;          // Line: the line, newline included
		std::wstring dir;          // OpenLog, LoadTimes, SaveTimes: the pack data folder
		unsigned join = 0;         // LoadTimes: the join they're for
		bool other = false;        // LoadTimes, SaveTimes: another server's (by address)
		std::string address;
		double t[STAGES] = {};     // SaveTimes: seconds per stage
	};
	// Past this much queued work, log lines are dropped (the screen's thread
	// is stuck); the other work is always queued.
	constexpr size_t kMaxQueuedFiles = 2000;

	struct State
	{
		std::mutex m;
		bool running = false;           // the window thread is alive (for the session, once started)
		bool webWanted = false;         // keep the page loaded between joins (Prewarm; Park clears it)
		unsigned webAsk = 0;            // counts Prewarms that asked anew, so a failed start isn't retried every frame
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
		double expected[STAGES] = {};   // kDefaultSecs until the screen's thread has read this PC's own
		std::wstring server = L"Meowy Network", map, status, steamid;
		bool other = false;             // another server's join: the neutral native screen, no web page
		std::wstring address;           // that server's, for its own stage times
		bool knownTimes = true;         // expected[] is this server's own (else no ETA for another server)
		std::wstring dataDir;
		bool logging = false;           // this join's lines go to last_join.txt in the pack data folder
		std::vector<FileOp> files;      // for the screen's thread to do, in order
	};
	State g;
	std::atomic<bool> g_cancel{ false };
	// Set when the screen's thread has something new to look at (a join, the
	// menu's Prewarm or Park, the join's end); it waits on this between joins.
	// Made once, before the thread, under g.m.
	HANDLE g_wake = nullptr;

	// With g.m held.
	void Wake()
	{
		if (g_wake)
			SetEvent(g_wake);
	}

	// --- Stage times --------------------------------------------------------

	// Other servers' times: "address t0 t1 t2 t3 t4" lines, newest first.
	struct ServerTimes
	{
		std::string address;
		double t[STAGES];
	};

	std::string Ascii(const std::wstring& s)
	{
		std::string out;
		for (wchar_t c : s)
			if (c > L' ' && c <= L'~')
				out += static_cast<char>(c);
		return out;
	}

	// The screen's thread only, like everything below that opens a file.
	std::vector<ServerTimes> ReadServerTimes(const std::wstring& dir)
	{
		std::vector<ServerTimes> list;
		FILE* f = _wfopen((dir + L"\\join_times_servers.txt").c_str(), L"r");
		if (!f)
			return list;
		char address[80];
		ServerTimes e;
		while (list.size() < kMaxServerTimes &&
			std::fscanf(f, "%79s %lf %lf %lf %lf %lf", address, &e.t[0], &e.t[1], &e.t[2], &e.t[3], &e.t[4]) == 1 + STAGES)
		{
			e.address = address;
			list.push_back(e);
		}
		std::fclose(f);
		return list;
	}

	// This PC's own times for the join (BeginJoin has set the defaults): read
	// outside g.m, then put in under it if that join is still the one on screen.
	void LoadExpected(const FileOp& op)
	{
		double v[STAGES];
		bool found = false;
		if (op.other)
		{
			for (const ServerTimes& e : ReadServerTimes(op.dir))
				if (!op.address.empty() && e.address == op.address)
				{
					std::copy(e.t, e.t + STAGES, v);
					found = true;
					break;
				}
		}
		else if (FILE* f = _wfopen((op.dir + L"\\join_times.txt").c_str(), L"r"))
		{
			found = std::fscanf(f, "%lf %lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3], &v[4]) == STAGES;
			std::fclose(f);
		}
		if (!found)
			return;
		std::lock_guard<std::mutex> lock(g.m);
		if (g.joinSeq != op.join)
			return;   // a newer join has its own on the way
		if (op.other)
			g.knownTimes = true;
		for (int i = 0; i < STAGES; ++i)
			if (v[i] > 0.5 && v[i] < 600)
				g.expected[i] = v[i];
	}

	// Called with g.m held, once the player is in. Stages that were seen get
	// this join's time; the others keep what they had. Written by the
	// screen's thread (SaveTimesNow).
	void QueueSaveTimes(ULONGLONG end)
	{
		FileOp op;
		op.kind = FileOp::SaveTimes;
		op.dir = g.dataDir;
		op.other = g.other;
		op.address = Ascii(g.address);
		for (int i = 0; i < STAGES; ++i)
		{
			op.t[i] = g.expected[i];
			if (!g.stageStart[i])
				continue;
			ULONGLONG next = end;
			for (int j = i + 1; j < STAGES; ++j)
				if (g.stageStart[j]) { next = g.stageStart[j]; break; }
			op.t[i] = (next - g.stageStart[i]) / 1000.0;
		}
		g.files.push_back(std::move(op));
	}

	void SaveTimesNow(const FileOp& op)
	{
		CreateDirectoryW(op.dir.c_str(), nullptr);
		if (op.other)
		{
			if (op.address.empty() || op.address.size() > 79)
				return;
			std::vector<ServerTimes> list = ReadServerTimes(op.dir);
			list.erase(std::remove_if(list.begin(), list.end(), [&](const ServerTimes& e) { return e.address == op.address; }), list.end());
			ServerTimes now;
			now.address = op.address;
			std::copy(op.t, op.t + STAGES, now.t);
			list.insert(list.begin(), now);
			if (list.size() > kMaxServerTimes)
				list.resize(kMaxServerTimes);
			FILE* f = _wfopen((op.dir + L"\\join_times_servers.txt").c_str(), L"w");
			if (!f)
				return;
			for (const ServerTimes& e : list)
				std::fprintf(f, "%s %.1f %.1f %.1f %.1f %.1f\n", e.address.c_str(), e.t[0], e.t[1], e.t[2], e.t[3], e.t[4]);
			std::fclose(f);
			return;
		}
		FILE* f = _wfopen((op.dir + L"\\join_times.txt").c_str(), L"w");
		if (!f)
			return;
		std::fprintf(f, "%.1f %.1f %.1f %.1f %.1f\n", op.t[0], op.t[1], op.t[2], op.t[3], op.t[4]);
		std::fclose(f);
	}

	// With g.m held: a line for this join's log, written by the screen's
	// thread on its next turn (so up to a frame late, with its own time).
	void Log(const char* what, const std::string& detail = "")
	{
		if (!g.logging || g.files.size() >= kMaxQueuedFiles)
			return;
		char at[32];
		std::snprintf(at, sizeof(at), "%.2f\t", (GetTickCount64() - g.begin) / 1000.0);
		FileOp op;
		op.text = std::string(at) + what + "\t" + detail + "\n";
		g.files.push_back(std::move(op));
	}

	// With g.m held: this join's log ends (the screen's thread closes the file).
	void EndLogLocked()
	{
		if (!g.logging)
			return;
		g.logging = false;
		FileOp op;
		op.kind = FileOp::CloseLog;
		g.files.push_back(std::move(op));
	}

	FILE* g_log = nullptr;   // last_join.txt, open while a join is logged; the screen's thread only

	// The screen's thread, each turn: takes the queued file work under g.m and
	// does it outside, so neither thread ever waits on g.m behind a disk (or a
	// virus scanner checking a file as it's closed). One flush per turn.
	void DoFileWork()
	{
		std::vector<FileOp> ops;
		{
			std::lock_guard<std::mutex> lock(g.m);
			if (g.files.empty())
				return;
			ops.swap(g.files);
		}
		for (const FileOp& op : ops)
		{
			switch (op.kind)
			{
			case FileOp::Line:
				if (g_log)
					std::fputs(op.text.c_str(), g_log);
				break;
			case FileOp::OpenLog:
				if (g_log)
					std::fclose(g_log);
				CreateDirectoryW(op.dir.c_str(), nullptr);
				g_log = _wfopen((op.dir + L"\\last_join.txt").c_str(), L"w");
				break;
			case FileOp::CloseLog:
				if (g_log)
					std::fclose(g_log);
				g_log = nullptr;
				break;
			case FileOp::LoadTimes:
				LoadExpected(op);
				break;
			case FileOp::SaveTimes:
				SaveTimesNow(op);
				break;
			}
		}
		if (g_log)
			std::fflush(g_log);
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
		unsigned buttonsJoin = 0;   // the join whose native frame put those buttons on screen
		bool repaint = false;       // the system asked for the window to be drawn again (WM_PAINT)
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

	void Text(HDC dc, HFONT f, COLORREF c, const std::wstring& s, RECT r, UINT flags)
	{
		SelectObject(dc, f);
		SetTextColor(dc, c);
		DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, flags | DT_NOPREFIX);
	}

	// Brushes and pens by colour (and width), made once and kept for the
	// session: the two palettes have about twenty colours between them.
	HBRUSH BrushFor(COLORREF c)
	{
		static std::vector<std::pair<COLORREF, HBRUSH>> cache;
		for (auto& b : cache)
			if (b.first == c)
				return b.second;
		HBRUSH b = CreateSolidBrush(c);
		cache.push_back({ c, b });
		return b;
	}

	HPEN PenFor(int width, COLORREF c)
	{
		struct Key
		{
			int width;
			COLORREF c;
		};
		static std::vector<std::pair<Key, HPEN>> cache;
		for (auto& p : cache)
			if (p.first.width == width && p.first.c == c)
				return p.second;
		HPEN p = CreatePen(PS_SOLID, width, c);
		cache.push_back({ { width, c }, p });
		return p;
	}

	void Fill(HDC dc, RECT r, COLORREF c, int radius)
	{
		HGDIOBJ oldB = SelectObject(dc, BrushFor(c));
		HGDIOBJ oldP = SelectObject(dc, GetStockObject(NULL_PEN));
		RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, radius, radius);
		SelectObject(dc, oldP);
		SelectObject(dc, oldB);
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

	// What the drawing shows, copied under g.m once per frame.
	struct Snap
	{
		int stage = CONNECT;
		double expected[STAGES] = {};
		ULONGLONG starts[STAGES] = {}, begin = 0;
		std::wstring server, map, status;
		bool other = false, known = true;
	};

	Snap TakeSnap()
	{
		Snap s;
		std::lock_guard<std::mutex> lock(g.m);
		s.stage = g.stage;
		s.begin = g.begin;
		for (int i = 0; i < STAGES; ++i)
		{
			s.expected[i] = g.expected[i];
			s.starts[i] = g.stageStart[i];
		}
		s.server = g.server;
		s.map = g.map;
		s.status = g.status;
		s.other = g.other;
		s.known = g.knownTimes;
		return s;
	}

	struct Palette
	{
		COLORREF white, dim, faint, rose, rose2, glass, glass2, line, track, done, ready;
	};

	Palette PaletteFor(bool other)
	{
		return { RGB(255, 255, 255), RGB(190, 188, 196), RGB(128, 126, 134),
			other ? RGB(79, 70, 229) : RGB(225, 29, 72), other ? RGB(165, 180, 252) : RGB(251, 113, 133),
			RGB(22, 21, 27), RGB(30, 28, 36),
			RGB(40, 39, 46), RGB(44, 43, 50), RGB(205, 204, 210), RGB(34, 197, 94) };
	}

	double Clampd(double lo, double x, double hi) { return std::max(lo, std::min(x, hi)); }

	// The page's layout in device pixels, for the parts that both halves of
	// the drawing need: the frame and the join strip along the bottom.
	struct Layout
	{
		double k = 1, cw = 0;   // device pixels per CSS pixel; the viewport's width in CSS pixels
		int pad = 0, topH = 0, stripH = 0, pageBottom = 0;
		int sy = 0, sr = 0, sx = 0, gapS = 0, tx = 0, tw = 0, etaL = 0, etaR = 0, segL = 0, segR = 0;
		RECT cancel = {}, hide = {};
		// Everything that changes from frame to frame (the spinner, the stage
		// and status lines, the segments, the clock and "Joining for") is in here.
		RECT live = {};
		int S(double px) const { return static_cast<int>(px * k + 0.5); }
	};

	Layout LayoutFor(int w, int h)
	{
		Layout l;
		l.k = g_cssScale > 0.5 ? g_cssScale : 1.0;
		l.cw = w / l.k;
		l.pad = l.S(Clampd(20, l.cw * 0.032, 56));   // --pl-pad: clamp(20px, 3.2vw, 56px)
		l.topH = l.S(68);
		l.stripH = l.S(84);
		l.pageBottom = h - l.stripH;
		l.sy = l.pageBottom + l.stripH / 2;
		l.sr = l.S(15);
		l.sx = l.pad + l.sr;
		l.gapS = l.S(Clampd(12, l.cw * 0.016, 28));   // .pl-strip gap: clamp(12px, 1.6vw, 28px)
		l.tx = l.sx + l.sr + l.gapS;
		l.tw = l.S(Clampd(220, l.cw * 0.22, 380));
		// Buttons on the right, then the clock, then the segments between.
		const int bh = l.S(44);
		l.cancel = { w - l.pad - l.S(92), l.sy - bh / 2, w - l.pad, l.sy + bh / 2 };
		l.hide = { l.cancel.left - l.gapS - l.S(126), l.sy - bh / 2, l.cancel.left - l.gapS, l.sy + bh / 2 };
		l.etaR = l.hide.left - l.gapS;
		l.etaL = l.etaR - l.S(150);
		l.segL = l.tx + l.tw + l.gapS;
		l.segR = l.etaL - l.gapS;
		// The spinner's ring pen reaches 1.5 CSS px past its circle; the texts
		// clip to their own rectangles, all inside this.
		l.live.left = std::max(0, l.sx - l.sr - l.S(4));
		l.live.top = std::max(l.pageBottom + 1, l.sy - l.S(24));
		l.live.right = std::max(static_cast<int>(l.live.left), std::min(w, l.etaR));
		l.live.bottom = std::max(static_cast<int>(l.live.top), std::min(h, l.sy + l.S(24)));
		return l;
	}

	// Seconds shown in another server's stage row: whole seconds, rounded.
	ULONGLONG RowSecs(ULONGLONG from, ULONGLONG to) { return (to - from + 500) / 1000; }

	// What the static layer was drawn from: when any of it changes, it's drawn
	// again. Everything DrawStatic reads is here.
	struct LayerKey
	{
		int w = 0, h = 0, stage = -1;
		double scale = 0;
		bool other = false, known = false;
		std::wstring server, map;
		double expected[STAGES] = {};
		ULONGLONG starts[STAGES] = {};
		ULONGLONG stageSecs = 0;   // another server: the live stage's seconds, in its row

		bool operator==(const LayerKey& o) const
		{
			return w == o.w && h == o.h && stage == o.stage && scale == o.scale && other == o.other && known == o.known &&
				server == o.server && map == o.map && std::equal(expected, expected + STAGES, o.expected) &&
				std::equal(starts, starts + STAGES, o.starts) && stageSecs == o.stageSecs;
		}
	};

	LayerKey KeyFor(const Snap& s, int w, int h, ULONGLONG now)
	{
		LayerKey key;
		key.w = w;
		key.h = h;
		key.stage = s.stage;
		key.scale = g_cssScale;
		key.other = s.other;
		key.known = s.known;
		key.server = s.server;
		key.map = s.map;
		std::copy(s.expected, s.expected + STAGES, key.expected);
		std::copy(s.starts, s.starts + STAGES, key.starts);
		if (s.other && s.starts[s.stage])
			key.stageSecs = RowSecs(s.starts[s.stage], now);
		return key;
	}

	// The same layout and colours as the web page's Home page (Website
	// pack-loading.css), in its CSS pixels, so the switch to the page when it's
	// ready is barely visible: top bar, server name, the game cards, the strip.
	//
	// Drawn in two layers. DrawStatic draws everything that only changes with
	// the stage, the join's details or the window's size, once, into a bitmap
	// kept between frames; DrawLive draws the strip's moving parts (all inside
	// Layout::live) over a copy of it every frame.
	void DrawStatic(HDC dc, int w, int h, const Snap& s, ULONGLONG now, const Layout& l)
	{
		const int stage = s.stage;
		const bool other = s.other;
		// Another server: neutral (no Meowy brand, games or rose), its own words.
		const wchar_t* const* stageLines = other ? kOtherStageLines : kStageLines;
		SetBkMode(dc, TRANSPARENT);
		// Work in the page's CSS pixels: cw x ch is its viewport.
		auto S = [&](double px) { return l.S(px); };
		const double cw = l.cw;
		const Palette p = PaletteFor(other);

		// Background: near black with a faint tint at the top (rose, or blue for another server).
		TRIVERTEX vert[2] = {
			{ 0, 0, static_cast<COLOR16>(other ? 0x0E00 : 0x1C00), 0x0F00, static_cast<COLOR16>(other ? 0x1E00 : 0x1400), 0xFF00 },
			{ w, h, 0x0A00, 0x0A00, 0x0C00, 0xFF00 },
		};
		GRADIENT_RECT gr = { 0, 1 };
		GradientFill(dc, vert, 2, &gr, 1, GRADIENT_FILL_RECT_V);

		const int pad = l.pad, topH = l.topH;

		// ─── Top bar: brand and the page buttons (the page makes them work) ───
		int cy = topH / 2;
		Fill(dc, { pad, cy - S(14), pad + S(28), cy + S(14) }, p.rose, S(10));
		if (other)
		{
			// The game's name, and whose screen this is (so it can be turned off).
			Text(dc, FontFor(S(15), FW_BOLD), p.white, L"GARRY’S MOD", { pad + S(40), 0, pad + S(260), topH }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
			Text(dc, FontFor(S(13), FW_NORMAL), p.faint, L"Loading screen by the Meowy Client Pack", { w / 2, 0, w - pad, topH },
				DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		}
		else
		{
			Text(dc, FontFor(S(15), FW_BOLD), p.white, L"MEOWY NETWORK", { pad + S(40), 0, pad + S(260), topH }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
			int navX = pad + S(40) + S(178) + S(Clampd(16, cw * 0.03, 48));
			Fill(dc, { navX, cy - S(21), navX + S(4) + 3 * S(76) + S(4), cy + S(21) }, RGB(24, 23, 28), S(42));
			const wchar_t* tabs[] = { L"Home", L"Play", L"City" };
			for (int i = 0; i < 3; ++i)
			{
				RECT t = { navX + S(4) + i * S(76), cy - S(17), navX + S(4) + i * S(76) + S(76), cy + S(17) };
				if (i == 0)
					Fill(dc, t, p.rose, S(36));
				Text(dc, FontFor(S(15), FW_SEMIBOLD), i == 0 ? p.white : p.dim, tabs[i], t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			}
		}

		// ─── Hero (left) ───────────────────────────────────────────────────────
		int pageTop = topH, pageBottom = l.pageBottom;
		int colGap = S(Clampd(20, cw * 0.04, 72));
		int leftW = (w - 2 * pad - colGap) * 11 / 21;
		const int titlePx = S(Clampd(34, cw * 0.042, 76));
		int heroY = pageTop + (pageBottom - pageTop) / 2 - titlePx * 2;
		std::wstring eyebrow = s.map.empty() ? std::wstring(other ? L"JOINING A SERVER" : L"JOINING DOWNTOWN") : L"JOINING " + s.map;
		for (auto& c : eyebrow)
			c = static_cast<wchar_t>(towupper(c));
		Text(dc, FontFor(S(13), FW_BOLD), p.rose2, eyebrow, { pad, heroY, pad + leftW, heroY + S(20) }, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
		RECT title = { pad, heroY + S(26), pad + leftW, heroY + S(26) + titlePx * 7 / 2 };   // up to three lines
		Text(dc, FontFor(titlePx, FW_HEAVY), p.white, s.server, title, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);

		// ─── Game cards (right); for another server, where the time goes ─────
		int rx = pad + leftW + colGap, rw = w - pad - rx;
		if (other)
		{
			// Each stage: done (its time), now (live), or to come (its usual time, if known).
			int rowH = S(52), listTop = pageTop + (pageBottom - pageTop - (S(30) + STAGES * rowH)) / 2;
			Text(dc, FontFor(S(Clampd(13, cw * 0.0095, 16)), FW_BOLD), p.faint, L"WHERE THE TIME GOES", { rx, listTop, rx + rw, listTop + S(24) }, DT_LEFT | DT_SINGLELINE);
			for (int i = 0; i < STAGES; ++i)
			{
				int y = listTop + S(30) + i * rowH;
				RECT row = { rx, y, rx + rw, y + rowH - S(8) };
				Fill(dc, row, i == stage ? p.glass2 : p.glass, S(12));
				Fill(dc, { row.left + S(14), (row.top + row.bottom) / 2 - S(5), row.left + S(24), (row.top + row.bottom) / 2 + S(5) },
					i < stage ? p.done : i == stage ? (stage == READY ? p.ready : p.rose) : p.track, S(10));
				Text(dc, FontFor(S(15), i == stage ? FW_BOLD : FW_NORMAL), i <= stage ? p.white : p.dim, stageLines[i],
					{ row.left + S(38), row.top, row.right - S(110), row.bottom }, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
				wchar_t secs[48] = L"";
				if (s.starts[i] && i <= stage)
				{
					ULONGLONG end = now;
					for (int j = i + 1; j < STAGES; ++j)
						if (s.starts[j]) { end = s.starts[j]; break; }
					swprintf(secs, 48, L"%llu s", RowSecs(s.starts[i], end));   // the live one is in LayerKey::stageSecs
				}
				else if (i > stage && s.known)
				{
					swprintf(secs, 48, L"usually %.0f s", s.expected[i]);
				}
				Text(dc, FontFor(S(13), FW_NORMAL), i == stage ? p.white : p.faint, secs, { row.right - S(110), row.top, row.right - S(14), row.bottom },
					DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
			}
		}
		int cardH = S(108), cardGap = S(12);
		int cardsTop = pageTop + (pageBottom - pageTop - (S(30) + 3 * cardH + 2 * cardGap)) / 2;
		if (!other)
			Text(dc, FontFor(S(Clampd(13, cw * 0.0095, 16)), FW_BOLD), p.faint, L"SOMETHING TO DO WHILE YOU WAIT", { rx, cardsTop, rx + rw, cardsTop + S(24) }, DT_LEFT | DT_SINGLELINE);
		const wchar_t* names[] = { L"Where in Downtown?", L"Rooftop run", L"The city cat" };
		const wchar_t* blurbs[] = {
			L"A photo from somewhere in the city. Find it on the map.",
			L"Leap across the real rooftops of Downtown. You arrive when the game is ready.",
			L"Pet the cat. Head, cheeks, nose, belly or tail: she reacts to all of them.",
		};
		int y0 = cardsTop + S(30);
		for (int i = 0; i < (other ? 0 : 3); ++i)
		{
			int y = y0 + i * (cardH + cardGap);
			if (y + cardH > pageBottom - S(10))
				break;
			Fill(dc, { rx, y, rx + rw, y + cardH }, p.glass, S(16));
			RECT art = { rx + S(12), y + S(12), rx + S(12) + S(132), y + S(12) + S(84) };
			Fill(dc, art, p.glass2, S(12));
			Fill(dc, { art.left, art.bottom - S(4), art.right, art.bottom }, i == 1 ? p.rose : p.glass2, 0);
			RECT text = { art.right + S(18), y + S(22), rx + rw - S(12), y + cardH - S(10) };
			int titleCss = static_cast<int>(Clampd(16, cw * 0.012, 20));
			Text(dc, FontFor(S(titleCss), FW_BOLD), p.white, names[i], { text.left, text.top, text.right, text.top + S(titleCss + 8) }, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
			Text(dc, FontFor(S(14), FW_NORMAL), p.dim, blurbs[i], { text.left, text.top + S(titleCss + 10), text.right, text.bottom }, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
		}

		// ─── Join strip (bottom): its background and buttons; DrawLive does the rest ───
		RECT strip = { 0, pageBottom, w, h };
		Fill(dc, strip, RGB(10, 10, 13), 0);
		Fill(dc, { 0, pageBottom, w, pageBottom + 1 }, p.line, 0);
		v.cancelBtn = l.cancel;
		v.hideBtn = l.hide;
		Fill(dc, l.cancel, p.rose, S(12));
		Fill(dc, l.hide, RGB(26, 26, 31), S(12));
		Text(dc, FontFor(S(15), FW_SEMIBOLD), p.white, L"Cancel", l.cancel, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		Text(dc, FontFor(S(15), FW_SEMIBOLD), p.dim, L"Game's screen", l.hide, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}

	// The strip's moving parts, over the static layer's copy of Layout::live.
	void DrawLive(HDC dc, const Snap& s, ULONGLONG now, const Layout& l)
	{
		const int stage = s.stage;
		const wchar_t* const* stageNames = s.other ? kOtherStageNames : kStageNames;
		const wchar_t* const* stageLines = s.other ? kOtherStageLines : kStageLines;
		SetBkMode(dc, TRANSPARENT);
		auto S = [&](double px) { return l.S(px); };
		const Palette p = PaletteFor(s.other);
		const int sy = l.sy, sr = l.sr, sx = l.sx;

		// Spinner: a rose arc going round a faint ring.
		HGDIOBJ oldPen = SelectObject(dc, PenFor(S(3), RGB(40, 40, 44)));
		HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
		Ellipse(dc, sx - sr, sy - sr, sx + sr, sy + sr);
		SelectObject(dc, PenFor(S(3), stage == READY ? p.ready : p.rose));
		double a0 = (now % 900) / 900.0 * 6.2831853;
		Arc(dc, sx - sr, sy - sr, sx + sr, sy + sr, sx + static_cast<int>(sr * std::cos(a0)), sy - static_cast<int>(sr * std::sin(a0)),
			sx + static_cast<int>(sr * std::cos(a0 + 1.6)), sy - static_cast<int>(sr * std::sin(a0 + 1.6)));
		SelectObject(dc, oldPen);
		SelectObject(dc, oldBrush);

		double elapsed = s.starts[stage] ? (now - s.starts[stage]) / 1000.0 : (now - s.begin) / 1000.0;
		double frac = s.expected[stage] > 0 ? elapsed / s.expected[stage] : 1;
		double shown = 0.95 * (1 - std::exp(-2.2 * frac));
		std::wstring detail = s.status;
		if (stage == SCRIPTS)
			detail = L"Garry’s Mod may say “Not Responding” here. It’s working.";
		else if (elapsed > 3 * s.expected[stage] && elapsed > 60)
			detail = L"This is taking longer than usual.";
		const int tx = l.tx, tw = l.tw;
		Text(dc, FontFor(S(17), FW_BOLD), p.white, stage == READY ? L"You’re in! Spawning…" : stageLines[stage],
			{ tx, sy - S(22), tx + tw, sy }, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
		Text(dc, FontFor(S(13), FW_NORMAL), p.dim, detail, { tx, sy + S(2), tx + tw, sy + S(22) }, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

		double remain = s.expected[stage] - elapsed;
		if (remain < 2)
			remain = 2;
		for (int i = stage + 1; i < STAGES; ++i)
			remain += s.expected[i];
		ULONGLONG joining = (now - s.begin) / 1000;
		wchar_t clock[64], since[64];
		// No guess on a server this PC hasn't joined before: just the time so far.
		if (!s.known && stage != READY)
			swprintf(clock, 64, L"First join here");
		else
			swprintf(clock, 64, stage == READY ? L"Ready now" : L"Ready around %s", Clock(static_cast<ULONGLONG>(remain * 1000)).c_str());
		swprintf(since, 64, L"Joining for %llu:%02llu", joining / 60, joining % 60);
		Text(dc, FontFor(S(16), FW_BOLD), p.white, clock, { l.etaL, sy - S(22), l.etaR, sy }, DT_RIGHT | DT_SINGLELINE);
		Text(dc, FontFor(S(12), FW_NORMAL), p.faint, since, { l.etaL, sy + S(3), l.etaR, sy + S(22) }, DT_RIGHT | DT_SINGLELINE);

		int segL = l.segL, segR = l.segR, gap = S(8);
		int segW = (segR - segL - gap * (STAGES - 1)) / STAGES;
		if (segW > S(20))
		{
			for (int i = 0; i < STAGES; ++i)
			{
				RECT seg = { segL + i * (segW + gap), sy - S(10), segL + i * (segW + gap) + segW, sy - S(3) };
				Fill(dc, seg, p.track, S(7));
				double f = i < stage ? 1 : i == stage ? shown : 0;
				if (f > 0)
				{
					RECT full = seg;
					full.right = seg.left + std::max(static_cast<int>(segW * f), S(7));
					Fill(dc, full, stage == READY ? p.ready : i == stage ? p.rose : p.done, S(7));
				}
				Text(dc, FontFor(S(12), FW_NORMAL), i <= stage ? p.white : p.faint, stageNames[i], { seg.left, sy + S(2), seg.right, sy + S(20) },
					DT_LEFT | DT_SINGLELINE);
			}
		}
	}

	// A bitmap selected into its own memory DC, with the bitmap the DC came
	// with put back before it's deleted (GDI won't delete a bitmap that's
	// selected into a DC).
	struct Surface
	{
		HDC dc = nullptr;
		HBITMAP bmp = nullptr;
		HGDIOBJ original = nullptr;

		bool Make(HDC screen, int w, int h)
		{
			HDC newDc = CreateCompatibleDC(screen);
			HBITMAP newBmp = newDc ? CreateCompatibleBitmap(screen, w, h) : nullptr;
			if (!newBmp)
			{
				if (newDc)
					DeleteDC(newDc);
				return false;
			}
			dc = newDc;
			bmp = newBmp;
			original = SelectObject(dc, bmp);
			return true;
		}

		void Free()
		{
			if (!dc)
				return;
			SelectObject(dc, original);
			DeleteObject(bmp);
			DeleteDC(dc);
			dc = nullptr;
			bmp = nullptr;
			original = nullptr;
		}
	};

	// The native drawing's buffers, as big as the window (8 MB each at 1080p):
	// `layer` holds DrawStatic's drawing, `frame` the layer plus the live parts,
	// which is what goes to the window. Freed while the web page shows and
	// between joins.
	struct Native
	{
		Surface layer, frame;
		int w = 0, h = 0;
		bool layerOk = false;
		LayerKey key;

		// Tried again next frame if it fails (a huge window, low memory).
		bool Ensure(HDC screen, int width, int height)
		{
			if (frame.dc && width == w && height == h)
				return true;
			Free();
			if (!layer.Make(screen, width, height))
				return false;
			if (!frame.Make(screen, width, height))
			{
				layer.Free();
				return false;
			}
			w = width;
			h = height;
			return true;
		}

		void Free()
		{
			layer.Free();
			frame.Free();
			w = h = 0;
			layerOk = false;
		}
	};

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
			// A click on the last join's buttons (still on screen, or queued
			// before it ended) is for no join: it can't cancel a new one.
			if (!g.joining || v.buttonsJoin != g.joinSeq)
				return 0;
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
		case WM_PAINT:
		{
			// The system wants the window drawn again (it was shown, or what DWM
			// held for it was lost): marked done here, and the next frame puts all
			// of the native drawing on screen, not only its live parts.
			PAINTSTRUCT ps;
			BeginPaint(hwnd, &ps);
			EndPaint(hwnd, &ps);
			v.repaint = true;
			return 0;
		}
		case WM_ERASEBKGND:
			return 1;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	bool IsTopmost(HWND hwnd)
	{
		return (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
	}

	// Keeps the screen over the game, and over nothing else. Called every frame
	// while it covers the game; only moves the window when it's out of place.
	//
	// The game (with no popup of its own up) or this screen in front: topmost,
	// at the bottom of the topmost band. HWND_TOP from this thread can't move
	// the window above the active one, so it only worked on a join where this
	// window came up before the game's (launching straight into the server); a
	// second join in the same session (retry, the server browser) left it under
	// the game, showing GMod's own loading page. Topmost works from any thread,
	// and at the bottom of its band other programs' always-on-top windows (Task
	// Manager, say) stay above it. An owned window would also stay on top, but
	// would share the game's input thread and freeze with it during loads.
	//
	// Anything else in front (another program, or a window of the game's own
	// such as an error box): directly above the game in the Z order, so
	// everything above the game, the foreground window included, stays above
	// this screen and alt-tab behaves as usual. (HWND_NOTOPMOST alone would put
	// the screen above all of them.)
	void KeepAboveGame()
	{
		const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
		const bool gameTopmost = IsTopmost(v.game);
		HWND fg = GetForegroundWindow();
		HWND popup = GetWindow(v.game, GW_ENABLEDPOPUP);
		const bool gamePopup = popup && popup != v.game && IsWindowVisible(popup);
		const bool front = (fg == v.game && !gamePopup) || fg == v.hwnd;

		if (front && !gameTopmost)
		{
			HWND below = GetWindow(v.hwnd, GW_HWNDNEXT);
			if (IsTopmost(v.hwnd) && (!below || !IsTopmost(below)))
				return;   // already the lowest topmost window
			if (!IsTopmost(v.hwnd))
				SetWindowPos(v.hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
			HWND lowest = nullptr;
			for (HWND w = GetTopWindow(nullptr); w && IsTopmost(w); w = GetWindow(w, GW_HWNDNEXT))
				if (w != v.hwnd)
					lowest = w;
			if (lowest)
				SetWindowPos(v.hwnd, lowest, 0, 0, 0, 0, flags);
			return;
		}

		// Directly above the game, in its band: whatever is just above the game
		// (leaving this screen out) ends up just above this screen.
		HWND above = GetWindow(v.game, GW_HWNDPREV);
		if (above == v.hwnd && IsTopmost(v.hwnd) == gameTopmost)
			return;   // already there
		if (above == v.hwnd)
			above = GetWindow(v.hwnd, GW_HWNDPREV);
		HWND after;
		if (above && IsTopmost(above) == gameTopmost)
			after = above;           // just under it, so just over the game
		else if (gameTopmost)
			after = HWND_TOPMOST;    // the game is the top window of all
		else                         // the game is the top normal window: nothing normal is above it
			after = IsTopmost(v.hwnd) ? HWND_NOTOPMOST : HWND_TOP;
		SetWindowPos(v.hwnd, after, 0, 0, 0, 0, flags);
	}

	double QpcMs()
	{
		static const double perMs = [] {
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			return f.QuadPart / 1000.0;
		}();
		LARGE_INTEGER c;
		QueryPerformanceCounter(&c);
		return c.QuadPart / perMs;
	}

	// Waits up to `ms` for g_wake or a message for this thread (WebView2's
	// completions and events come as messages, and so does input).
	//
	// Between joins (an endless wait) it also wakes for input already waiting
	// that something looked at without taking (MWMO_INPUTAVAILABLE), so none is
	// left there for good. During a join it doesn't: the page's browser window
	// is a child of this one, which shares the input queue with the browser's
	// thread, and that flag could wake this thread again and again for input
	// only the browser can take (the mouse moving over the page). Every message
	// for this thread is taken before each wait, and a join's waits last a frame
	// at most, so nothing waits longer than that.
	void WaitForWork(DWORD ms, bool joining)
	{
		const DWORD flags = joining ? 0 : MWMO_INPUTAVAILABLE;
		if (g_wake)
			MsgWaitForMultipleObjectsEx(1, &g_wake, ms, QS_ALLINPUT, flags);
		else   // no event (CreateEvent failed): look again now and then
			MsgWaitForMultipleObjectsEx(0, nullptr, ms == INFINITE ? 50 : ms, QS_ALLINPUT, flags);
	}

	// A page that failed while hidden between joins is started again after
	// these waits, at most this many times per visit to the menu (Prewarm).
	constexpr ULONGLONG kRestartWaitMs[] = { 10000, 60000, 60000 };
	constexpr int kMaxRestarts = 3;

	// The screen's thread. Started at the menu (Prewarm) or at the first join,
	// and then kept for the session: the window stays, hidden, and the web page
	// is kept loaded while the menu wants it (Prewarm), so a join only has to
	// show it. Between joins it sleeps until something changes (g_wake or a
	// message); the page is hidden (WebView2 throttles it) and, if a join
	// showed it, reloaded fresh at the menu or at the next join; parked (Park:
	// in game, or the menu in the background), it's stopped and its processes
	// freed instead.
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
			{
				std::lock_guard<std::mutex> lock(g.m);
				g.joining = false;
				EndLogLocked();
			}
			// What this join logged so far, and the log closed, while this is still
			// the only screen's thread: a join can't start another until running
			// is cleared below.
			DoFileWork();
			{
				std::lock_guard<std::mutex> lock(g.m);
				g.running = false;
				// A join that began meanwhile ends here too; its file work waits
				// for the next thread.
				g.joining = false;
				EndLogLocked();
			}
			if (SUCCEEDED(com))
				CoUninitialize();
			return 0;
		}
		SetLayeredWindowAttributes(v.hwnd, 0, 0, LWA_ALPHA);
		int lastAlpha = 0;   // the alpha byte last set: it's only set again when it changes

		// The web page: started between joins when Prewarm asks, or by a join. If
		// it isn't ready when a join starts, the native drawing shows until it is;
		// if it fails while showing (its browser died), the native drawing takes
		// over and the next join starts it again; if it fails while hidden at the
		// menu, it's started again a little later. Parked, it's stopped once no
		// join is on screen.
		bool webOn = false;
		bool webShown = false;      // the page is this join's screen (on screen unless the game is minimised)
		bool usedThisJoin = false;  // the page was on screen in this join
		bool pageStale = false;     // a join showed it since its last load: reloaded before it's shown again
		bool atMenu = true;         // no join has reached the game since the menu last asked for the page
		unsigned webTried = 0, seenAsk = 0;
		int restarts = 0;
		ULONGLONG restartAt = 0;
		bool restartAfterJoin = false;   // it failed during this join: started again a little after it

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
		bool iconic = false;      // the game is minimised: the screen (and the page) are hidden
		ULONGLONG shownAt = 0;
		unsigned seenJoin = 0;
		bool joinOpen = false;    // a join is on screen that hasn't been ended (EndPrevious) yet
		bool joinOther = false;   // this join is to another server: native drawing only, never the web page
		Native native;
		bool fullBlit = true;     // the window needs all of the native frame, not only its live parts
		double lastFrameMs = 0;
		bool frameNow = true;
		// Native paint and blit times for the 'frames' log line.
		double paintMs = 0, paintMaxMs = 0;
		int painted = 0, paintedFull = 0;

		// The join on screen is over: faded out, or replaced by a new join
		// while it was still up. Keyboard focus goes back to the game, the page
		// is hidden (and marked to be reloaded if this join showed it), and the
		// native buffers are freed. The window itself is left to the caller.
		auto EndPrevious = [&]() {
			joinOpen = false;
			// Keyboard focus goes back to the game if this screen had it.
			if (g_focusMode)
			{
				if (v.game && GetForegroundWindow() == v.hwnd)
					SetForegroundWindow(v.game);
				SetWindowLongPtrW(v.hwnd, GWL_EXSTYLE, GetWindowLongPtrW(v.hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
				g_focusMode = false;
			}
			if (webOn)
			{
				loading_webview::SetVisible(false);
				// Fresh for the next join, but not now: a reload on the frame the
				// player spawns would be a hitch. Never for a join that didn't
				// show it (another server's, or one ended before it was ready).
				if (usedThisJoin)
					pageStale = true;
			}
			webShown = false;
			usedThisJoin = false;
			loading_webview::TakeActions();   // a click in its last frame is for no join
			native.Free();
		};

		for (;;)
		{
			MSG msg;
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
			if (v.repaint)
			{
				// WM_PAINT: all of the window on the next frame, and that frame now.
				v.repaint = false;
				fullBlit = frameNow = true;
			}
			loading_webview::Poll();
			DoFileWork();
			ULONGLONG now = GetTickCount64();

			bool joining;
			unsigned join;
			{
				std::lock_guard<std::mutex> lock(g.m);
				joining = g.joining;
				join = g.joinSeq;
			}

			// ─── Between joins: hidden, page loaded and waiting (or parked) ─────
			if (!joining)
			{
				if (shown)
				{
					ShowWindow(v.hwnd, SW_HIDE);
					shown = false;
				}
				loading_webview::TakeActions();   // nothing is on screen to ask
				bool wanted;
				unsigned ask;
				{
					std::lock_guard<std::mutex> lock(g.m);
					wanted = g.webWanted;
					ask = g.webAsk;
				}
				if (ask != seenAsk)
				{
					// The menu asked for the page anew: at the menu, with fresh tries.
					seenAsk = ask;
					atMenu = true;
					restarts = 0;
					restartAt = 0;
				}
				// Outside g.m, as at a join's start: WebView2 may pump this thread's messages.
				if (webOn && !wanted)
				{
					loading_webview::Stop();
					webOn = false;
					pageStale = false;
					restartAt = 0;
				}
				else if (webOn && wanted && loading_webview::Failed())
				{
					// Failed while hidden (its browser died, or its reload
					// failed): started again a little later, so the next join
					// doesn't have to cold-start it. The join still does, as a
					// last resort.
					loading_webview::Stop();
					webOn = false;
					pageStale = false;
					restartAt = restarts < kMaxRestarts ? now + kRestartWaitMs[restarts] : 0;
				}
				else if (webOn && wanted && pageStale && atMenu)
				{
					pageStale = false;
					loading_webview::Reload();   // fresh for the next join
				}
				else if (!webOn && wanted && SUCCEEDED(com) && (ask != webTried || (restartAt && now >= restartAt)))
				{
					if (ask == webTried)
						++restarts;
					webTried = ask;
					restartAt = 0;
					webOn = loading_webview::Start(v.hwnd, PageUrl(), WebProfileDir());
				}
				DWORD wait = INFINITE;
				if (!webOn && wanted && restartAt)
					wait = now >= restartAt ? 0 : static_cast<DWORD>(restartAt - now);
				if (loading_webview::Pending())
					wait = std::min<DWORD>(wait, 100);   // a Start waiting for the old browser to go
				WaitForWork(wait, false);
				continue;
			}

			// ─── A join started ────────────────────────────────────────────────
			if (join != seenJoin)
			{
				seenJoin = join;
				shownAt = 0;
				if (joinOpen)
				{
					// The last join was still on screen (a map change or retry
					// right after spawning, a quick hop to another server): it
					// ends here, and the window stays up, opaque, for this one.
					EndPrevious();
					if (shown)
						shownAt = now - kFadeInMs;
				}
				joinOpen = true;
				webShown = false;
				usedThisJoin = false;
				lastPost = 0;
				countFrom = now;
				frames = 0;
				paintMs = paintMaxMs = 0;
				painted = paintedFull = 0;
				fullBlit = frameNow = true;
				loading_webview::TakeActions();   // anything older is for no join
				std::wstring dataDir;
				{
					std::lock_guard<std::mutex> lock(g.m);
					dataDir = g.dataDir;
					joinOther = g.other;
				}
				shotDir.clear();
				if (GetFileAttributesW((dataDir + L"\\screenshot_debug").c_str()) != INVALID_FILE_ATTRIBUTES)
				{
					shotDir = dataDir + L"\\shots";
					CreateDirectoryW(shotDir.c_str(), nullptr);
				}
				// A page that failed since the last join (its browser died while
				// hidden, or the reload after that join failed, and it wasn't
				// started again yet) is started afresh now, so this join can
				// still get it.
				if (webOn && loading_webview::Failed())
				{
					loading_webview::Stop();
					webOn = false;
					pageStale = false;
				}
				restartAt = 0;   // this join starts it if it needs it
				restartAfterJoin = false;
				// Outside g.m: starting WebView2 loads its DLL and may launch its
				// browser, and the game thread takes g.m every frame. It may also
				// pump this thread's messages, and WndProc takes g.m.
				const char* web;
				if (joinOther)
				{
					// The page is Meowy Network's; another server gets the neutral native screen.
					web = "web not used (another server)";
				}
				else if (!webOn && SUCCEEDED(com))
				{
					webOn = loading_webview::Start(v.hwnd, PageUrl(), WebProfileDir());
					web = !webOn ? "web unavailable" : loading_webview::Pending() ? "web start (after the last browser closes)" : "web start";
				}
				else if (webOn && pageStale)
				{
					// Shown by the last join and not reloaded since (in game, or
					// replaced by this join): reloaded now, the native drawing meanwhile.
					pageStale = false;
					loading_webview::Reload();
					web = "web reload";
				}
				else
				{
					web = !webOn ? "web unavailable" : loading_webview::Ready() ? "web already loaded" : "web loading";
				}
				std::lock_guard<std::mutex> lock(g.m);
				Log(web);
			}

			if (webOn && !joinOther)
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
				if (loading_webview::Failed())
				{
					// Before it showed, or while showing (its browser or page
					// process died): the native drawing, with its own Cancel,
					// takes over from the next frame.
					const bool wasShown = webShown;
					loading_webview::Stop();
					webOn = false;
					webShown = false;
					pageStale = false;
					fullBlit = frameNow = true;
					restartAfterJoin = true;
					std::lock_guard<std::mutex> lock(g.m);
					Log(wasShown ? "web failed while shown" : "web failed");
				}
				else if (!webShown && loading_webview::Ready())
				{
					webShown = usedThisJoin = true;
					loading_webview::SetVisible(!iconic);
					native.Free();   // the page covers it from now on
					std::lock_guard<std::mutex> lock(g.m);
					Log("web shown");
				}
				// Each request since the last turn (several can come at once).
				const unsigned actions = loading_webview::TakeActions();
				if (actions & (loading_webview::kCancel | loading_webview::kHide))
				{
					std::lock_guard<std::mutex> lock(g.m);
					if (actions & loading_webview::kCancel)
						g_cancel = true;
					Log((actions & loading_webview::kCancel) ? "cancel" : "hide");
					FinishLocked(true);
				}
				if (actions & loading_webview::kHold)
				{
					std::lock_guard<std::mutex> lock(g.m);
					if (!g.holdUntil)
					{
						g.holdUntil = now + kHoldMs;
						Log("hold");
					}
				}
				if ((actions & loading_webview::kFocus) && !g_focusMode)
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
			bool ended = false, reachedGame = false;
			{
				std::lock_guard<std::mutex> lock(g.m);
				if (!g.finishing && g.done && now - g.doneAt > 300 && now >= g.holdUntil)
					FinishLocked(false);
				if (!g.finishing && g.inGame && !g.done && now - g.inGameAt > (g.other ? kInGameOtherMs : kInGameTimeoutMs))
					FinishLocked(false);
				if (g.finishing)
				{
					ULONGLONG len = g.quick ? kQuickFadeMs : kFadeOutMs;
					ULONGLONG t = now - g.finishAt;
					if (t >= len)
					{
						g.joining = false;
						g.finishing = false;
						reachedGame = g.inGame;
						EndLogLocked();
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
				EndPrevious();
				// In game now, or still at the menu (cancelled, failed): a page
				// this join showed is reloaded at once at the menu, but in game
				// only at the next join.
				atMenu = !reachedGame;
				// A page that failed during this join is started again a little
				// after it (if it's still wanted then), as for one that fails
				// while hidden, so the next join doesn't have to cold-start it.
				if (restartAfterJoin)
				{
					restartAfterJoin = false;
					restartAt = restarts < kMaxRestarts ? now + kRestartWaitMs[restarts] : 0;
				}
				ShowWindow(v.hwnd, SW_HIDE);
				SetLayeredWindowAttributes(v.hwnd, 0, 0, LWA_ALPHA);
				lastAlpha = 0;
				shown = false;
				// Hidden first, so dropping topmost can't put it over anything;
				// the next join places it again (KeepAboveGame) before showing it.
				if (IsTopmost(v.hwnd))
					SetWindowPos(v.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
				continue;   // the between-joins turn writes the log's last lines, and stops a parked page
			}

			// A frame every 16 ms with the page (it draws itself; this only keeps
			// the window in place and fades it), every 33 ms when drawn natively.
			// Messages (input, WebView2) wake the loop in between; they're
			// handled, but don't draw an extra frame.
			const double period = webShown ? 16 : 33;
			if (frameNow || QpcMs() - lastFrameMs >= period - 2)
			{
				frameNow = false;
				lastFrameMs = QpcMs();
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
					iconic = IsIconic(v.game) != 0 || rc.right < 64 || rc.bottom < 64;
					if (iconic)
					{
						if (shown)
							ShowWindow(v.hwnd, SW_HIDE);
						shown = false;
						fullBlit = true;
						// The page stops drawing (and its games stop running) while minimised.
						if (webShown)
							loading_webview::SetVisible(false);
					}
					else
					{
						if (std::memcmp(&pos, &last, sizeof(pos)) != 0)
						{
							SetWindowPos(v.hwnd, nullptr, pos.left, pos.top, rc.right, rc.bottom, SWP_NOACTIVATE | SWP_NOZORDER);
							last = pos;
							fullBlit = true;
						}
						// Only sent to WebView2 when the size changed, or when the
						// browser is new (it was made at the window's size then,
						// 1x1 if at the menu).
						if (webOn)
							loading_webview::Resize(rc.right, rc.bottom);
						// Over the game and nothing else; placed before it's shown.
						KeepAboveGame();
						if (webShown)
							loading_webview::SetVisible(true);
						// Shown before the native drawing: drawing into a hidden
						// window is lost, so the frame it's shown on puts all of it
						// on screen. Shown see-through (a restore after a minimise
						// would otherwise show what the window held before it for
						// a moment); the alpha is set again at the end of the frame.
						if (!shown)
						{
							if (lastAlpha != 0)
							{
								SetLayeredWindowAttributes(v.hwnd, 0, 0, LWA_ALPHA);
								lastAlpha = 0;
							}
							ShowWindow(v.hwnd, SW_SHOWNOACTIVATE);
							shown = true;
							fullBlit = true;
							if (!shownAt)
							{
								shownAt = now;
								std::lock_guard<std::mutex> lock(g.m);
								Log(webShown ? "shown (web page)" : "shown (native)");
							}
						}

						int w = rc.right, h = rc.bottom;
						if (!webShown)
						{
							// Native: the static layer (drawn again only when what it
							// shows changed), then the live parts over a copy of it.
							// Only those go to the window, unless all of it must
							// (the layer changed, the window moved or was hidden,
							// a fade: alpha changes each frame anyway).
							const double t0 = QpcMs();
							HDC dc = GetDC(v.hwnd);
							if (dc && native.Ensure(dc, w, h))
							{
								const Snap s = TakeSnap();
								const Layout l = LayoutFor(w, h);
								const LayerKey key = KeyFor(s, w, h, now);
								bool full = fullBlit || alpha < 1.0;
								if (!native.layerOk || !(key == native.key))
								{
									DrawStatic(native.layer.dc, w, h, s, now, l);
									native.key = key;
									native.layerOk = true;
									full = true;
								}
								const RECT r = full ? RECT{ 0, 0, w, h } : l.live;
								const int rw = r.right - r.left, rh = r.bottom - r.top;
								BitBlt(native.frame.dc, r.left, r.top, rw, rh, native.layer.dc, r.left, r.top, SRCCOPY);
								DrawLive(native.frame.dc, s, now, l);
								BitBlt(dc, r.left, r.top, rw, rh, native.frame.dc, r.left, r.top, SRCCOPY);
								fullBlit = false;
								if (full)
									v.buttonsJoin = seenJoin;   // its Cancel and Game's screen are on screen now
								const double ms = QpcMs() - t0;
								paintMs += ms;
								paintMaxMs = std::max(paintMaxMs, ms);
								++painted;
								if (full)
									++paintedFull;
							}
							if (dc)
								ReleaseDC(v.hwnd, dc);
						}
						++frames;
						if (!shotDir.empty() && now - lastShot >= 4000)
						{
							wchar_t name[32];
							swprintf(name, 32, webShown ? L"\\shot_%02d.png" : L"\\shot_%02d.bmp", shotNo++);
							if (webShown)
								loading_webview::Capture(shotDir + name);
							else if (native.frame.dc)
								SaveBmp(native.frame.dc, native.frame.bmp, w, h, shotDir + name);
							lastShot = now;
						}
						const int a = static_cast<int>(alpha * 255);
						if (a != lastAlpha)
						{
							SetLayeredWindowAttributes(v.hwnd, 0, static_cast<BYTE>(a), LWA_ALPHA);
							lastAlpha = a;
						}
					}
				}
			}
			// Proof the screen keeps going while the game is busy: frames drawn
			// per 5 s, and what the native ones cost (paint and blit).
			if (now - countFrom >= 5000)
			{
				std::string detail = std::to_string(frames);
				if (painted)
				{
					char buf[96];
					std::snprintf(buf, sizeof(buf), " (native paint %.2f ms avg, %.2f ms max, %d of %d full)", paintMs / painted, paintMaxMs,
						paintedFull, painted);
					detail += buf;
				}
				std::lock_guard<std::mutex> lock(g.m);
				Log("frames", detail);
				frames = 0;
				paintMs = paintMaxMs = 0;
				painted = paintedFull = 0;
				countFrom = now;
			}
			const double left = period - (QpcMs() - lastFrameMs);
			WaitForWork(left <= 0 ? 0 : static_cast<DWORD>(std::ceil(left)), true);
		}
	}
}

namespace
{
	void StartThreadLocked()
	{
		if (g.running)
			return;
		if (!g_wake)
			g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto-reset
		HANDLE t = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr);
		if (t)
		{
			g.running = true;
			CloseHandle(t);
		}
	}
}

// The game's thread calls these. Each only changes g under g.m and queues
// file work for the screen's thread: no file is opened, written or flushed here.
namespace loading_screen
{
	void BeginJoin(const std::wstring& dataDir, double panelOpenMs, bool other, const std::wstring& server, const std::wstring& map,
		const std::wstring& address)
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
		g.other = other;
		g.server = server;
		g.map = map;
		g.address = address;
		g.steamid.clear();
		g.status = L"Connecting…";
		g_cancel = false;
		// The usual times until the screen's thread has read this PC's own (a
		// frame later): no ETA for another server until then.
		for (int i = 0; i < STAGES; ++i)
			g.expected[i] = kDefaultSecs[i];
		g.knownTimes = !other;

		g.joining = true;
		++g.joinSeq;

		// A new last_join.txt (the last join's, if it's still open, is closed first).
		FileOp open;
		open.kind = FileOp::OpenLog;
		open.dir = dataDir;
		g.files.push_back(std::move(open));
		g.logging = true;
		Log(other ? "begin (another server)" : "begin", "GMod's loading panel open " + std::to_string(static_cast<long long>(panelOpenMs)) + " ms");
		FileOp load;
		load.kind = FileOp::LoadTimes;
		load.dir = dataDir;
		load.join = g.joinSeq;
		load.other = other;
		load.address = Ascii(address);
		g.files.push_back(std::move(load));

		StartThreadLocked();
		Wake();
	}

	void Begin(const std::wstring& dataDir, double panelOpenMs)
	{
		BeginJoin(dataDir, panelOpenMs, false, L"Meowy Network", L"", L"");
	}

	void BeginOther(const std::wstring& dataDir, double panelOpenMs, const std::string& serverName, const std::string& map,
		const std::string& address)
	{
		std::wstring name = Widen(serverName);
		for (wchar_t& c : name)
			if (c == L'\r' || c == L'\n' || c == L'\t')
				c = L' ';
		BeginJoin(dataDir, panelOpenMs, true, name.empty() ? Widen(address) : name, Widen(map), Widen(address));
	}

	void Prewarm(const std::wstring& dataDir)
	{
		std::lock_guard<std::mutex> lock(g.m);
		if (g.dataDir.empty())
			g.dataDir = dataDir;
		if (!g.webWanted)
		{
			g.webWanted = true;
			++g.webAsk;
		}
		StartThreadLocked();
		Wake();
	}

	void Park()
	{
		std::lock_guard<std::mutex> lock(g.m);
		g.webWanted = false;
		Wake();
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
		// Another server never sends Done: its times are kept from here.
		if (g.other && !g.quick)
			QueueSaveTimes(now);
		Log("in game");
		Wake();
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
			QueueSaveTimes(now);
		Log("done");
		Wake();
	}

	void End()
	{
		std::lock_guard<std::mutex> lock(g.m);
		if (!g.joining)
			return;
		Log("end");
		FinishLocked(true);
		Wake();
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
	void BeginOther(const std::wstring&, double, const std::string&, const std::string&, const std::string&) {}
	void Prewarm(const std::wstring&) {}
	void Park() {}
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
