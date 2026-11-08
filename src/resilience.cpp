#include "resilience.h"

#include "crash_reports.h"
#include "file_util.h"
#include "server_gate.h"

#include <algorithm>
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

	// When this process started (unix time): a report written after it isn't
	// from a session that ended before it.
	static long long ProcessStarted()
	{
		FILETIME created, exited, kernel, user;
		if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
			return static_cast<long long>(std::time(nullptr));
		ULARGE_INTEGER t;
		t.LowPart = created.dwLowDateTime;
		t.HighPart = created.dwHighDateTime;
		return static_cast<long long>(t.QuadPart / 10000000ULL) - 11644473600LL;
	}

	std::string CrashedSession(const std::wstring& dir, std::string* name, const std::wstring* crashRoot)
	{
		const std::wstring file = dir + L"\\on_server.txt";
		const std::string text = ReadSmallFile(file, 256);
		DeleteFileW(file.c_str());
		long long since = 0;
		std::string found;
		const std::string address = ParseServerFile(text, &since, &found);
		if (address.empty())
			return std::string();
		// Only a crash: closing the game on a server leaves the file too, but no
		// report. The reports written since the session started (and before this
		// start) are that crash's.
		const std::vector<crashes::Report> reports = crashRoot ? crashes::SinceIn(*crashRoot, since - 1, 20) : crashes::Since(since - 1, 20);
		const long long started = ProcessStarted();
		std::vector<std::string> names;
		for (const crashes::Report& r : reports)
			if (r.time <= started)
				names.push_back(r.name);
		if (names.empty())
			return std::string();
		if (server_gate::IsOurAddress(address))
			NoteOurCrashes(dir, names);
		if (name)
			*name = found;
		return address;
	}

	// meowy_crashes.txt: one report file name a line, oldest first.
	static bool CrashName(const std::string& n)
	{
		return !n.empty() && n.size() <= 64 && n.find_first_of("\\/:\r\n") == std::string::npos;
	}

	static std::vector<std::string> ReadOurCrashes(const std::wstring& dir)
	{
		std::vector<std::string> names;
		const std::string text = ReadSmallFile(dir + L"\\meowy_crashes.txt", 8192);
		size_t at = 0;
		while (at < text.size())
		{
			size_t end = text.find('\n', at);
			if (end == std::string::npos)
				end = text.size();
			std::string line = text.substr(at, end - at);
			at = end + 1;
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (CrashName(line))
				names.push_back(line);
		}
		return names;
	}

	void NoteOurCrashes(const std::wstring& dir, const std::vector<std::string>& add)
	{
		std::vector<std::string> names = ReadOurCrashes(dir);
		bool changed = false;
		for (const std::string& n : add)
		{
			if (!CrashName(n) || std::find(names.begin(), names.end(), n) != names.end())
				continue;
			names.push_back(n);
			changed = true;
		}
		if (!changed)
			return;
		constexpr size_t kKeep = 50;
		std::string text;
		for (size_t i = names.size() > kKeep ? names.size() - kKeep : 0; i < names.size(); ++i)
			text += names[i] + "\r\n";
		WriteSmallFile(dir + L"\\meowy_crashes.txt", text);
	}

	std::vector<std::string> OurCrashes(const std::wstring& dir, const std::vector<std::string>& names)
	{
		const std::vector<std::string> ours = ReadOurCrashes(dir);
		std::vector<std::string> out;
		for (const std::string& n : names)
			if (std::find(ours.begin(), ours.end(), n) != ours.end())
				out.push_back(n);
		return out;
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
	std::string CrashedSession(const std::wstring&, std::string*, const std::wstring*) { return std::string(); }
	void NoteOurCrashes(const std::wstring&, const std::vector<std::string>&) {}
	std::vector<std::string> OurCrashes(const std::wstring&, const std::vector<std::string>&) { return {}; }
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
