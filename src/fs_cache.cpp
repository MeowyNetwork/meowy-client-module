#include "fs_cache.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace
{
	// ASCII lowercase in place; false if there's anything but ASCII.
	bool LowerAscii(std::string& s)
	{
		for (char& c : s)
		{
			const unsigned char u = static_cast<unsigned char>(c);
			if (u >= 0x80)
				return false;
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
		}
		return true;
	}

}

#ifdef _WIN32
#include "pe_image.h"

namespace
{
	using fs_cache::detail::Answer;

	// --- The folder listings -----------------------------------------------------
	struct Listing
	{
		bool exists = false;
		bool reusable = false;                   // a plain folder: its write time tells when names change
		FILETIME written = {};                   // the folder's last-write time when it was listed
		std::unordered_set<std::string> names;   // lowercase
		std::atomic<unsigned> join{ 0 };         // the join it was last listed or checked in
	};

	SRWLOCK g_lock = SRWLOCK_INIT;
	std::unordered_map<std::string, std::shared_ptr<Listing>> g_folders;

	std::atomic<bool> g_active{ false };
	std::atomic<unsigned> g_join{ 1 };
	std::atomic<long long> g_opens{ 0 }, g_absent{ 0 }, g_listed{ 0 }, g_reused{ 0 }, g_passed{ 0 };

