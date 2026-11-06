#include "resilience.h"

#include "crash_reports.h"
#include "server_gate.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
	constexpr size_t kMaxLines = 24;
	constexpr size_t kMaxLineLength = 160;
	constexpr size_t kMaxSettings = 512;

	struct Line
	{
		unsigned long long at;
		std::string text;
	};

	std::mutex g_mutex;
	Line g_lines[kMaxLines];
	size_t g_next = 0;

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

	bool WriteSmallFile(const std::wstring& path, const std::string& text)
	{
		HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		DWORD n = 0;
		BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
		CloseHandle(h);
		return ok && n == text.size();
	}
#endif
}

namespace resilience
{
	void OnConsoleLine(const char* message)
	{
		if (!message)
			return;
		const unsigned long long at = NowMs();
		std::lock_guard<std::mutex> lock(g_mutex);
		// One message can hold several lines ("\nServer connection timed out.\n").
		const char* p = message;
		while (*p)
		{
			const char* end = std::strchr(p, '\n');
			size_t len = end ? static_cast<size_t>(end - p) : std::strlen(p);
			bool blank = true;
			for (size_t i = 0; i < len; ++i)
				if (p[i] != ' ' && p[i] != '\r' && p[i] != '\t')
					blank = false;
			if (!blank)
			{
				g_lines[g_next].at = at;
				g_lines[g_next].text.assign(p, len < kMaxLineLength ? len : kMaxLineLength);
				g_next = (g_next + 1) % kMaxLines;
			}
			if (!end)
				break;
			p = end + 1;
		}
	}

	std::vector<std::string> RecentLines(unsigned withinMs)
	{
		const unsigned long long now = NowMs();
		std::vector<std::string> out;
		std::lock_guard<std::mutex> lock(g_mutex);
		for (size_t i = 0; i < kMaxLines; ++i)
		{
			const Line& l = g_lines[(g_next + i) % kMaxLines];
			if (l.at && now - l.at <= withinMs)
				out.push_back(l.text);
		}
		return out;
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
