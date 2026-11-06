#include "connect_watch.h"
#include "server_rep.h"

#include "server_gate.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <mutex>
#include <cstring>
#include <cwchar>
#include <string>

namespace
{
	struct LoggingContext;   // tier0's LoggingContext_t; only passed through

	// tier0's ILoggingListener: one virtual, Log(context, message).
	class Listener
	{
	public:
		virtual void Log(const LoggingContext* context, const char* message);
	};

	std::atomic<bool> g_ours{ false };
	std::atomic<unsigned> g_lines{ 0 };
	bool g_registered = false;
	// Written on the logging thread, read on the main thread.
	std::mutex g_addressMutex;
	std::string g_lastAddress;
	std::atomic<ULONGLONG> g_at{ 0 };
	// The engine's last "Disconnected: <reason>" line (a join refused, a kick,
	// a timeout) and when; written on the logging thread.
	std::string g_lastDisconnect;
	std::atomic<ULONGLONG> g_disconnectAt{ 0 };

	void Note(const std::string& address)
	{
		g_ours = server_gate::IsOurAddress(address);
		{
			std::lock_guard<std::mutex> lock(g_addressMutex);
			g_lastAddress = address;
		}
		g_at = GetTickCount64();
	}

	std::string Last()
	{
		std::lock_guard<std::mutex> lock(g_addressMutex);
		return g_lastAddress;
	}

	// The address after `prefix` at the start of `message` (the engine's own
	// line; not chat or a server's print that merely contains it), up to
	// "...", a space or the line end.
	bool AddressAfter(const char* message, const char* prefix, std::string& address)
	{
		const size_t len = std::strlen(prefix);
		if (std::strncmp(message, prefix, len) != 0)
			return false;
		const char* at = message;
		address.clear();
		for (const char* p = at + std::strlen(prefix); *p && address.size() < 64; ++p)
		{
			if ((p[0] == '.' && p[1] == '.' && p[2] == '.') || *p == ' ' || *p == '\r' || *p == '\n')
				break;
			address += *p;
		}
		return !address.empty();
	}

	void Listener::Log(const LoggingContext*, const char* message)
	{
		// "Commencing connection retry to 138.201.62.52:27015" (retry, ~0.4 s
		// before the next line) and "Connecting to 138.201.62.52:27015..."
		// (connect, the server browser, steam://connect).
		++g_lines;
		if (!message)
			return;
		server_rep::Capture(message);
		std::string address;
		if (AddressAfter(message, "Commencing connection retry to ", address) || AddressAfter(message, "Connecting to ", address))
			Note(address);
		static const char kDisconnected[] = "Disconnected: ";
		if (std::strncmp(message, kDisconnected, sizeof(kDisconnected) - 1) == 0)
		{
			std::string reason;
			for (const char* p = message + sizeof(kDisconnected) - 1; *p && *p != '\r' && *p != '\n' && reason.size() < 128; ++p)
				reason += *p;
			{
				std::lock_guard<std::mutex> lock(g_addressMutex);
				g_lastDisconnect = reason;
			}
			g_disconnectAt = GetTickCount64();
		}
	}

	Listener g_listener;
}

namespace connect_watch
{
	void Start()
	{
		static bool started = false;
		if (started)
			return;
		started = true;

		// Launched to join a server (Steam's join links and "+connect a.b.c.d:port"):
		// the address is on the command line before the engine prints anything.
		const wchar_t* cmd = GetCommandLineW();
		if (const wchar_t* c = std::wcsstr(cmd, L"+connect "))
		{
			std::string address;
			for (c += 9; *c && *c != L' ' && *c != L'"' && address.size() < 64; ++c)
				address += static_cast<char>(*c < 128 ? *c : '?');
			Note(address);
		}

		HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
		if (!tier0)
			return;
		using Register = void (*)(Listener*);
		auto reg = reinterpret_cast<Register>(GetProcAddress(tier0, "LoggingSystem_RegisterLoggingListener"));
		if (reg)
		{
			reg(&g_listener);   // the DLL is pinned, so the listener outlives the game's use of it
			g_registered = true;
		}
	}

	std::string Describe()
	{
		ULONGLONG at = g_at;
		return std::string(g_registered ? "listening" : "not listening") + ", " + std::to_string(g_lines.load()) + " lines, last connect " +
			(at ? Last() + " " + std::to_string(GetTickCount64() - at) + " ms ago" : "none");
	}

	std::string LastAddress()
	{
		return g_at ? Last() : std::string();
	}

	bool LastDisconnect(std::string& reason, unsigned long long& msAgo)
	{
		ULONGLONG at = g_disconnectAt;
		if (!at)
			return false;
		{
			std::lock_guard<std::mutex> lock(g_addressMutex);
			reason = g_lastDisconnect;
		}
		msAgo = GetTickCount64() - at;
		return true;
	}

	bool HeadingToUs(unsigned withinMs)
	{
		ULONGLONG at = g_at;
		// A command-line target is noted at start-up, some seconds before the
		// join; it counts for the first join of the session.
		static bool firstJoin = true;
		bool ok = g_ours && at && (GetTickCount64() - at <= withinMs || (firstJoin && GetTickCount64() - at <= 60000));
		if (ok)
			firstJoin = false;
		return ok;
	}
}

#else

namespace connect_watch
{
	void Start() {}
	bool HeadingToUs(unsigned) { return false; }
	std::string Describe() { return "unsupported"; }
	std::string LastAddress() { return std::string(); }
	bool LastDisconnect(std::string&, unsigned long long&) { return false; }
}

#endif