	// The folder as it is now: absent, or present with its write time and
	// whether it's a link (a link's own time doesn't follow its target's names).
	bool Probe(const std::string& dir, bool& exists, FILETIME& written, bool& link)
	{
		WIN32_FILE_ATTRIBUTE_DATA a;
		if (!GetFileAttributesExA(dir.c_str(), GetFileExInfoStandard, &a))
		{
			const DWORD e = GetLastError();
			exists = false;
			return e == ERROR_PATH_NOT_FOUND || e == ERROR_FILE_NOT_FOUND || e == ERROR_INVALID_NAME;
		}
		exists = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		written = a.ftLastWriteTime;
		link = (a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
		return true;
	}

	// A listing from an earlier join still holds if the folder is as it was:
	// still absent, or a plain folder whose write time (it changes whenever a
	// name is added, removed or renamed) hasn't moved.
	bool StillHolds(const std::string& dir, const Listing& l)
	{
		bool exists = false, link = false;
		FILETIME written = {};
		if (!Probe(dir, exists, written, link))
			return false;
		if (!l.exists)
			return !exists;
		return exists && l.reusable && !link && CompareFileTime(&written, &l.written) == 0;
	}

	// Reads one folder. Null: couldn't tell (access denied, say), don't cache.
	std::shared_ptr<Listing> List(const std::string& dir)
	{
		auto listing = std::make_shared<Listing>();
		listing->join = g_join.load();
		bool exists = false, link = false;
		// Before the listing: a name added in between moves the time, so the
		// next join lists again rather than trusting a listing that missed it.
		// Unless the folder changed within the last couple of seconds: the
		// clock can be 15 ms coarse, so a second change in the same tick
		// wouldn't move the time. Such a listing is only for this join.
		if (Probe(dir, exists, listing->written, link) && exists && !link)
		{
			FILETIME now;
			GetSystemTimeAsFileTime(&now);
			const auto ticks = [](const FILETIME& f) {
				return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
			};
			const unsigned long long kSettled = 2ULL * 10000000ULL;   // 2 s in 100 ns units
			listing->reusable = ticks(now) >= ticks(listing->written) + kSettled;
		}
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileExA((dir + "\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
			FIND_FIRST_EX_LARGE_FETCH);
		if (h == INVALID_HANDLE_VALUE)
		{
			const DWORD e = GetLastError();
			if (e == ERROR_PATH_NOT_FOUND || e == ERROR_FILE_NOT_FOUND || e == ERROR_DIRECTORY || e == ERROR_INVALID_NAME)
				return listing;   // no such folder: nothing in it can be opened
			return nullptr;
		}
		listing->exists = true;
		do
		{
			std::string name = fd.cFileName;
			if (LowerAscii(name))
				listing->names.insert(std::move(name));
			// A name with other characters can't match a query (queries are
			// ASCII), so leaving it out never answers "absent" for a real file.
		} while (FindNextFileA(h, &fd));
		FindClose(h);
		return listing;
	}

	// --- The hook ------------------------------------------------------------------
#ifdef _WIN64
	using FopenFn = void* (*)(void*, const char*, const char*, uintptr_t, int64_t*);
	const uint8_t kRbCheck[] = { 0x80, 0x3B, 0x72, 0x75 };   // cmp byte [rbx],'r'; jne
	const uint8_t kRbCheck2[] = { 0x80, 0x7B, 0x01, 0x62 };  // cmp byte [rbx+1],'b'
	constexpr size_t kRbAt = 0x9F;
#else
	using FopenFn = void*(__thiscall*)(void*, const char*, const char*, uintptr_t, int64_t*);
	const uint8_t kRbCheck[] = { 0x80, 0x3E, 0x72, 0x75 };   // cmp byte [esi],'r'; jne
	const uint8_t kRbCheck2[] = { 0x80, 0x7E, 0x01, 0x62 };  // cmp byte [esi+1],'b'
	constexpr size_t kRbAt = 0x7C;
#endif
	constexpr int kFopenSlot = 129;

	FopenFn g_fopen = nullptr;
	void* g_object = nullptr;
	void** g_originalVtable = nullptr;
	void** g_copy = nullptr;            // [0] = the complete-object locator, then the slots

	bool ReadsOnly(const char* options)
	{
		if (!options || !*options)
			return false;
		for (const char* p = options; *p; ++p)
			if (*p == 'w' || *p == 'a' || *p == '+')
				return false;
		return options[0] == 'r';
	}

	void* Fopen(void* self, const char* name, const char* options, uintptr_t flags, int64_t* size)
	{
		if (g_active.load(std::memory_order_relaxed))
		{
			++g_opens;
			std::string dir, file;
			if (fs_cache::detail::Split(name, dir, file) && !fs_cache::detail::Excluded(dir))
			{
				if (ReadsOnly(options))
				{
					if (fs_cache::detail::Lookup(dir, file) == Answer::Absent)
					{
						++g_absent;
						SetLastError(ERROR_FILE_NOT_FOUND);
						return nullptr;   // what FS_fopen returns for a file that isn't there
					}
				}
				else
				{
					fs_cache::detail::Invalidate(dir);   // a write: the listing may be stale
				}
			}
			++g_passed;
		}
		return g_fopen(self, name, options, flags, size);
	}

#ifdef _WIN64
	void* HookFopen(void* self, const char* name, const char* options, uintptr_t flags, int64_t* size)
	{
		return Fopen(self, name, options, flags, size);
	}
#else
	// thiscall: `this` in ecx, the rest on the stack, callee cleans; fastcall
	// with an unused edx has the same shape.
	void* __fastcall HookFopen(void* self, void*, const char* name, const char* options, uintptr_t flags, int64_t* size)
	{
		return Fopen(self, name, options, flags, size);
	}
#endif

	bool InModule(HMODULE mod, const void* p, size_t len)
	{
		uintptr_t base = 0;
		size_t size = 0;
		if (!pe_image::ImageRange(mod, base, size))
			return false;
		const auto a = reinterpret_cast<uintptr_t>(p);
		return a >= base && a + len <= base + size;
	}

	// The object's class from MSVC RTTI (the locator before the vtable).
	bool IsFileSystemStdio(HMODULE mod, void** vtable)
	{
		if (!InModule(mod, vtable - 1, sizeof(void*)))
			return false;
		auto* col = static_cast<const uint32_t*>(vtable[-1]);
		if (!InModule(mod, col, 6 * sizeof(uint32_t)) || col[1] != 0)   // offset 0: the primary vtable
			return false;
#ifdef _WIN64
		if (col[0] != 1)
			return false;
		const uintptr_t image = reinterpret_cast<uintptr_t>(col) - col[5];
		auto* name = reinterpret_cast<const char*>(image + col[3] + 16);
#else
		if (col[0] != 0)
			return false;
		auto* name = reinterpret_cast<const char*>(static_cast<uintptr_t>(col[3]) + 8);
#endif
		static const char kName[] = ".?AVCFileSystem_Stdio@@";
		return InModule(mod, name, sizeof(kName)) && std::memcmp(name, kName, sizeof(kName)) == 0;
	}
}
#endif

namespace fs_cache
{
	namespace detail
	{
		bool Split(const char* path, std::string& dir, std::string& name)
		{
			if (!path)
				return false;
			const size_t n = std::strlen(path);
			if (n < 4 || n >= 260)
				return false;
			const char d = path[0];
			if (!((d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z')) || path[1] != ':' || (path[2] != '\\' && path[2] != '/'))
				return false;
			std::string out = std::string(1, path[0]) + ":";
			std::string component;
			std::string last;
			for (size_t i = 3; i <= n; ++i)
			{
				const char c = i < n ? path[i] : '\\';
				if (c == '\\' || c == '/')
				{
					if (component.empty())
						continue;   // a doubled separator
					if (component == "." || component == ".." || component.back() == '.' || component.back() == ' ')
						return false;
					if (!last.empty())
						out += "\\" + last;
					last = component;
					component.clear();
					continue;
				}
				// wildcards, streams, short names, line breaks: not for the cache
				if (c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == ':' || c == '~' ||
					c == '\r' || c == '\n' || static_cast<unsigned char>(c) < 0x20)
					return false;
				component += c;
			}
			if (last.empty() || path[n - 1] == '\\' || path[n - 1] == '/')
				return false;
			if (!LowerAscii(out) || !LowerAscii(last))
				return false;
			dir = out;
			name = last;
			return true;
		}

		bool Excluded(const std::string& dir)
		{
			static const char* const kWritten[] = {
				"\\garrysmod\\data", "\\garrysmod\\download", "\\garrysmod\\cache", "\\garrysmod\\downloadlists",
				"\\garrysmod\\screenshots", "\\garrysmod\\cfg", "\\garrysmod\\settings", "\\steamapps\\workshop",
			};
			for (const char* w : kWritten)
			{
				const size_t at = dir.find(w);
				if (at == std::string::npos)
					continue;
				const size_t end = at + std::strlen(w);
				if (end == dir.size() || dir[end] == '\\')
					return true;
			}
			return false;
		}

		Answer Lookup(const std::string& dir, const std::string& name)
		{
#ifdef _WIN32
			std::shared_ptr<Listing> listing;
			AcquireSRWLockShared(&g_lock);
			auto it = g_folders.find(dir);
			if (it != g_folders.end())
				listing = it->second;
			ReleaseSRWLockShared(&g_lock);
			const unsigned join = g_join.load();
			if (listing && listing->join.load() != join)
			{
				// From an earlier join: used again only if the folder is as it was.
				if (StillHolds(dir, *listing))
				{
					listing->join = join;
					++g_reused;
				}
				else
				{
					listing = nullptr;
				}
			}
			if (!listing)
			{
				std::shared_ptr<Listing> fresh = List(dir);
				if (!fresh)
					return Answer::Unknown;
				++g_listed;
				AcquireSRWLockExclusive(&g_lock);
				std::shared_ptr<Listing>& slot = g_folders[dir];
				if (!slot || slot->join.load() != join)
					slot = fresh;   // another thread may have listed it for this join meanwhile
				listing = slot;
				ReleaseSRWLockExclusive(&g_lock);
			}
			if (!listing->exists)
				return Answer::Absent;
			return listing->names.count(name) ? Answer::Present : Answer::Absent;
#else
			(void)dir; (void)name;
			return Answer::Unknown;
#endif
		}

		void Invalidate(const std::string& dir)
		{
#ifdef _WIN32
			AcquireSRWLockExclusive(&g_lock);
			g_folders.erase(dir);
			ReleaseSRWLockExclusive(&g_lock);
#else
			(void)dir;
#endif
		}

		void Clear()
		{
#ifdef _WIN32
			AcquireSRWLockExclusive(&g_lock);
			g_folders.clear();
			ReleaseSRWLockExclusive(&g_lock);
#endif
		}

		void NewJoin()
		{
#ifdef _WIN32
			++g_join;
#endif
		}

		size_t Folders()
		{
#ifdef _WIN32
			AcquireSRWLockShared(&g_lock);
			const size_t n = g_folders.size();
			ReleaseSRWLockShared(&g_lock);
			return n;
#else
			return 0;
#endif
		}
	}

	bool Install(std::string& why)
	{
#ifdef _WIN32
		if (g_copy)
			return true;
		HMODULE fs = GetModuleHandleW(L"filesystem_stdio.dll");
		if (!fs)
		{
			why = "filesystem_stdio.dll not loaded";
			return false;
		}
		using CreateInterfaceFn = void* (*)(const char*, int*);
		auto create = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(fs, "CreateInterface"));
		void* object = create ? create("VFileSystem022", nullptr) : nullptr;
		if (!object)
		{
			why = "no VFileSystem022";
			return false;
		}
		void** vtable = *static_cast<void***>(object);
		if (!IsFileSystemStdio(fs, vtable))
		{
			why = "the file system isn't CFileSystem_Stdio";
			return false;
		}
		uint8_t* text = nullptr;
		size_t textSize = 0;
		if (!pe_image::TextSection(fs, text, textSize))
		{
			why = "filesystem_stdio has no code section";
			return false;
		}
		auto inText = [&](const void* p) { return p >= text && p < text + textSize; };
		size_t slots = 0;
		while (slots < 512 && inText(vtable[slots]))
			++slots;
		if (slots <= kFopenSlot)
		{
			why = "the file system's vtable is too short";
			return false;
		}
		auto* fopen = static_cast<const uint8_t*>(vtable[kFopenSlot]);
		if (!InModule(fs, fopen + kRbAt, 9) || std::memcmp(fopen + kRbAt, kRbCheck, 4) != 0 ||
			std::memcmp(fopen + kRbAt + 5, kRbCheck2, 4) != 0)
		{
			why = "slot 129 isn't FS_fopen (different GMod build)";
			return false;
		}

		// The copy: the locator, then every slot, FS_fopen's replaced. Never
		// freed: another thread may still be reading through it.
		g_copy = static_cast<void**>(VirtualAlloc(nullptr, (slots + 1) * sizeof(void*), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		if (!g_copy)
		{
			why = "couldn't allocate the vtable copy";
			return false;
		}
		g_copy[0] = vtable[-1];
		std::memcpy(g_copy + 1, vtable, slots * sizeof(void*));
		g_fopen = reinterpret_cast<FopenFn>(vtable[kFopenSlot]);
		g_copy[1 + kFopenSlot] = reinterpret_cast<void*>(&HookFopen);
		DWORD old;
		VirtualProtect(g_copy, (slots + 1) * sizeof(void*), PAGE_READONLY, &old);
		g_object = object;
		g_originalVtable = vtable;
		InterlockedExchangePointer(static_cast<PVOID*>(object), g_copy + 1);
		return true;
#else
		why = "Windows only";
		return false;
#endif
	}

	bool Uninstall()
	{
#ifdef _WIN32
		if (!g_object)
			return false;
		SetActive(false);
		detail::Clear();
		if (InterlockedCompareExchangePointer(static_cast<PVOID*>(g_object), g_originalVtable, g_copy + 1) != g_copy + 1)
			return false;   // someone else swapped it since: leave theirs
		g_object = nullptr;
		return true;
#else
		return false;
#endif
	}

	void SetActive(bool on)
	{
#ifdef _WIN32
		// Listings stay between joins; each new join checks a folder's write
		// time before it trusts the listing from an earlier one.
		if (on)
			detail::NewJoin();
		g_active = on && g_object;
#else
		(void)on;
#endif
	}

	std::string Describe()
	{
#ifdef _WIN32
		return std::string(g_object ? (g_active ? "on, active" : "on, idle") : "off") + ": " +
			std::to_string(g_absent.load()) + " of " + std::to_string(g_opens.load()) +
			" opens answered \"not there\" from " + std::to_string(g_listed.load()) + " folder listings (" +
			std::to_string(g_reused.load()) + " reused from earlier joins), " +
			std::to_string(g_passed.load()) + " opened as before";
#else
		return "unsupported";
#endif
	}
}
