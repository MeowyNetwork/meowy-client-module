#include "prewarm.h"

#include "file_util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#endif

namespace
{
	using prewarm::detail::Item;

	constexpr int64_t kMinSize = 1 << 20;          // files worth warming: 1 MB or more
	constexpr size_t kMaxItems = 4000;             // per server
	constexpr size_t kMaxListBytes = 1 << 20;      // a server's list file
	constexpr size_t kKeepServers = 60;            // lists kept; the oldest go first
	constexpr DWORD kChunk = 1 << 20;              // read size

	constexpr size_t kMaxWorkshop = 2000;          // Workshop items per server

	std::mutex g_mutex;
	std::vector<Item> g_join;                      // this join's files, in the order opened
	std::unordered_set<std::string> g_seen;
	std::vector<uint64_t> g_joinWorkshop;          // this join's Workshop items, in the order opened
	std::unordered_set<uint64_t> g_seenWorkshop;
	std::atomic<bool> g_recording{ false };        // written under g_mutex; read without it first

	// The warming thread: one at a time; a new Start (or Cancel) moves the
	// generation on and the old thread stops at its next read.
	std::atomic<unsigned> g_gen{ 0 };
	std::atomic<bool> g_running{ false };
	std::atomic<unsigned long long> g_doneBytes{ 0 }, g_totalBytes{ 0 };
	std::atomic<int> g_doneFiles{ 0 };
	std::string g_target;                          // under g_mutex
	std::string g_lastResult;                      // under g_mutex

#ifdef _WIN32
	std::wstring Widen(const std::string& s)
	{
		if (s.empty())
			return std::wstring();
		int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
		if (n <= 0)
			n = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
		std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
		if (n > 0 && !MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n))
			MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
		return w;
	}

	// Keeps at most kKeepServers lists, dropping the least recently written.
	void Prune(const std::wstring& joinsDir)
	{
		struct F { std::wstring path; FILETIME t; };
		std::vector<F> files;
		WIN32_FIND_DATAW fd;
		HANDLE h = FindFirstFileW((joinsDir + L"\\*.txt").c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE)
			return;
		do
		{
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				files.push_back({ joinsDir + L"\\" + fd.cFileName, fd.ftLastWriteTime });
		} while (FindNextFileW(h, &fd));
		FindClose(h);
		if (files.size() <= kKeepServers)
			return;
		std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return CompareFileTime(&a.t, &b.t) > 0; });
		for (size_t i = kKeepServers; i < files.size(); ++i)
			DeleteFileW(files[i].path.c_str());
	}

	void Warm(unsigned gen, std::vector<Item> items, unsigned long long budget, std::string address)
	{
		// Very low I/O and CPU priority: the menu, or a join that starts, comes first.
		SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
		const auto t0 = std::chrono::steady_clock::now();
		std::vector<char> buffer(kChunk);
		unsigned long long done = 0;
		int files = 0;
		bool stopped = false;
		for (const Item& it : items)
		{
			if (g_gen.load() != gen || done >= budget)
			{
				stopped = g_gen.load() != gen;
				break;
			}
			HANDLE f = CreateFileW(Widen(it.path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
			if (f == INVALID_HANDLE_VALUE)
				continue;
			LARGE_INTEGER size;
			if (!GetFileSizeEx(f, &size) || size.QuadPart != it.size)
			{
				CloseHandle(f);   // changed since it was recorded: not this server's file any more
				continue;
			}
			DWORD got = 0;
			while (g_gen.load() == gen && done < budget && ReadFile(f, buffer.data(), kChunk, &got, nullptr) && got > 0)
			{
				done += got;
				g_doneBytes = done;
			}
			CloseHandle(f);
			++files;
			g_doneFiles = files;
		}
		if (g_gen.load() != gen)
			stopped = true;
		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			char line[256];
			std::snprintf(line, sizeof(line), "%s %s: %.0f MB in %d files, %.1f s", stopped ? "stopped warming" : "warmed",
				address.c_str(), done / 1048576.0, files, secs);
			g_lastResult = line;
		}
		SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
		if (g_gen.load() == gen)
			g_running = false;
	}
#endif
}

namespace prewarm
{
	namespace detail
	{
		std::wstring FileFor(const std::wstring& dir, const std::string& address)
		{
			std::wstring name;
			for (char c : address)
			{
				if ((c >= '0' && c <= '9') || c == '.')
					name.push_back(static_cast<wchar_t>(c));
				else if (c == ':')
					name.push_back(L'_');
				else
					return std::wstring();   // only a.b.c.d:port
			}
			if (name.empty())
				return std::wstring();
			return dir + L"\\joins\\" + name + L".txt";
		}

