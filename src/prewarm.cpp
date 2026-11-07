#include "prewarm.h"

#include "file_util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include "pe_image.h"
#endif

namespace
{
	using prewarm::detail::Item;
	using prewarm::detail::Range;
	using prewarm::detail::kBlock;

	constexpr int64_t kMinSize = 1 << 20;          // files worth warming whole: 1 MB or more
	constexpr size_t kMaxItems = 4000;             // per server
	constexpr size_t kMaxRanges = 8000;            // files with ranges, per server
	constexpr size_t kMaxListBytes = 4 << 20;      // a server's list file
	constexpr size_t kKeepServers = 60;            // lists kept; the oldest go first
	constexpr unsigned long kChunk = 1 << 20;      // read size
	constexpr int64_t kMaxBlocks = 1 << 16;        // blocks noted per file (64 GB)

	constexpr size_t kMaxWorkshop = 2000;          // Workshop items per server

	std::mutex g_mutex;
	std::vector<Item> g_join;                      // this join's files, in the order opened
	std::unordered_set<std::string> g_seen;
	std::vector<uint64_t> g_joinWorkshop;          // this join's Workshop items, in the order opened
	std::unordered_set<uint64_t> g_seenWorkshop;
	std::atomic<bool> g_recording{ false };        // written under g_mutex; read without it first

	// This join's reads: per file (lowercase path), which blocks. Under g_readMutex.
	struct Reads { std::string path; int64_t size; std::vector<uint8_t> blocks; size_t marked = 0; };
	std::mutex g_readMutex;
	std::unordered_map<std::string, Reads> g_reads;

	// The warming thread: one at a time; a new Start (or Cancel) moves the
	// generation on and the old thread stops at its next read.
	std::atomic<unsigned> g_gen{ 0 };
	std::atomic<bool> g_running{ false };
	std::atomic<unsigned long long> g_doneBytes{ 0 }, g_totalBytes{ 0 };
	std::atomic<int> g_doneFiles{ 0 };
	std::string g_target;                          // under g_mutex
	std::string g_lastResult;                      // under g_mutex

	std::string Lower(std::string s)
	{
		for (char& c : s)
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
		return s;
	}

	// Blocks set in a bitmap -> runs [start, end).
	std::vector<std::pair<uint32_t, uint32_t>> Runs(const std::vector<uint8_t>& blocks)
	{
		std::vector<std::pair<uint32_t, uint32_t>> runs;
		for (uint32_t i = 0; i < blocks.size(); ++i)
		{
			if (!blocks[i])
				continue;
			if (!runs.empty() && runs.back().second == i)
				runs.back().second = i + 1;
			else
				runs.push_back({ i, i + 1 });
		}
		return runs;
	}

	uint64_t RangeBytes(const Range& r)
	{
		uint64_t total = 0;
		for (const auto& run : r.runs)
		{
			const int64_t start = static_cast<int64_t>(run.first) * kBlock;
			const int64_t end = std::min<int64_t>(static_cast<int64_t>(run.second) * kBlock, r.size);
			if (end > start)
				total += static_cast<uint64_t>(end - start);
		}
		return total;
	}

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

	std::string Narrow(const std::wstring& w)
	{
		if (w.empty())
			return std::string();
		const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
		std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
		if (n > 0)
			WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
		return s;
	}

