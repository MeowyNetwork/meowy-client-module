#include "resilience.h"

#include "crash_reports.h"
#include "file_util.h"
#include "server_gate.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <atomic>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
	constexpr size_t kMaxSettings = 512;

	std::atomic<unsigned long long> g_restartUntil{ 0 };   // GetTickCount64 ms; 0: none

	unsigned long long NowMs()
	{
#ifdef _WIN32
		return GetTickCount64();
#else
		return 0;
#endif
	}

#ifdef _WIN32
	std::string ReadSmallFile(const std::wstring& path, size_t max)
	{
		HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return std::string();
		std::string buf(max, '\0');
		DWORD n = 0;
		BOOL ok = ReadFile(h, &buf[0], static_cast<DWORD>(max), &n, nullptr);
		CloseHandle(h);
		buf.resize(ok ? n : 0);
		return buf;
	}

	// All at once (file_util.h): resilience.txt holds the player's answer and the
	// cl_timeout to put back, which an interrupted write would lose for good.
	bool WriteSmallFile(const std::wstring& path, const std::string& text)
	{
		return file_util::WriteAtomic(path, text);
	}
#endif
}

namespace resilience
{
	int ExpectRestart(double seconds)
	{
		int s = !(seconds >= 30) ? 30 : seconds > 600 ? 600 : static_cast<int>(seconds);
		g_restartUntil = NowMs() + static_cast<unsigned long long>(s) * 1000ULL;
		return s;
	}

	bool RestartExpected()
	{
		unsigned long long until = g_restartUntil;
		if (until == 0)
			return false;
		if (NowMs() > until)
		{
			g_restartUntil = 0;   // run out
			return false;
		}
		return true;
	}

	void ClearRestart()
	{
		g_restartUntil = 0;
	}

#ifdef _WIN32
	std::string ReadSettings(const std::wstring& dir)
	{
		return ReadSmallFile(dir + L"\\resilience.txt", kMaxSettings);
	}

	bool WriteSettings(const std::wstring& dir, const std::string& text)
	{
		if (text.size() > kMaxSettings)
			return false;
		return WriteSmallFile(dir + L"\\resilience.txt", text);
	}

	void MarkSession(const std::wstring& dir, const std::string& address)
	{
		if (!server_gate::IsOurAddress(address))
			return;
		WriteSmallFile(dir + L"\\on_server.txt", address + " " + std::to_string(static_cast<long long>(std::time(nullptr))) + "\r\n");
	}

	void ClearSession(const std::wstring& dir)
	{
		DeleteFileW((dir + L"\\on_server.txt").c_str());
	}

	std::string CrashedSession(const std::wstring& dir)
	{
		const std::wstring file = dir + L"\\on_server.txt";
		std::string text = ReadSmallFile(file, 128);
		DeleteFileW(file.c_str());
		size_t space = text.find(' ');
		if (space == std::string::npos)
			return std::string();
		std::string address = text.substr(0, space);
		long long since = std::atoll(text.c_str() + space + 1);
		if (since <= 0 || !server_gate::IsOurAddress(address))
			return std::string();
		// Only a crash: closing the game on a server leaves the file too, but no report.
		if (crashes::Since(since, 1).empty())
			return std::string();
		return address;
	}
#else
	std::string ReadSettings(const std::wstring&) { return std::string(); }
	bool WriteSettings(const std::wstring&, const std::string&) { return false; }
	void MarkSession(const std::wstring&, const std::string&) {}
	void ClearSession(const std::wstring&) {}
	std::string CrashedSession(const std::wstring&) { return std::string(); }
#endif
}