		bool Parse(const std::string& text, std::vector<Item>& out, std::vector<uint64_t>* workshop)
		{
			size_t pos = 0;
			while (pos < text.size())
			{
				size_t end = text.find('\n', pos);
				if (end == std::string::npos)
					end = text.size();
				std::string line = text.substr(pos, end - pos);
				pos = end + 1;
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				const size_t tab = line.rfind('\t');
				if (tab == std::string::npos || tab == 0)
					continue;
				const char* value = line.c_str() + tab + 1;
				if (line.compare(0, tab, "workshop") == 0)
				{
					char* stop = nullptr;
					const unsigned long long id = std::strtoull(value, &stop, 10);
					if (workshop && *value >= '0' && *value <= '9' && stop && !*stop && id && workshop->size() < kMaxWorkshop)
						workshop->push_back(id);
					continue;
				}
				char* stop = nullptr;
				const long long size = std::strtoll(value, &stop, 10);
				if (!stop || *stop || size < kMinSize || out.size() >= kMaxItems)
					continue;
				out.push_back({ line.substr(0, tab), static_cast<int64_t>(size) });
			}
			return !out.empty();
		}

		std::string Format(const std::vector<Item>& items, const std::vector<uint64_t>& workshop)
		{
			// Workshop items first: they're few and short, so the byte cap never drops them.
			std::string out;
			for (uint64_t id : workshop)
				out += "workshop\t" + std::to_string(static_cast<unsigned long long>(id)) + "\r\n";
			for (const Item& it : items)
			{
				if (it.path.find_first_of("\t\r\n") != std::string::npos)
					continue;
				std::string line = it.path + "\t" + std::to_string(static_cast<long long>(it.size)) + "\r\n";
				if (out.size() + line.size() > kMaxListBytes)
					break;
				out += line;
			}
			return out;
		}

		uint64_t WorkshopId(const std::string& path)
		{
			std::string p = path;
			for (char& c : p)
			{
				if (c == '/')
					c = '\\';
				else if (c >= 'A' && c <= 'Z')
					c = static_cast<char>(c - 'A' + 'a');
			}
			const auto digitsAt = [&](size_t at, size_t& end) -> uint64_t {
				end = at;
				uint64_t id = 0;
				while (end < p.size() && p[end] >= '0' && p[end] <= '9' && end - at < 19)
					id = id * 10 + static_cast<uint64_t>(p[end++] - '0');
				return end > at ? id : 0;
			};
			size_t end = 0;
			static const std::string kContent = "\\workshop\\content\\4000\\";
			size_t at = p.find(kContent);
			if (at != std::string::npos)
			{
				const uint64_t id = digitsAt(at + kContent.size(), end);
				return (id && end < p.size() && p[end] == '\\') ? id : 0;
			}
			static const std::string kCache = "\\cache\\workshop\\";
			at = p.find(kCache);
			if (at != std::string::npos)
			{
				const uint64_t id = digitsAt(at + kCache.size(), end);
				return (id && p.compare(end, std::string::npos, ".gma") == 0) ? id : 0;
			}
			return 0;
		}

