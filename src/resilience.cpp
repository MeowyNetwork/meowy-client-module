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
		return file_util::ReadSmall(path, max);
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

	// on_server.txt and last_server.txt: "<address> <unix time>\r\n<name>\r\n".
	// The address is the engine's, checked by the caller (ours, or a public
	// a.b.c.d:port); the name is the server's own text, cleaned, and only ever
	// shown as text.
	static bool WriteServerFile(const std::wstring& path, const std::string& address, const std::string& name)
	{
		if (address.empty() || address.find_first_of(" \r\n") != std::string::npos)
			return false;
		return WriteSmallFile(path, address + " " + std::to_string(static_cast<long long>(std::time(nullptr))) +
			"\r\n" + CleanName(name) + "\r\n");
	}

	// The address ("" if the text isn't such a file), its time and the name.
	static std::string ParseServerFile(const std::string& text, long long* since, std::string* name)
	{
		const size_t space = text.find(' ');
		if (space == std::string::npos)
			return std::string();
		std::string address = text.substr(0, space);
		const long long t = std::atoll(text.c_str() + space + 1);
		if (t <= 0 || address.empty())
			return std::string();
		if (since)
			*since = t;
		if (name)
		{
			const size_t line = text.find('\n');
			const size_t end = line == std::string::npos ? std::string::npos : text.find_first_of("\r\n", line + 1);
			*name = line == std::string::npos ? std::string() : CleanName(text.substr(line + 1, end == std::string::npos ? std::string::npos : end - line - 1));
		}
		return address;
	}

	void MarkSession(const std::wstring& dir, const std::string& address, const std::string& name)
	{
		WriteServerFile(dir + L"\\on_server.txt", address, name);
	}

	void ClearSession(const std::wstring& dir)
	{
		DeleteFileW((dir + L"\\on_server.txt").c_str());
	}

	std::string CrashedSession(const std::wstring& dir, std::string* name)
	{
		const std::wstring file = dir + L"\\on_server.txt";
		const std::string text = ReadSmallFile(file, 256);
		DeleteFileW(file.c_str());
		long long since = 0;
		std::string found;
		const std::string address = ParseServerFile(text, &since, &found);
		if (address.empty())
			return std::string();
		// Only a crash: closing the game on a server leaves the file too, but no report.
		if (crashes::Since(since, 1).empty())
			return std::string();
		if (name)
			*name = found;
		return address;
	}

	void MarkLastServer(const std::wstring& dir, const std::string& address, const std::string& name)
	{
		WriteServerFile(dir + L"\\last_server.txt", address, name);
	}

	std::string LastServer(const std::wstring& dir, std::string* name, long long* since)
	{
		return ParseServerFile(ReadSmallFile(dir + L"\\last_server.txt", 256), since, name);
	}

	void ForgetLastServer(const std::wstring& dir)
	{
		DeleteFileW((dir + L"\\last_server.txt").c_str());
	}
#else
	std::string ReadSettings(const std::wstring&) { return std::string(); }
	bool WriteSettings(const std::wstring&, const std::string&) { return false; }
	void MarkSession(const std::wstring&, const std::string&, const std::string&) {}
	void ClearSession(const std::wstring&) {}
	std::string CrashedSession(const std::wstring&, std::string*) { return std::string(); }
	void MarkLastServer(const std::wstring&, const std::string&, const std::string&) {}
	std::string LastServer(const std::wstring&, std::string*, long long*) { return std::string(); }
	void ForgetLastServer(const std::wstring&) {}
#endif

	std::string CleanName(const std::string& name)
	{
		std::string out;
		for (unsigned char c : name)
		{
			if (c < 0x20 || c == 0x7F)
				continue;
			out += static_cast<char>(c);
		}
		if (out.size() > 64)
		{
			size_t cut = 64;
			while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
				--cut;   // don't split a UTF-8 character
			out.resize(cut);
		}
		return out;
	}
}
