#include "fs_cache.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{
	// 64-bit FNV-1a: the keys for folders and the names in them.
	constexpr uint64_t kFnvBasis = 14695981039346656037ULL;
	constexpr uint64_t kFnvPrime = 1099511628211ULL;

	inline uint64_t Fnv(uint64_t h, char c)
	{
		return (h ^ static_cast<unsigned char>(c)) * kFnvPrime;
	}

	uint64_t Fnv(const char* s, size_t n)
	{
		uint64_t h = kFnvBasis;
		for (size_t i = 0; i < n; ++i)
			h = Fnv(h, s[i]);
		return h;
	}

	inline char Lower(char c)
	{
		return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
	}

	inline bool IsSep(char c)
	{
		return c == '\\' || c == '/';
	}

	// A path split for the cache, on the stack: buf holds the lowercase folder,
	// a '\' and the name ("x:\a\b\name"), NUL-terminated.
	struct Parts
	{
		char buf[260];
		uint32_t dirLen = 0;     // "x:\a\b" (2, "x:", for a file in a drive's root)
		uint32_t nameOff = 0;    // dirLen + 1
		uint32_t nameLen = 0;
		uint64_t dirHash = 0;    // Fnv of the folder's bytes
		uint64_t nameHash = 0;   // Fnv of the name's bytes
	};

	// detail::Split's rules in one pass, with no allocation: the same paths are
	// refused (too short or long, not "X:\", ".", "..", a trailing dot or
	// space, the characters below, anything but ASCII, a trailing separator),
	// doubled separators are skipped, and the rest is written out lowercase with
	// '\' between the parts. The output is never longer than the path (each part
	// comes after at least one separator), so it fits.
	bool SplitFast(const char* path, Parts& p)
	{
		if (!path)
			return false;
		size_t n = 0;
		while (n < sizeof(p.buf) && path[n])
			++n;
		if (n < 4 || n >= sizeof(p.buf))
			return false;
		const char d = path[0];
		if (!((d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z')) || path[1] != ':' || !IsSep(path[2]))
			return false;
		char* out = p.buf;
		size_t len = 0;
		uint64_t h = kFnvBasis;
		out[len++] = Lower(d);
		h = Fnv(h, Lower(d));
		out[len++] = ':';
		h = Fnv(h, ':');
		bool open = false, any = false;
		size_t sep = 0, lastSep = 0;   // where the part's '\' is in buf
		uint64_t before = 0, lastBefore = 0, part = 0, lastPart = 0;   // hashes of buf up to that '\', of the part
		for (size_t i = 3; i <= n; ++i)
		{
			const char c = i < n ? path[i] : '\\';
			if (IsSep(c))
			{
				if (!open)
					continue;   // a doubled separator
				const char back = out[len - 1];
				if (back == '.' || back == ' ')
					return false;   // ".", "..", or a trailing dot or space
				open = false;
				any = true;
				lastSep = sep;
				lastBefore = before;
				lastPart = part;
				continue;
			}
			// wildcards, streams, short names, line breaks: not for the cache
			if (c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == ':' || c == '~' ||
				c == '\r' || c == '\n' || static_cast<unsigned char>(c) < 0x20)
				return false;
			if (static_cast<unsigned char>(c) >= 0x80)
				return false;   // anything but ASCII
			if (!open)
			{
				sep = len;
				before = h;
				out[len++] = '\\';
				h = Fnv(h, '\\');
				part = kFnvBasis;
				open = true;
			}
			const char lc = Lower(c);
			out[len++] = lc;
			h = Fnv(h, lc);
			part = Fnv(part, lc);
		}
		if (!any || IsSep(path[n - 1]))
			return false;
		out[len] = '\0';
		p.dirLen = static_cast<uint32_t>(lastSep);
		p.nameOff = static_cast<uint32_t>(lastSep + 1);
		p.nameLen = static_cast<uint32_t>(len - lastSep - 1);
		p.dirHash = lastBefore;
		p.nameHash = lastPart;
		return true;
	}

	// detail::Excluded with no allocation.
	bool ExcludedFast(const char* dir, size_t len)
	{
		static constexpr std::string_view kWritten[] = {
			"\\garrysmod\\data", "\\garrysmod\\download", "\\garrysmod\\cache", "\\garrysmod\\downloadlists",
			"\\garrysmod\\screenshots", "\\garrysmod\\cfg", "\\garrysmod\\settings", "\\steamapps\\workshop",
		};
		const std::string_view d(dir, len);
		for (const std::string_view w : kWritten)
		{
			const size_t at = d.find(w);
			if (at == std::string_view::npos)
				continue;
			const size_t end = at + w.size();
			if (end == len || dir[end] == '\\')
				return true;
		}
		return false;
	}
}

#ifdef _WIN32
#include "pe_image.h"
#include "prewarm.h"

namespace
{
	using fs_cache::detail::Answer;
	using fs_cache::detail::TestPoint;

	// --- The folder listings -----------------------------------------------------
	// Everything but `join` is set before a listing is kept, and never changes.
	struct Listing
	{
		std::string dir;                   // lowercase, as Split gives it
		bool exists = false;
		bool reusable = false;             // a plain folder that had settled before it was listed: its write time tells when names change
		bool link = false;                 // a reparse point: its own write time doesn't follow its target's names
		bool unknown = false;              // couldn't be read: "can't tell", for this join only
		bool excluded = false;             // something else writes to it: never listed
		FILETIME written = {};             // the folder's last-write time, read after the listing
		std::vector<uint64_t> names;       // Fnv of each lowercase ASCII name, sorted
		std::atomic<unsigned> join{ 0 };   // the join it was last listed or checked in
	};

	// The folder hash is already well mixed.
	struct KeyHash
	{
		size_t operator()(uint64_t h) const noexcept { return static_cast<size_t>(h ^ (h >> 32)); }
	};

	// By folder hash; the few folders that share one are told apart by `dir`.
	SRWLOCK g_lock = SRWLOCK_INIT;
	std::unordered_map<uint64_t, std::vector<std::shared_ptr<Listing>>, KeyHash> g_folders;

	std::atomic<bool> g_active{ false };
	std::atomic<unsigned> g_join{ 1 };
	std::atomic<uint64_t> g_epoch{ 0 };   // counts up at every drop: a listing made meanwhile isn't kept
	std::atomic<long long> g_opens{ 0 }, g_absent{ 0 }, g_listed{ 0 }, g_reused{ 0 }, g_passed{ 0 }, g_inferred{ 0 };
	std::atomic<void (*)(TestPoint)> g_testHook{ nullptr };

	constexpr unsigned kKeepJoins = 3;    // a listing not used in this many joins is dropped
	constexpr size_t kMaxAncestors = 8;   // how far up a folder's answer may come from
	constexpr size_t kMaxSeps = 260;      // a path in a 260-byte buffer has fewer separators

	void AtTestPoint(TestPoint at)
	{
		if (auto hook = g_testHook.load(std::memory_order_acquire))
			hook(at);
	}

	// Under g_lock, either mode: the folder's entry, or null.
	std::shared_ptr<Listing>* FindLocked(uint64_t hash, const char* dir, size_t len)
	{
		auto it = g_folders.find(hash);
		if (it == g_folders.end())
			return nullptr;
		for (std::shared_ptr<Listing>& l : it->second)
			if (l->dir.size() == len && std::memcmp(l->dir.data(), dir, len) == 0)
				return &l;
		return nullptr;
	}

	// Under g_lock, exclusive: takes the folder's entry out into `dropped`, to be
	// freed after the lock.
	void DropLocked(uint64_t hash, const char* dir, size_t len, std::vector<std::shared_ptr<Listing>>& dropped)
	{
		auto it = g_folders.find(hash);
		if (it == g_folders.end())
			return;
		std::vector<std::shared_ptr<Listing>>& v = it->second;
		for (size_t i = 0; i < v.size(); ++i)
		{
			if (v[i]->dir.size() == len && std::memcmp(v[i]->dir.data(), dir, len) == 0)
			{
				dropped.push_back(std::move(v[i]));
				v[i] = std::move(v.back());
				v.pop_back();
				break;
			}
		}
		if (v.empty())
			g_folders.erase(it);
	}

	// The '\' positions in a folder's path ("x:\a\b": 2, 4), with the hash of
	// the path before each, which is the hash of the folder above. Returns how many.
	size_t Seps(const char* dir, size_t len, uint32_t (&at)[kMaxSeps], uint64_t (&before)[kMaxSeps])
	{
		size_t k = 0;
		uint64_t h = kFnvBasis;
		for (size_t i = 0; i < len; ++i)
		{
			if (dir[i] == '\\' && k < kMaxSeps)
			{
				at[k] = static_cast<uint32_t>(i);
				before[k] = h;
				++k;
			}
			h = Fnv(h, dir[i]);
		}
		return k;
	}

	// The folder as it is now: absent, or present with its write time and
	// whether it's a link (a link's own time doesn't follow its target's names).
	bool Probe(const char* dir, bool& exists, FILETIME& written, bool& link)
	{
		WIN32_FILE_ATTRIBUTE_DATA a;
		if (!GetFileAttributesExA(dir, GetFileExInfoStandard, &a))
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

	// A present listing from an earlier join still holds if the folder is a
	// plain folder whose write time (it changes whenever a name is added,
	// removed or renamed) hasn't moved. Never one that couldn't be read.
	bool StillHolds(const Listing& l)
	{
		if (l.unknown || !l.exists || !l.reusable)
			return false;
		bool exists = false, link = false;
		FILETIME written = {};
		if (!Probe(l.dir.c_str(), exists, written, link))
			return false;
		return exists && !link && CompareFileTime(&written, &l.written) == 0;
	}

	unsigned long long Ticks(const FILETIME& f)
	{
		return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
	}

	// Reads one folder: absent, present with its names, or "can't tell"
	// (access denied, say). The listing first, its write time after (fs_cache.h
	// has why): a folder that isn't there costs one call.
	std::shared_ptr<Listing> List(const char* dir, size_t len, unsigned join)
	{
		auto listing = std::make_shared<Listing>();
		listing->dir.assign(dir, len);
		listing->join = join;
		char pattern[sizeof(Parts::buf) + 4];
		if (len + 3 > sizeof(pattern))
		{
			listing->exists = true;
			listing->unknown = true;
			return listing;
		}
		std::memcpy(pattern, dir, len);
		pattern[len] = '\\';
		pattern[len + 1] = '*';
		pattern[len + 2] = '\0';
		FILETIME start;
		GetSystemTimeAsFileTime(&start);
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileExA(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
		if (h == INVALID_HANDLE_VALUE)
		{
			const DWORD e = GetLastError();
			if (e == ERROR_PATH_NOT_FOUND || e == ERROR_FILE_NOT_FOUND || e == ERROR_DIRECTORY || e == ERROR_INVALID_NAME)
				return listing;   // no such folder: nothing in it can be opened
			listing->exists = true;
			listing->unknown = true;
			return listing;
		}
		listing->exists = true;
		do
		{
			uint64_t hash = kFnvBasis;
			bool ascii = true;
			for (const char* s = fd.cFileName; *s; ++s)
			{
				if (static_cast<unsigned char>(*s) >= 0x80)
				{
					ascii = false;
					break;
				}
				hash = Fnv(hash, Lower(*s));
			}
			// A name with other characters can't match a query (queries are
			// ASCII), so leaving it out never answers "absent" for a real file.
			if (ascii)
				listing->names.push_back(hash);
		} while (FindNextFileA(h, &fd));
		FindClose(h);
		AtTestPoint(TestPoint::Listed);
		std::sort(listing->names.begin(), listing->names.end());
		listing->names.erase(std::unique(listing->names.begin(), listing->names.end()), listing->names.end());
		listing->names.shrink_to_fit();
		// Reusable in a later join only if nothing changed in the 2 s before the
		// listing started, nor since. A changed folder is listed again next join.
		bool exists = false, link = false;
		pattern[len] = '\0';
		if (Probe(pattern, exists, listing->written, link) && exists)
		{
			const unsigned long long kSettled = 2ULL * 10000000ULL;   // 2 s in 100 ns units
			listing->link = link;
			listing->reusable = !link && Ticks(listing->written) + kSettled <= Ticks(start);
		}
		return listing;
	}

	// A folder this join doesn't know yet, from the nearest folder above it that
	// this join does know: absent if that one is absent, or was listed without
	// the next name on the way down. Never from a link, a folder that couldn't
	// be read or one not checked this join. No system call.
	bool InferAbsent(const Parts& p, unsigned join)
	{
		uint32_t at[kMaxSeps];
		uint64_t before[kMaxSeps];
		const size_t k = Seps(p.buf, p.dirLen, at, before);
		const size_t stop = k > kMaxAncestors ? k - kMaxAncestors : 0;
		bool absent = false;
		AcquireSRWLockShared(&g_lock);
		for (size_t i = k; i-- > stop;)
		{
			const std::shared_ptr<Listing>* found = FindLocked(before[i], p.buf, at[i]);
			if (!found)
				continue;
			const Listing& above = **found;
			if (!above.excluded && !above.link && !above.unknown && above.join.load() == join)
			{
				if (!above.exists)
				{
					absent = true;
				}
				else
				{
					const size_t from = at[i] + 1;
					const size_t to = i + 1 < k ? at[i + 1] : p.dirLen;
					absent = !std::binary_search(above.names.begin(), above.names.end(), Fnv(p.buf + from, to - from));
				}
			}
			break;   // the nearest folder known above decides
		}
		ReleaseSRWLockShared(&g_lock);
		return absent;
	}

	// Keeps a new listing for this join unless a write dropped anything since
	// `epoch` was read (then null: it may have missed what the write made).
	// Another thread's listing for this join, kept meanwhile, wins.
	std::shared_ptr<Listing> Keep(std::shared_ptr<Listing> fresh, uint64_t hash, uint64_t epoch, unsigned join)
	{
		AtTestPoint(TestPoint::BeforeInsert);
		std::shared_ptr<Listing> kept, displaced;   // displaced is freed after the lock
		AcquireSRWLockExclusive(&g_lock);
		if (g_epoch.load(std::memory_order_acquire) == epoch)
		{
			std::shared_ptr<Listing>* slot = FindLocked(hash, fresh->dir.data(), fresh->dir.size());
			if (slot && (*slot)->join.load() == join)
			{
				kept = *slot;
			}
			else
			{
				kept = fresh;
				if (slot)
				{
					displaced = std::move(*slot);
					*slot = std::move(fresh);
				}
				else
				{
					g_folders[hash].push_back(std::move(fresh));
				}
			}
		}
		ReleaseSRWLockExclusive(&g_lock);
		return kept;
	}

	// A folder this join doesn't know yet: excluded, absent from the folder
	// above, or listed. Null if a write got in the way.
	std::shared_ptr<Listing> Fill(const Parts& p, unsigned join)
	{
		const uint64_t epoch = g_epoch.load(std::memory_order_acquire);
		std::shared_ptr<Listing> fresh;
		if (ExcludedFast(p.buf, p.dirLen))
		{
			fresh = std::make_shared<Listing>();
			fresh->dir.assign(p.buf, p.dirLen);
			fresh->exists = true;
			fresh->excluded = true;
			fresh->join = join;
		}
		else if (InferAbsent(p, join))
		{
			fresh = std::make_shared<Listing>();
			fresh->dir.assign(p.buf, p.dirLen);
			fresh->join = join;   // not reusable: a later join checks it again
			++g_inferred;
		}
		else
		{
			fresh = List(p.buf, p.dirLen, join);
			++g_listed;
		}
		return Keep(std::move(fresh), p.dirHash, epoch, join);
	}

	Answer LookupParts(const Parts& p)
	{
		const unsigned join = g_join.load();
		std::shared_ptr<Listing> listing;
		AcquireSRWLockShared(&g_lock);
		if (std::shared_ptr<Listing>* found = FindLocked(p.dirHash, p.buf, p.dirLen))
			listing = *found;
		ReleaseSRWLockShared(&g_lock);
		if (listing && listing->join.load() != join)
		{
			// From an earlier join: used again only if the folder is as it was.
			// An absent one is looked at again (from the folder above, or one call).
			if (listing->excluded)
			{
				listing->join = join;   // still in use: not dropped
			}
			else if (StillHolds(*listing))
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
			listing = Fill(p, join);
			if (!listing)
				return Answer::Unknown;
		}
		if (listing->excluded || listing->unknown)
			return Answer::Unknown;
		if (!listing->exists)
			return Answer::Absent;
		return std::binary_search(listing->names.begin(), listing->names.end(), p.nameHash) ? Answer::Present : Answer::Absent;
	}

	// What a write does: drops the folder and every folder above it, so none of
	// them can answer for a name the write makes. Counts the epoch up first, so
	// a listing that's under way isn't kept.
	void InvalidateDir(const char* dir, size_t len)
	{
		uint32_t at[kMaxSeps];
		uint64_t before[kMaxSeps];
		const size_t k = Seps(dir, len, at, before);
		const uint64_t whole = Fnv(dir, len);
		std::vector<std::shared_ptr<Listing>> dropped;   // freed after the lock
		dropped.reserve(k + 1);
		AcquireSRWLockExclusive(&g_lock);
		g_epoch.fetch_add(1, std::memory_order_acq_rel);
		DropLocked(whole, dir, len, dropped);
		for (size_t i = 0; i < k; ++i)
			DropLocked(before[i], dir, at[i], dropped);
		ReleaseSRWLockExclusive(&g_lock);
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
		const bool active = g_active.load(std::memory_order_relaxed);
		Parts parts;
		bool wrote = false;
		if (active)
		{
			++g_opens;
			if (SplitFast(name, parts))
			{
				if (ReadsOnly(options))
				{
					if (LookupParts(parts) == Answer::Absent)
					{
						++g_absent;
						SetLastError(ERROR_FILE_NOT_FOUND);
						return nullptr;   // what FS_fopen returns for a file that isn't there
					}
				}
				else if (!ExcludedFast(parts.buf, parts.dirLen))
				{
					InvalidateDir(parts.buf, parts.dirLen);   // a write: the listings may be stale
					wrote = true;
				}
			}
			++g_passed;
		}
		void* file = g_fopen(self, name, options, flags, size);
		// Again now the file exists: a listing made since the first drop may
		// have missed it, and the epoch keeps one still under way from being kept.
		if (wrote)
			InvalidateDir(parts.buf, parts.dirLen);
		// The join's big files, for warming them before the next join to this
		// server, and its Workshop items (prewarm.h). FS_fopen gives back the
		// file's size; prewarm keeps only what it needs.
		if (active && file && ReadsOnly(options))
			prewarm::Opened(name, size ? *size : -1);
		return file;
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
			Parts p;
			if (!SplitFast(path, p))
				return false;
			dir.assign(p.buf, p.dirLen);
			name.assign(p.buf + p.nameOff, p.nameLen);
			return true;
		}

		bool Excluded(const std::string& dir)
		{
			return ExcludedFast(dir.data(), dir.size());
		}

		Answer Lookup(const std::string& dir, const std::string& name)
		{
#ifdef _WIN32
			Parts p;
			if (dir.size() + 1 + name.size() >= sizeof(p.buf))
				return Answer::Unknown;
			std::memcpy(p.buf, dir.data(), dir.size());
			p.buf[dir.size()] = '\\';
			std::memcpy(p.buf + dir.size() + 1, name.data(), name.size());
			p.buf[dir.size() + 1 + name.size()] = '\0';
			p.dirLen = static_cast<uint32_t>(dir.size());
			p.nameOff = p.dirLen + 1;
			p.nameLen = static_cast<uint32_t>(name.size());
			p.dirHash = Fnv(dir.data(), dir.size());
			p.nameHash = Fnv(name.data(), name.size());
			return LookupParts(p);
#else
			(void)dir; (void)name;
			return Answer::Unknown;
#endif
		}

		void Invalidate(const std::string& dir)
		{
#ifdef _WIN32
			InvalidateDir(dir.data(), dir.size());
#else
			(void)dir;
#endif
		}

		void Clear()
		{
#ifdef _WIN32
			decltype(g_folders) dropped;   // freed after the lock
			AcquireSRWLockExclusive(&g_lock);
			g_folders.swap(dropped);
			g_epoch.fetch_add(1, std::memory_order_acq_rel);
			ReleaseSRWLockExclusive(&g_lock);
#endif
		}

		void NewJoin()
		{
#ifdef _WIN32
			// Drops what no join used lately: folders differ from server to
			// server, and on 32-bit the address space is worth having back.
			const unsigned join = ++g_join;
			std::vector<std::shared_ptr<Listing>> dropped;   // freed after the lock
			AcquireSRWLockExclusive(&g_lock);
			for (auto it = g_folders.begin(); it != g_folders.end();)
			{
				std::vector<std::shared_ptr<Listing>>& v = it->second;
				for (size_t i = 0; i < v.size();)
				{
					if (v[i]->join.load() + kKeepJoins < join)
					{
						dropped.push_back(std::move(v[i]));
						v[i] = std::move(v.back());
						v.pop_back();
					}
					else
					{
						++i;
					}
				}
				it = v.empty() ? g_folders.erase(it) : std::next(it);
			}
			ReleaseSRWLockExclusive(&g_lock);
#endif
		}

		size_t Folders()
		{
#ifdef _WIN32
			size_t n = 0;
			AcquireSRWLockShared(&g_lock);
			for (const auto& bucket : g_folders)
				n += bucket.second.size();
			ReleaseSRWLockShared(&g_lock);
			return n;
#else
			return 0;
#endif
		}

		void SetTestHook(void (*hook)(TestPoint))
		{
#ifdef _WIN32
			g_testHook.store(hook, std::memory_order_release);
#else
			(void)hook;
#endif
		}

		bool SwapIn(void** slot, void* original, void* ours)
		{
#ifdef _WIN32
			if (*slot == ours)
				return true;
			return InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(slot), ours, original) == original;
#else
			(void)slot; (void)original; (void)ours;
			return false;
#endif
		}
	}

	bool Install(std::string& why)
	{
#ifdef _WIN32
		if (g_object)
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
		if (g_copy)
		{
			// Taken out earlier this session (Uninstall keeps the copy: another
			// thread may still be reading through it). The same copy goes back,
			// over the game's own vtable only. Before 1.7.1 this returned at once,
			// and the hook stayed out for the rest of the session.
			if (!detail::SwapIn(static_cast<void**>(object), g_originalVtable, g_copy + 1))
			{
				why = "the file system's vtable was replaced by something else since";
				return false;
			}
			g_object = object;
			return true;
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
		// time before it trusts the listing from an earlier one, and drops the
		// ones no recent join used.
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
			std::to_string(g_inferred.load()) + " folders known absent from the folder above, " +
			std::to_string(g_passed.load()) + " opened as before";
#else
		return "unsupported";
#endif
	}
}
