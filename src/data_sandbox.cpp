#include "data_sandbox.h"
#include "file_util.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace
{
	const wchar_t kMenu[] = L"menu";
	const wchar_t kBefore[] = L"before-sandbox";
	const wchar_t kOther[] = L"other";
	const wchar_t kP2P[] = L"p2p";

	// The vault's "active" file. Before 1.6.1 it said whose files a plain data/
	// held; Recover() still reads that once, to convert. Since then it only says
	// "linked": an older installer's uninstall reads it to know whose files are
	// in data/, and any value but "menu", "before-sandbox" or none makes it move
	// data/'s files to its backup folder. With "menu" it would copy the vault's
	// menu folder onto data/, which is that same folder through the link, and
	// delete each file as a duplicate of itself.
	const char kLinked[] = "linked";

	// --- Paths ----------------------------------------------------------------

	DWORD Attributes(const std::wstring& p) { return GetFileAttributesW(p.c_str()); }
	bool Exists(const std::wstring& p) { return Attributes(p) != INVALID_FILE_ATTRIBUTES; }
	// A folder, or a link to one (attributes are the link's own: even a link
	// whose folder is gone counts).
	bool IsDir(const std::wstring& p)
	{
		DWORD a = Attributes(p);
		return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
	}
	bool IsLink(const std::wstring& p)
	{
		DWORD a = Attributes(p);
		return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
	}
	// A real folder, not a link to one.
	bool IsPlainDir(const std::wstring& p) { return IsDir(p) && !IsLink(p); }

	std::wstring Widen(const std::string& s)
	{
		if (s.empty())
			return std::wstring();
		int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
		std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
		if (n > 0)
			MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
		return w;
	}

	std::string Narrow(const std::wstring& w)
	{
		if (w.empty())
			return std::string();
		int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
		std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
		if (n > 0)
			WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
		return s;
	}

	// Absolute, with "." and ".." resolved and no trailing backslash (except a
	// drive root): the form a junction's target must have.
	std::wstring FullPath(const std::wstring& p)
	{
		DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
		if (n == 0)
			return std::wstring();
		std::wstring out(n, L'\0');
		n = GetFullPathNameW(p.c_str(), n, &out[0], nullptr);
		out.resize(n);
		while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/'))
			out.pop_back();
		return out;
	}

	std::wstring Parent(const std::wstring& p)
	{
		size_t slash = p.find_last_of(L"\\/");
		return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash);
	}

	std::wstring Leaf(const std::wstring& p)
	{
		size_t slash = p.find_last_of(L"\\/");
		return slash == std::wstring::npos ? p : p.substr(slash + 1);
	}

	bool SameText(const std::wstring& a, const std::wstring& b)
	{
		return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
	}

	bool EndsWithText(const std::wstring& s, const std::wstring& suffix)
	{
		return s.size() >= suffix.size() && SameText(s.substr(s.size() - suffix.size()), suffix);
	}

	// The folder that holds this module.
	std::wstring OwnFolder()
	{
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&OwnFolder), &self);
		wchar_t path[MAX_PATH];
		DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
		if (n == 0 || n >= MAX_PATH)
			return std::wstring();
		return Parent(std::wstring(path, n));
	}

	// garrysmod\data: the game's working folder is the install root (GarrysMod\);
	// failing that, this module is in garrysmod\lua\bin. A folder or a link.
	std::wstring DataDir()
	{
		static std::wstring dir;
		if (!dir.empty())
			return dir;
		std::vector<std::wstring> candidates;
		wchar_t cwd[MAX_PATH];
		DWORD n = GetCurrentDirectoryW(MAX_PATH, cwd);
		if (n && n < MAX_PATH)
			candidates.push_back(std::wstring(cwd) + L"\\garrysmod\\data");
		std::wstring own = OwnFolder();
		if (!own.empty())
			candidates.push_back(own + L"\\..\\..\\data");
		for (const std::wstring& c : candidates)
		{
			std::wstring full = FullPath(c);
			if (!full.empty() && IsDir(full))
				return dir = full;
		}
		return dir;
	}

	std::wstring Volume(const std::wstring& path)
	{
		wchar_t v[MAX_PATH];
		return GetVolumePathNameW(path.c_str(), v, MAX_PATH) ? std::wstring(v) : std::wstring();
	}

	// Where a new vault goes: %LOCALAPPDATA%\MeowyClientPack\vault when that's
	// on the game's drive (converting data/ is then a rename), else
	// steamapps\MeowyClientPackVault, beside the game but outside its folder.
	// Both are out of reach of GMod's file paths. (A vault data/ already links
	// to is used wherever it is.)
	std::wstring DefaultVault(const std::wstring& data)
	{
		const std::wstring garrysmod = Parent(data);
		wchar_t base[MAX_PATH];
		DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
		const std::wstring gameVolume = Volume(garrysmod);
		if (n && n < MAX_PATH && !gameVolume.empty() && SameText(Volume(base), gameVolume))
			return FullPath(std::wstring(base) + L"\\MeowyClientPack\\vault");
		// garrysmod -> GarrysMod -> common -> steamapps
		return FullPath(garrysmod + L"\\..\\..\\..\\MeowyClientPackVault");
	}

	// A vault folder of the pack's (either location).
	bool IsVault(const std::wstring& dir)
	{
		return EndsWithText(dir, L"\\MeowyClientPack\\vault") || EndsWithText(dir, L"\\MeowyClientPackVault");
	}

	// A folder and its missing parents.
	bool EnsureDir(const std::wstring& dir)
	{
		if (IsDir(dir))
			return true;
		std::wstring parent = Parent(dir);
		if (parent.empty() || parent == dir || (parent.size() > 2 && !EnsureDir(parent)))
			return false;
		return CreateDirectoryW(dir.c_str(), nullptr) || IsDir(dir);
	}

	std::vector<std::wstring> Entries(const std::wstring& dir)
	{
		std::vector<std::wstring> out;
		WIN32_FIND_DATAW f;
		HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &f);
		if (h == INVALID_HANDLE_VALUE)
			return out;
		do
		{
			if (std::wcscmp(f.cFileName, L".") && std::wcscmp(f.cFileName, L".."))
				out.push_back(f.cFileName);
		} while (FindNextFileW(h, &f));
		FindClose(h);
		return out;
	}

	// "<stem>.<label>[-n]<ext>" that doesn't exist in `dir` yet.
	std::wstring FreeName(const std::wstring& dir, const std::wstring& name, const std::wstring& label)
	{
		size_t dot = name.find_last_of(L'.');
		std::wstring stem = dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
		std::wstring ext = dot == std::wstring::npos || dot == 0 ? std::wstring() : name.substr(dot);
		for (int i = 1;; ++i)
		{
			std::wstring candidate = dir + L"\\" + stem + L"." + label + (i > 1 ? L"-" + std::to_wstring(i) : L"") + ext;
			if (!Exists(candidate))
				return candidate;
		}
	}

	// Moves everything in `from` into `to` (created if missing). Never
	// overwrites and never deletes a file: a folder in both is merged; any other
	// clash keeps both, the one that doesn't get the name renamed to
	// "<stem>.<label>[-n]<ext>". `fromWins`: `from`'s entry takes the name.
	// Same drive (renames). True when `from` ended up empty.
	bool MergeInto(const std::wstring& from, const std::wstring& to, const std::wstring& label, bool fromWins)
	{
		if (!EnsureDir(to))
			return false;
		for (const std::wstring& e : Entries(from))
		{
			const std::wstring src = from + L"\\" + e, dst = to + L"\\" + e;
			if (!Exists(dst))
			{
				MoveFileExW(src.c_str(), dst.c_str(), 0);
			}
			else if (IsPlainDir(src) && IsPlainDir(dst))
			{
				MergeInto(src, dst, label, fromWins);
				RemoveDirectoryW(src.c_str());   // only if it's empty now
			}
			else if (fromWins)
			{
				if (MoveFileExW(dst.c_str(), FreeName(to, e, label).c_str(), 0))
					MoveFileExW(src.c_str(), dst.c_str(), 0);
			}
			else
			{
				MoveFileExW(src.c_str(), FreeName(to, e, label).c_str(), 0);
			}
		}
		return Entries(from).empty();
	}

	std::string ReadSmallFile(const std::wstring& path)
	{
		HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return std::string();
		char buf[512];
		DWORD got = 0;
		if (!ReadFile(h, buf, sizeof(buf), &got, nullptr))
			got = 0;
		CloseHandle(h);
		std::string s(buf, got);
		while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
			s.pop_back();
		return s;
	}

	// --- Junctions --------------------------------------------------------------

	// REPARSE_DATA_BUFFER's mount-point form (ntifs.h; not in the user-mode SDK).
	// The path buffer follows: the substitute name ("\??\C:\..."), then the
	// print name, each NUL-terminated.
	struct MountPointHeader
	{
		DWORD tag;
		WORD dataLength;   // everything after these first 8 bytes
		WORD reserved;
		WORD substituteOffset, substituteLength, printOffset, printLength;   // bytes, within the path buffer
	};
	constexpr DWORD kTagAndLength = 8;

	HANDLE OpenLink(const std::wstring& path, DWORD access)
	{
		return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	}

	// The folder the junction at `path` points to ("C:\..."), or "" when it
	// isn't a junction to a folder.
	std::wstring LinkTarget(const std::wstring& path)
	{
		if (!IsLink(path))
			return std::wstring();
		HANDLE h = OpenLink(path, FILE_READ_ATTRIBUTES);
		if (h == INVALID_HANDLE_VALUE)
			return std::wstring();
		std::vector<BYTE> buf(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
		DWORD got = 0;
		BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr);
		CloseHandle(h);
		MountPointHeader hdr;
		if (!ok || got < sizeof(hdr))
			return std::wstring();
		std::memcpy(&hdr, buf.data(), sizeof(hdr));
		const size_t start = sizeof(hdr) + hdr.substituteOffset;
		if (hdr.tag != IO_REPARSE_TAG_MOUNT_POINT || hdr.substituteLength % 2 || start % 2 || start + hdr.substituteLength > got)
			return std::wstring();
		std::wstring target(reinterpret_cast<const wchar_t*>(buf.data() + start), hdr.substituteLength / 2);
		// "\??\C:\...": a folder. ("\??\Volume{...}\" is a whole drive mounted there: not ours.)
		if (target.rfind(L"\\??\\", 0) != 0 || target.size() < 7 || target[5] != L':')
			return std::wstring();
		target.erase(0, 4);
		while (target.size() > 3 && target.back() == L'\\')
			target.pop_back();
		return target;
	}

	// Points the junction at `link` (an existing junction, or an empty folder) to
	// the folder `target` (a full path), in one call. Windows refuses a folder
	// with anything in it, so a real data/ can't be hidden behind a link here.
	bool SetLink(const std::wstring& link, const std::wstring& target, DWORD* error = nullptr)
	{
		const std::wstring sub = L"\\??\\" + target;
		const size_t subBytes = sub.size() * sizeof(wchar_t), printBytes = target.size() * sizeof(wchar_t);
		const size_t pathBytes = subBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t);
		if (sizeof(MountPointHeader) + pathBytes > MAXIMUM_REPARSE_DATA_BUFFER_SIZE)
		{
			if (error)
				*error = ERROR_FILENAME_EXCED_RANGE;
			return false;
		}
		MountPointHeader hdr{};
		hdr.tag = IO_REPARSE_TAG_MOUNT_POINT;
		hdr.dataLength = static_cast<WORD>(sizeof(hdr) - kTagAndLength + pathBytes);
		hdr.substituteOffset = 0;
		hdr.substituteLength = static_cast<WORD>(subBytes);
		hdr.printOffset = static_cast<WORD>(subBytes + sizeof(wchar_t));
		hdr.printLength = static_cast<WORD>(printBytes);
		std::vector<BYTE> buf(sizeof(hdr) + pathBytes, 0);
		std::memcpy(buf.data(), &hdr, sizeof(hdr));
		std::memcpy(buf.data() + sizeof(hdr), sub.c_str(), subBytes);
		std::memcpy(buf.data() + sizeof(hdr) + hdr.printOffset, target.c_str(), printBytes);

		HANDLE h = OpenLink(link, GENERIC_WRITE);
		if (h == INVALID_HANDLE_VALUE)
		{
			if (error)
				*error = GetLastError();
			return false;
		}
		DWORD ret = 0;
		BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), static_cast<DWORD>(buf.size()), nullptr, 0, &ret, nullptr);
		DWORD err = ok ? ERROR_SUCCESS : GetLastError();
		CloseHandle(h);
		if (ok && !SameText(LinkTarget(link), target))
			err = ERROR_INVALID_DATA;
		if (error)
			*error = err;
		return err == ERROR_SUCCESS;
	}

	// A new junction at `link` (which must not exist yet) to `target`.
	bool MakeLink(const std::wstring& link, const std::wstring& target, DWORD* error)
	{
		if (!CreateDirectoryW(link.c_str(), nullptr))
		{
			*error = GetLastError();
			return false;
		}
		if (SetLink(link, target, error))
			return true;
		RemoveDirectoryW(link.c_str());
		return false;
	}

	// --- Keys -------------------------------------------------------------------

	// "a.b.c.d:port", numbers in range.
	bool IsIpPort(const std::string& address)
	{
		unsigned a, b, c, d, port;
		char tail;
		if (address.size() > 21 || sscanf_s(address.c_str(), "%3u.%3u.%3u.%3u:%5u%c", &a, &b, &c, &d, &port, &tail, 1) != 5)
			return false;
		return a <= 255 && b <= 255 && c <= 255 && d <= 255 && port >= 1 && port <= 65535 &&
			address.find_first_not_of("0123456789.:") == std::string::npos;
	}

	// "1.2.3.4:27015" -> "1.2.3.4_27015".
	std::wstring KeyFor(const std::string& address)
	{
		std::wstring k;
		for (char c : address)
			k += (c >= '0' && c <= '9') || c == '.' ? static_cast<wchar_t>(c) : L'_';
		return k;
	}

	// The folder for a connection whose address isn't "a.b.c.d:port": fixed
	// names, never made from the address. All Steam P2P hosts share one,
	// everything else another.
	std::wstring KindKey(const std::string& address)
	{
		std::string lower;
		for (char c : address.substr(0, 16))
			lower += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
		if (lower.rfind("p2p:", 0) == 0 || lower.rfind("steamid:", 0) == 0 || lower.rfind("[", 0) == 0 || lower.rfind("steam_", 0) == 0)
			return kP2P;
		return kOther;
	}

	// One plain folder name: letters, digits, '.', '_', '-'; never only dots.
	bool IsSafeKey(const std::wstring& k)
	{
		if (k.empty() || k.size() > 64 || k.find_first_not_of(L'.') == std::wstring::npos)
			return false;
		for (wchar_t c : k)
			if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'.' || c == L'_' || c == L'-'))
				return false;
		return true;
	}

	// Whose files a plain data/ holds, from the "active" file a pre-1.6.1 pack
	// wrote: "<key>", or mid-switch "switching:<from>><to>:<phase>" (phase 1
	// moving data/ out: <from>'s; phase 2 moving <to>'s in: <to>'s), or the
	// oldest "switching:<to>". No file: never sandboxed, data/ is everything from
	// before the pack. "linked": data/ was a link of this version's that was
	// removed since (by hand, or an older version); what's in it now was written
	// since, and goes with the menu's.
	std::wstring LegacyOwner(const std::wstring& active)
	{
		std::wstring owner = active;
		if (active.rfind(L"switching:", 0) == 0)
		{
			std::wstring rest = active.substr(10);
			size_t arrow = rest.find(L'>'), colon = rest.rfind(L':');
			if (arrow != std::wstring::npos && colon != std::wstring::npos && colon > arrow)
				owner = rest.substr(colon + 1) == L"1" ? rest.substr(0, arrow) : rest.substr(arrow + 1, colon - arrow - 1);
			else
				owner = rest;
		}
		if (owner.empty())
			return kBefore;
		if (owner == Widen(kLinked))
			return kMenu;
		return IsSafeKey(owner) ? owner : std::wstring(kBefore);
	}

	// --- State --------------------------------------------------------------------

	struct State
	{
		bool loaded = false;
		bool linked = false;       // data/ is our link; `vault` and `key` are valid
		std::wstring data, vault, key;
		std::string off;           // why not, when !linked
		std::string problem;       // the last switch that failed
		ULONGLONG retryAt = 0;     // no new attempt before this (GetTickCount64)
	};
	State g;

	void Load()
	{
		if (g.loaded)
			return;
		g.loaded = true;
		g.data = DataDir();
		if (g.data.empty())
		{
			g.off = "garrysmod/data wasn't found";
			return;
		}
		const std::wstring target = LinkTarget(g.data);
		if (!target.empty() && IsVault(Parent(target)) && IsSafeKey(Leaf(target)))
		{
			g.linked = true;
			g.vault = Parent(target);
			g.key = Leaf(target);
			return;
		}
		g.off = IsLink(g.data) ? "garrysmod/data is a link the pack didn't make, so the pack leaves it alone"
			: "garrysmod/data isn't linked (the pack links it when it starts with the game)";
	}

	// data/ links to the vault folder `key`. A failure leaves the link where it
	// was and is tried again two seconds later, not every frame.
	void SwitchTo(const std::wstring& key)
	{
		Load();
		if (!g.linked || key == g.key)
			return;
		const ULONGLONG now = GetTickCount64();
		if (now < g.retryAt)
			return;
		const std::wstring target = g.vault + L"\\" + key;
		DWORD error = ERROR_SUCCESS;
		if (!EnsureDir(target))
			error = GetLastError();
		else if (SetLink(g.data, target, &error))
		{
			g.key = key;
			g.retryAt = 0;
			return;
		}
		g.retryAt = now + 2000;
		g.problem = "couldn't link garrysmod/data to " + Narrow(key) + " (error " + std::to_string(error) + ")";
	}

	// Links don't work on this drive (FAT32 or exFAT, say): no sandbox. data/
	// gets the player's own files back, as without the pack (on a clash both are
	// kept); each server's folder stays in the vault, out of every server's reach.
	std::string Unsandboxed(const std::wstring& data, const std::wstring& vault, const std::wstring& owner, DWORD error)
	{
		if (owner != kMenu && owner != kBefore)
			MergeInto(data, vault + L"\\" + owner, L"older", true);   // a server's files, left in data/ by a crash
		const std::wstring menu = vault + L"\\" + kMenu, before = vault + L"\\" + kBefore;
		if (IsPlainDir(menu) && MergeInto(menu, data, L"menu", false))
			RemoveDirectoryW(menu.c_str());
		if (IsPlainDir(before) && MergeInto(before, data, L"before-pack", false))
			RemoveDirectoryW(before.c_str());
		DeleteFileW((vault + L"\\active").c_str());
		RemoveDirectoryW(vault.c_str());   // only if nothing is left in it
		return "garrysmod/data: links can't be made on this drive (error " + std::to_string(error) +
			"), so every server shares one garrysmod/data";
	}

	// A plain data/ becomes the link: its files go to the vault folder of whoever
	// they belong to, in one rename, and a link to the menu's folder takes its
	// place. Every step either completes or is undone; nothing is overwritten.
	std::string Convert(const std::wstring& data)
	{
		const std::wstring vault = DefaultVault(data);
		const std::wstring menu = vault + L"\\" + kMenu;
		if (vault.empty() || !EnsureDir(menu))
			return "garrysmod/data: couldn't make the pack's vault folder (error " + std::to_string(GetLastError()) +
				"); every server shares one garrysmod/data this session";
		const std::wstring activeFile = vault + L"\\active";
		const std::wstring owner = LegacyOwner(Widen(ReadSmallFile(activeFile)));

		// 1. A link beside data/ first: if links don't work here, nothing has moved.
		const std::wstring link = data + L".meowy-link";
		if (IsDir(link))
			RemoveDirectoryW(link.c_str());   // left by a start that died here: removes a link (only the link) or an empty folder
		DWORD error = ERROR_SUCCESS;
		if (Exists(link))
			return "garrysmod/data: \"" + Narrow(Leaf(link)) + "\" is in the way; every server shares one garrysmod/data this session";
		if (!MakeLink(link, menu, &error))
			return Unsandboxed(data, vault, owner, error);

		// 2. data/, as it is, into the vault in one rename. A file in it that's
		//    open somewhere stops the rename, and then nothing has changed.
		const std::wstring home = vault + L"\\" + owner;
		if (IsPlainDir(home) && Entries(home).empty())
			RemoveDirectoryW(home.c_str());
		std::wstring staging = home;
		for (int i = 1; Exists(staging); ++i)
			staging = home + L".incoming" + (i > 1 ? L"-" + std::to_wstring(i) : L"");
		if (!MoveFileExW(data.c_str(), staging.c_str(), 0))
		{
			error = GetLastError();
			RemoveDirectoryW(link.c_str());
			return "garrysmod/data couldn't be moved (error " + std::to_string(error) +
				", a file in it is open?); every server shares one garrysmod/data this session";
		}

		// 3. The link takes data/'s place; if it can't, data/ is put back.
		if (!MoveFileExW(link.c_str(), data.c_str(), 0))
		{
			error = GetLastError();
			MoveFileExW(staging.c_str(), data.c_str(), 0);
			RemoveDirectoryW(link.c_str());
			return "garrysmod/data couldn't be linked (error " + std::to_string(error) +
				"); every server shares one garrysmod/data this session";
		}

		// 4. Some of the owner's files were already in the vault (a switch that
		//    stopped half-way): merged, data/'s copy (the newer) keeping the name.
		if (staging != home && MergeInto(staging, home, L"older", true))
			RemoveDirectoryW(staging.c_str());

		file_util::WriteAtomic(activeFile, kLinked);
		return "garrysmod/data is now a link into the pack's vault: every server sees only its own files "
			"(the files that were in it are kept with " + Narrow(owner) + "'s)";
	}
}