		int NoSeekPenalty(const std::wstring& path)
		{
#ifdef _WIN32
			wchar_t volume[MAX_PATH];
			if (!GetVolumePathNameW(path.c_str(), volume, MAX_PATH))
				return -1;
			std::wstring v = volume;
			if (v.size() < 2 || v[1] != L':')
				return -1;
			const std::wstring device = L"\\\\.\\" + v.substr(0, 2);
			HANDLE h = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
			if (h == INVALID_HANDLE_VALUE)
				return -1;
			STORAGE_PROPERTY_QUERY q = {};
			q.PropertyId = StorageDeviceSeekPenaltyProperty;
			q.QueryType = PropertyStandardQuery;
			DEVICE_SEEK_PENALTY_DESCRIPTOR d = {};
			DWORD got = 0;
			const BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &d, sizeof(d), &got, nullptr);
			CloseHandle(h);
			if (!ok || got < sizeof(d))
				return -1;
			return d.IncursSeekPenalty ? 0 : 1;
#else
			(void)path;
			return -1;
#endif
		}
	}

	void Opened(const char* path, int64_t size)
	{
		if (!path || !g_recording.load(std::memory_order_relaxed))
			return;
		const uint64_t workshop = detail::WorkshopId(path);
		if (size < kMinSize && !workshop)
			return;
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_recording)
			return;
		if (workshop && g_joinWorkshop.size() < kMaxWorkshop && g_seenWorkshop.insert(workshop).second)
			g_joinWorkshop.push_back(workshop);
		if (size >= kMinSize && g_join.size() < kMaxItems && g_seen.insert(path).second)
			g_join.push_back({ path, size });
	}

	void BeginJoin()
	{
		Cancel();
		std::lock_guard<std::mutex> lock(g_mutex);
		g_join.clear();
		g_seen.clear();
		g_joinWorkshop.clear();
		g_seenWorkshop.clear();
		g_recording = true;
	}

	int EndJoin(const std::wstring& dir, const std::string& address)
	{
#ifdef _WIN32
		std::vector<Item> items;
		std::vector<uint64_t> workshop;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (!g_recording)
				return 0;
			g_recording = false;
			items.swap(g_join);
			workshop.swap(g_joinWorkshop);
			g_seen.clear();
			g_seenWorkshop.clear();
		}
		const std::wstring file = detail::FileFor(dir, address);
		if (items.empty() || file.empty())
			return 0;
		const std::wstring joins = dir + L"\\joins";
		CreateDirectoryW(joins.c_str(), nullptr);
		if (!file_util::WriteAtomic(file, detail::Format(items, workshop)))
			return 0;
		Prune(joins);
		return static_cast<int>(items.size());
#else
		(void)dir; (void)address;
		return 0;
#endif
	}

	bool Recorded(const std::wstring& dir, const std::string& address, uint64_t* bytes)
	{
		const std::wstring file = detail::FileFor(dir, address);
		if (file.empty())
			return false;
		std::vector<Item> items;
		if (!detail::Parse(file_util::ReadSmall(file, kMaxListBytes), items))
			return false;
		if (bytes)
		{
			uint64_t total = 0;
			for (const Item& it : items)
				total += static_cast<uint64_t>(it.size);
			*bytes = total;
		}
		return true;
	}

	std::vector<uint64_t> WorkshopItems(const std::wstring& dir, const std::string& address)
	{
		std::vector<Item> items;
		std::vector<uint64_t> workshop;
		const std::wstring file = detail::FileFor(dir, address);
		if (!file.empty())
			detail::Parse(file_util::ReadSmall(file, kMaxListBytes), items, &workshop);
		return workshop;
	}

	bool Start(const std::wstring& dir, const std::string& address, uint64_t budget, std::string& why)
	{
#ifdef _WIN32
		const std::wstring file = detail::FileFor(dir, address);
		std::vector<Item> items;
		if (file.empty() || !detail::Parse(file_util::ReadSmall(file, kMaxListBytes), items))
		{
			why = "nothing recorded for that server";
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (g_running && g_target == address)
				return true;   // already warming it
		}
		if (detail::NoSeekPenalty(Widen(items.front().path)) != 1)
		{
			why = "its files aren't on an SSD";
			return false;
		}
		uint64_t total = 0;
		for (const Item& it : items)
			total += static_cast<uint64_t>(it.size);
		const unsigned gen = ++g_gen;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_target = address;
		}
		g_doneBytes = 0;
		g_doneFiles = 0;
		g_totalBytes = std::min<uint64_t>(total, budget);
		g_running = true;
		try
		{
			std::thread(Warm, gen, std::move(items), static_cast<unsigned long long>(budget), address).detach();
		}
		catch (...)
		{
			g_running = false;
			why = "couldn't start the reader";
			return false;
		}
		return true;
#else
		(void)dir; (void)address; (void)budget;
		why = "Windows only";
		return false;
#endif
	}

	void Cancel()
	{
		++g_gen;
		g_running = false;
	}

	std::string Describe()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		std::string out;
		if (g_running)
		{
			char line[256];
			std::snprintf(line, sizeof(line), "warming %s: %.0f of %.0f MB, %d files", g_target.c_str(),
				g_doneBytes.load() / 1048576.0, g_totalBytes.load() / 1048576.0, g_doneFiles.load());
			out = line;
		}
		else
		{
			out = g_lastResult.empty() ? "idle" : g_lastResult;
		}
		if (g_recording)
			out += "; recording this join: " + std::to_string(g_join.size()) + " files, " +
				std::to_string(g_joinWorkshop.size()) + " Workshop items";
		return out;
	}
}