	bool StillThere(const Item& it)
	{
		WIN32_FILE_ATTRIBUTE_DATA a;
		if (!GetFileAttributesExW(Widen(it.path).c_str(), GetFileExInfoStandard, &a))
			return false;
		return ((static_cast<int64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow) == it.size;
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

	// --- The read hook ---------------------------------------------------------------
	using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
	using CloseHandleFn = BOOL(WINAPI*)(HANDLE);
	std::mutex g_hookMutex;                        // install state
	HMODULE g_hooked = nullptr;
	void** g_readSlot = nullptr;
	void** g_closeSlot = nullptr;
	ReadFileFn g_readFile = nullptr;
	CloseHandleFn g_closeHandle = nullptr;
	// A handle's file while a join loads (nullptr: not one to note). Under g_readMutex.
	struct Known { std::string path; int64_t size; bool note; };
	std::unordered_map<HANDLE, Known> g_handles;

	// The file behind a handle: a local disk file, its path and size.
	bool FileOf(HANDLE h, std::string& path, int64_t& size)
	{
		if (GetFileType(h) != FILE_TYPE_DISK)
			return false;
		wchar_t buf[1024];
		const DWORD n = GetFinalPathNameByHandleW(h, buf, 1024, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		if (n == 0 || n >= 1024)
			return false;
		std::wstring w(buf, n);
		if (w.compare(0, 4, L"\\\\?\\") == 0)
			w = w.substr(4);
		if (w.size() < 3 || w[1] != L':' || w[2] != L'\\')
			return false;   // a local "X:\..." path only
		LARGE_INTEGER s;
		if (!GetFileSizeEx(h, &s) || s.QuadPart <= 0)
			return false;
		path = Narrow(w);
		size = s.QuadPart;
		// The game's data folder holds what servers write: not content to warm.
		const std::string l = Lower(path);
		return l.find("\\garrysmod\\data\\") == std::string::npos;
	}

	void NoteHandleRead(HANDLE h, DWORD length, LPOVERLAPPED ov)
	{
		uint64_t offset = 0;
		if (ov)
		{
			offset = (static_cast<uint64_t>(ov->OffsetHigh) << 32) | ov->Offset;
		}
		else
		{
			LARGE_INTEGER zero = {}, pos = {};
			if (!SetFilePointerEx(h, zero, &pos, FILE_CURRENT))
				return;
			offset = static_cast<uint64_t>(pos.QuadPart);
		}
		std::string path;
		int64_t size = 0;
		{
			std::lock_guard<std::mutex> lock(g_readMutex);
			auto it = g_handles.find(h);
			if (it != g_handles.end())
			{
				if (!it->second.note)
					return;
				path = it->second.path;
				size = it->second.size;
			}
		}
		if (path.empty())
		{
			Known k;
			k.note = FileOf(h, k.path, k.size);
			{
				std::lock_guard<std::mutex> lock(g_readMutex);
				g_handles[h] = k;
			}
			if (!k.note)
				return;
			path = k.path;
			size = k.size;
		}
		prewarm::detail::NoteRead(path, size, offset, length);
	}

	BOOL WINAPI HookReadFile(HANDLE h, LPVOID buffer, DWORD length, LPDWORD read, LPOVERLAPPED ov)
	{
		if (g_recording.load(std::memory_order_relaxed) && length > 0)
		{
			const DWORD error = GetLastError();
			NoteHandleRead(h, length, ov);
			SetLastError(error);
		}
		return g_readFile(h, buffer, length, read, ov);
	}

	BOOL WINAPI HookCloseHandle(HANDLE h)
	{
		if (g_recording.load(std::memory_order_relaxed))
		{
			std::lock_guard<std::mutex> lock(g_readMutex);
			g_handles.erase(h);
		}
		return g_closeHandle(h);
	}

	bool WriteSlot(void** at, void* value)
	{
		DWORD old;
		if (!VirtualProtect(at, sizeof(void*), PAGE_READWRITE, &old))
			return false;
		*at = value;
		DWORD unused;
		VirtualProtect(at, sizeof(void*), old, &unused);
		return true;
	}

	// A module's import table entry for `name`, imported by name from kernel32
	// (or a core API set), or nullptr.
	void** ImportSlot(HMODULE module, const char* name)
	{
		const IMAGE_NT_HEADERS* nt = pe_image::Headers(module);
		if (!nt)
			return nullptr;
		auto* base = reinterpret_cast<uint8_t*>(module);
		const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!dir.VirtualAddress)
			return nullptr;
		for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d)
		{
			const char* dll = reinterpret_cast<const char*>(base + d->Name);
			if (_stricmp(dll, "kernel32.dll") != 0 && _strnicmp(dll, "api-ms-win-core-", 16) != 0)
				continue;
			if (!d->OriginalFirstThunk)
				continue;   // bound without names: can't tell the entries apart
			auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
			auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
			for (; names->u1.AddressOfData; ++names, ++iat)
			{
				if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
					continue;
				auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
				if (std::strcmp(reinterpret_cast<const char*>(byName->Name), name) == 0)
					return reinterpret_cast<void**>(&iat->u1.Function);
			}
		}
		return nullptr;
	}

	// --- Warming ---------------------------------------------------------------------
	void Warm(unsigned gen, std::vector<Item> items, std::vector<Range> ranges, unsigned long long budget, std::string address)
	{
		// Very low I/O and CPU priority: the menu, or a join that starts, comes first.
		SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
		const auto t0 = std::chrono::steady_clock::now();
		std::vector<char> buffer(kChunk);
		unsigned long long done = 0;
		int files = 0;
		const auto keepGoing = [&]() { return g_gen.load() == gen && done < budget; };
		const auto readAt = [&](HANDLE f, int64_t start, int64_t end) {
			LARGE_INTEGER pos;
			pos.QuadPart = start;
			if (!SetFilePointerEx(f, pos, nullptr, FILE_BEGIN))
				return;
			int64_t left = end - start;
			DWORD got = 0;
			while (left > 0 && keepGoing() &&
				ReadFile(f, buffer.data(), static_cast<DWORD>(std::min<int64_t>(left, kChunk)), &got, nullptr) && got > 0)
			{
				done += got;
				left -= got;
				g_doneBytes = done;
			}
		};
		// The ranges first: they're what the map and cache steps wait on, a few
		// hundred MB for many seconds.
		for (const Range& r : ranges)
		{
			if (!keepGoing())
				break;
			HANDLE f = CreateFileW(Widen(r.path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
			if (f == INVALID_HANDLE_VALUE)
				continue;
			LARGE_INTEGER size;
			if (GetFileSizeEx(f, &size) && size.QuadPart == r.size)
			{
				for (const auto& run : r.runs)
				{
					if (!keepGoing())
						break;
					readAt(f, static_cast<int64_t>(run.first) * kBlock, std::min<int64_t>(static_cast<int64_t>(run.second) * kBlock, r.size));
				}
				++files;
				g_doneFiles = files;
			}
			CloseHandle(f);
		}
		for (const Item& it : items)
		{
			if (!keepGoing())
				break;
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
			readAt(f, 0, it.size);
			CloseHandle(f);
			++files;
			g_doneFiles = files;
		}
		const bool stopped = g_gen.load() != gen;
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

		// "0-3,7-9" -> runs; false if anything's off.
		static bool ParseRuns(const char* s, int64_t size, std::vector<std::pair<uint32_t, uint32_t>>& runs)
		{
			const int64_t blocks = (size + kBlock - 1) / kBlock;
			uint32_t last = 0;
			while (*s)
			{
				char* stop = nullptr;
				const unsigned long a = std::strtoul(s, &stop, 10);
				if (stop == s || *stop != '-')
					return false;
				s = stop + 1;
				const unsigned long b = std::strtoul(s, &stop, 10);
				if (stop == s || (*stop && *stop != ','))
					return false;
				if (b <= a || static_cast<int64_t>(b) > blocks || a < last)
					return false;
				runs.push_back({ static_cast<uint32_t>(a), static_cast<uint32_t>(b) });
				last = static_cast<uint32_t>(b);
				s = *stop ? stop + 1 : stop;
			}
			return !runs.empty();
		}

		bool Parse(const std::string& text, std::vector<Item>& out, std::vector<uint64_t>* workshop, std::vector<Range>* ranges)
		{
			size_t pos = 0;
			bool any = false;
			while (pos < text.size())
			{
				size_t end = text.find('\n', pos);
				if (end == std::string::npos)
					end = text.size();
				std::string line = text.substr(pos, end - pos);
				pos = end + 1;
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (line.compare(0, 6, "range\t") == 0)
				{
					// range \t path \t size \t runs
					const size_t a = 6, b = line.find('\t', a), c = b == std::string::npos ? b : line.find('\t', b + 1);
					if (!ranges || c == std::string::npos || b == a || ranges->size() >= kMaxRanges)
						continue;
					char* stop = nullptr;
					const long long size = std::strtoll(line.c_str() + b + 1, &stop, 10);
					if (stop != line.c_str() + c || size <= 0)
						continue;
					Range r{ line.substr(a, b - a), static_cast<int64_t>(size), {} };
					if (ParseRuns(line.c_str() + c + 1, r.size, r.runs))
					{
						ranges->push_back(std::move(r));
						any = true;
					}
					continue;
				}
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
				any = true;
			}
			return any;
		}

		void Merge(std::vector<Item>& items, std::vector<uint64_t>& workshop, const std::vector<Item>& before,
			const std::vector<uint64_t>& workshopBefore, bool (*keep)(const Item&))
		{
			std::unordered_set<std::string> seen;
			for (const Item& it : items)
				seen.insert(Lower(it.path));
			for (const Item& it : before)
			{
				if (items.size() >= kMaxItems)
					break;
				if (seen.insert(Lower(it.path)).second && (!keep || keep(it)))
					items.push_back(it);
			}
			std::unordered_set<uint64_t> ids(workshop.begin(), workshop.end());
			for (uint64_t id : workshopBefore)
			{
				if (workshop.size() >= kMaxWorkshop)
					break;
				if (ids.insert(id).second)
					workshop.push_back(id);
			}
		}

		void MergeRanges(std::vector<Range>& ranges, const std::vector<Range>& before, const std::vector<Item>& whole,
			bool (*keep)(const Item&))
		{
			std::unordered_set<std::string> wholeSet;
			for (const Item& it : whole)
				wholeSet.insert(Lower(it.path));
			std::vector<Range> out;
			std::unordered_map<std::string, size_t> at;
			for (Range& r : ranges)
			{
				const std::string key = Lower(r.path);
				if (wholeSet.count(key) || at.count(key) || out.size() >= kMaxRanges)
					continue;
				at[key] = out.size();
				out.push_back(std::move(r));
			}
			for (const Range& b : before)
			{
				const std::string key = Lower(b.path);
				if (wholeSet.count(key))
					continue;
				auto it = at.find(key);
				if (it != at.end())
				{
					Range& r = out[it->second];
					if (r.size != b.size)
						continue;   // changed since: this join's blocks are the ones that count
					const int64_t blocks = (r.size + kBlock - 1) / kBlock;
					std::vector<uint8_t> map(static_cast<size_t>(std::min<int64_t>(blocks, kMaxBlocks)), 0);
					const std::vector<std::pair<uint32_t, uint32_t>>* both[] = { &r.runs, &b.runs };
					for (const auto* runs : both)
						for (const auto& run : *runs)
							for (uint32_t i = run.first; i < run.second && i < map.size(); ++i)
								map[i] = 1;
					r.runs = Runs(map);
					continue;
				}
				if (out.size() >= kMaxRanges || (keep && !keep({ b.path, b.size })))
					continue;
				at[key] = out.size();
				out.push_back(b);
			}
			ranges.swap(out);
		}

		std::string Format(const std::vector<Item>& items, const std::vector<uint64_t>& workshop, const std::vector<Range>& ranges)
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
					return out;
				out += line;
			}
			for (const Range& r : ranges)
			{
				if (r.path.find_first_of("\t\r\n") != std::string::npos || r.runs.empty())
					continue;
				std::string line = "range\t" + r.path + "\t" + std::to_string(static_cast<long long>(r.size)) + "\t";
				for (size_t i = 0; i < r.runs.size(); ++i)
					line += (i ? "," : "") + std::to_string(r.runs[i].first) + "-" + std::to_string(r.runs[i].second);
				line += "\r\n";
				if (out.size() + line.size() > kMaxListBytes)
					break;
				out += line;
			}
			return out;
		}

		void NoteRead(const std::string& path, int64_t size, uint64_t offset, uint64_t length)
		{
			if (size <= 0 || length == 0 || offset >= static_cast<uint64_t>(size) || !g_recording.load(std::memory_order_relaxed))
				return;
			const int64_t blocks = std::min<int64_t>((size + kBlock - 1) / kBlock, kMaxBlocks);
			const uint64_t first = offset / kBlock;
			const uint64_t last = std::min<uint64_t>((offset + length - 1) / kBlock, static_cast<uint64_t>(blocks - 1));
			std::lock_guard<std::mutex> lock(g_readMutex);
			const std::string key = Lower(path);
			auto it = g_reads.find(key);
			if (it == g_reads.end())
			{
				if (g_reads.size() >= kMaxRanges)
					return;
				it = g_reads.emplace(key, Reads{ path, size, std::vector<uint8_t>(static_cast<size_t>(blocks), 0) }).first;
			}
			Reads& r = it->second;
			if (r.size != size)
				return;
			for (uint64_t i = first; i <= last && i < r.blocks.size(); ++i)
			{
				if (!r.blocks[static_cast<size_t>(i)])
				{
					r.blocks[static_cast<size_t>(i)] = 1;
					++r.marked;
				}
			}
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

	bool InstallReadHook(std::string& why)
	{
#ifdef _WIN32
		std::lock_guard<std::mutex> lock(g_hookMutex);
		if (g_hooked)
			return true;
		HMODULE fs = GetModuleHandleW(L"filesystem_stdio.dll");
		if (!fs)
		{
			why = "filesystem_stdio.dll not loaded";
			return false;
		}
		void** read = ImportSlot(fs, "ReadFile");
		void** close = ImportSlot(fs, "CloseHandle");
		if (!read || !close)
		{
			why = std::string("filesystem_stdio doesn't import ") + (!read ? "ReadFile" : "CloseHandle") + " from kernel32 by name";
			return false;
		}
		g_readFile = reinterpret_cast<ReadFileFn>(*read);
		g_closeHandle = reinterpret_cast<CloseHandleFn>(*close);
		// CloseHandle first: it only drops handles ReadFile noted.
		if (!WriteSlot(close, reinterpret_cast<void*>(&HookCloseHandle)) || !WriteSlot(read, reinterpret_cast<void*>(&HookReadFile)))
		{
			WriteSlot(read, reinterpret_cast<void*>(g_readFile));
			WriteSlot(close, reinterpret_cast<void*>(g_closeHandle));
			why = "couldn't write the import table";
			return false;
		}
		g_readSlot = read;
		g_closeSlot = close;
		g_hooked = fs;
		return true;
#else
		why = "Windows only";
		return false;
#endif
	}

	void BeginJoin()
	{
		Cancel();
		{
			std::lock_guard<std::mutex> lock(g_readMutex);
			g_reads.clear();
#ifdef _WIN32
			g_handles.clear();
#endif
		}
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
		std::unordered_map<std::string, Reads> reads;
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
		{
			std::lock_guard<std::mutex> lock(g_readMutex);
			reads.swap(g_reads);
			g_handles.clear();
		}
		std::vector<Range> ranges;
		for (auto& kv : reads)
		{
			if (kv.second.marked)
				ranges.push_back({ kv.second.path, kv.second.size, Runs(kv.second.blocks) });
		}
		// The most blocks first, so a cut list keeps what matters most.
		std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return RangeBytes(a) > RangeBytes(b); });
		const std::wstring file = detail::FileFor(dir, address);
		if ((items.empty() && ranges.empty()) || file.empty())
			return 0;
		// Earlier joins' files and ranges stay while they're on disk at their recorded size.
		std::vector<Item> before;
		std::vector<uint64_t> workshopBefore;
		std::vector<Range> rangesBefore;
		detail::Parse(file_util::ReadSmall(file, kMaxListBytes), before, &workshopBefore, &rangesBefore);
		detail::Merge(items, workshop, before, workshopBefore, StillThere);
		detail::MergeRanges(ranges, rangesBefore, items, StillThere);
		const std::wstring joins = dir + L"\\joins";
		CreateDirectoryW(joins.c_str(), nullptr);
		if (!file_util::WriteAtomic(file, detail::Format(items, workshop, ranges)))
			return 0;
		Prune(joins);
		return static_cast<int>(items.size() + ranges.size());
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
		std::vector<Range> ranges;
		if (!detail::Parse(file_util::ReadSmall(file, kMaxListBytes), items, nullptr, &ranges))
			return false;
		if (bytes)
		{
			uint64_t total = 0;
			for (const Item& it : items)
				total += static_cast<uint64_t>(it.size);
			for (const Range& r : ranges)
				total += RangeBytes(r);
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
		std::vector<Range> ranges;
		if (file.empty() || !detail::Parse(file_util::ReadSmall(file, kMaxListBytes), items, nullptr, &ranges))
		{
			why = "nothing recorded for that server";
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (g_running && g_target == address)
				return true;   // already warming it
		}
		if (detail::NoSeekPenalty(Widen(!ranges.empty() ? ranges.front().path : items.front().path)) != 1)
		{
			why = "its files aren't on an SSD";
			return false;
		}
		uint64_t total = 0;
		for (const Item& it : items)
			total += static_cast<uint64_t>(it.size);
		for (const Range& r : ranges)
			total += RangeBytes(r);
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
			std::thread(Warm, gen, std::move(items), std::move(ranges), static_cast<unsigned long long>(budget), address).detach();
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
		std::string out;
		size_t readFiles = 0;
		{
			std::lock_guard<std::mutex> lock(g_readMutex);
			readFiles = g_reads.size();
		}
		std::lock_guard<std::mutex> lock(g_mutex);
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
				std::to_string(g_joinWorkshop.size()) + " Workshop items, reads from " + std::to_string(readFiles) + " files";
#ifdef _WIN32
		{
			std::lock_guard<std::mutex> hookLock(g_hookMutex);
			out += g_hooked ? "; read hook on" : "; read hook off";
		}
#endif
		return out;
	}
}