namespace data_sandbox
{
	std::string Recover()
	{
		g = State();
		const std::wstring data = DataDir();
		if (data.empty())
			return std::string();   // GMod makes data/ on its first run; it's linked from the next start

		if (IsLink(data))
		{
			const std::wstring target = LinkTarget(data);
			if (target.empty() || !IsVault(Parent(target)) || !IsSafeKey(Leaf(target)))
			{
				Load();   // someone else's link: never touched
				return std::string();
			}
			// Ours: back to the menu's folder (after a crash on a server it still
			// points there), which is made again if it was deleted by hand.
			const std::wstring menu = Parent(target) + L"\\" + kMenu;
			DWORD error = ERROR_SUCCESS;
			if (!EnsureDir(menu) || (!SameText(target, menu) && !SetLink(data, menu, &error)))
			{
				Load();
				return "garrysmod/data: couldn't link it back to the main menu's files (error " +
					std::to_string(error ? error : GetLastError()) + ")";
			}
			Load();
			return std::string();
		}

		std::string said = Convert(data);
		Load();
		return said;
	}

	void Enter(const std::string& address)
	{
		if (address.empty())
			return;
		// Only a real "a.b.c.d:port" names its own folder; anything else gets one
		// fixed folder per kind, never a name made from the address.
		SwitchTo(IsIpPort(address) ? KeyFor(address) : KindKey(address));
	}

	void EnterUnknown()
	{
		SwitchTo(kOther);
	}

	void Leave()
	{
		SwitchTo(kMenu);
	}

	std::string Describe()
	{
		Load();
		if (!g.linked)
			return "off: " + g.off;
		std::string s = "garrysmod/data links to " + Narrow(g.key) + " in " + Narrow(g.vault);
		if (!g.problem.empty())
			s += "; last problem: " + g.problem;
		return s;
	}
}

#else

namespace data_sandbox
{
	std::string Recover() { return std::string(); }
	void Enter(const std::string&) {}
	void EnterUnknown() {}
	void Leave() {}
	std::string Describe() { return "unsupported"; }
}

#endif
