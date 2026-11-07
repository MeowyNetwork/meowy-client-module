#include "updater.h"
#include "pack_versions.h"
#include "policy.h"
#include "file_util.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <ctime>
#include <thread>
#endif

namespace updater
{
	namespace
	{
		bool Digits(const std::string& s, size_t maxLen)
		{
			if (s.empty() || s.size() > maxLen)
				return false;
			for (char c : s)
				if (c < '0' || c > '9')
					return false;
			return true;
		}

		bool Hex(const std::string& s, size_t minLen, size_t maxLen)
		{
			if (s.size() < minLen || s.size() > maxLen)
				return false;
			for (char c : s)
				if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
					return false;
			return true;
		}

		// /pack/v1/ then letters, digits, '.', '_', '-', '/', no "..", no "//".
		bool SitePath(const std::string& p)
		{
			if (p.size() > 200 || p.compare(0, 9, "/pack/v1/") != 0 || p.find("..") != std::string::npos || p.find("//") != std::string::npos)
				return false;
			for (char c : p)
				if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/'))
					return false;
			return p.back() != '/';
		}

		std::vector<std::string> Split(const std::string& s)
		{
			std::vector<std::string> out;
			size_t at = 0;
			while (at <= s.size())
			{
				size_t sp = s.find(' ', at);
				if (sp == std::string::npos)
					sp = s.size();
				out.push_back(s.substr(at, sp - at));
				at = sp + 1;
			}
			return out;
		}
	}

	bool Parse(const std::string& body, Release* out, const char** why)
	{
		const char* dummy;
		if (!why)
			why = &dummy;
		if (body.empty() || body.size() > kMaxBody)
			return *why = "size", false;
		if (body.back() != '\n' || body.find('\r') != std::string::npos || body.find('\0') != std::string::npos)
			return *why = "line ends", false;
		Release r;
		bool seq = false, version = false, expires = false, stub = false, rollout = false, salt = false, notes = false;
		size_t at = 0, n = 0;
		while (at < body.size())
		{
			const size_t end = body.find('\n', at);
			const std::string line = body.substr(at, end - at);
			at = end + 1;
			if (n++ == 0)
			{
				if (line != "meowy-pack-release 1")
					return *why = "header", false;
				continue;
			}
			const size_t space = line.find(' ');
			if (space == std::string::npos || space == 0 || space + 1 >= line.size())
				return *why = "line", false;
			const std::string kw = line.substr(0, space), v = line.substr(space + 1);
			if (kw == "seq")
			{
				if (seq || !Digits(v, 19) || v[0] == '0')
					return *why = "seq", false;
				seq = true;
				r.seq = std::stoull(v);
			}
			else if (kw == "version")
			{
				if (version || !pack_versions::ValidVersion(v))
					return *why = "version", false;
				version = true;
				r.version = v;
			}
			else if (kw == "expires")
			{
				if (expires || !Digits(v, 12))
					return *why = "expires", false;
				expires = true;
				r.expires = std::stoull(v);
			}
			else if (kw == "min_stub")
			{
				if (stub || !Digits(v, 2))
					return *why = "min_stub", false;
				stub = true;
				r.minStub = std::stoi(v);
			}
			else if (kw == "rollout")
			{
				if (rollout || !Digits(v, 3) || std::stoi(v) > 100)
					return *why = "rollout", false;
				rollout = true;
				r.rollout = std::stoi(v);
			}
			else if (kw == "salt")
			{
				if (salt || !Hex(v, 8, 64))
					return *why = "salt", false;
				salt = true;
				r.salt = v;
			}
			else if (kw == "file")
			{
				const std::vector<std::string> f = Split(v);
				if (f.size() != 4 || (f[0] != "win64" && f[0] != "win32") || !Digits(f[1], 9) || !pack_versions::ValidSha256(f[2]) || !SitePath(f[3]))
					return *why = "file", false;
				for (const File& e : r.files)
					if (e.arch == f[0])
						return *why = "file twice", false;
				File e;
				e.arch = f[0];
				e.size = std::stoull(f[1]);
				e.sha256 = f[2];
				e.path = f[3];
				if (e.size == 0 || e.size > pack_versions::kMaxDll)
					return *why = "file size", false;
				r.files.push_back(e);
			}
			else if (kw == "notes")
			{
				if (notes || !SitePath(v))
					return *why = "notes", false;
				notes = true;
				r.notes = v;
			}
			else
			{
				for (char c : kw)
					if (!((c >= 'a' && c <= 'z') || c == '_'))
						return *why = "keyword", false;
			}
		}
		if (!seq || !version || !expires || !stub || !rollout || !salt || r.files.empty())
			return *why = "missing a field", false;
		if (out)
			*out = r;
		return true;
	}

	int RolloutSlot(const std::string& token, const std::string& salt)
	{
#ifdef _WIN32
		const std::string data = token + salt;
		unsigned char digest[32] = {};
		BCRYPT_ALG_HANDLE alg = nullptr;
		if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0)
		{
			BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), digest, sizeof digest);
			BCryptCloseAlgorithmProvider(alg, 0);
		}
		const uint32_t v = (static_cast<uint32_t>(digest[0]) << 24) | (static_cast<uint32_t>(digest[1]) << 16) | (static_cast<uint32_t>(digest[2]) << 8) | digest[3];
		return static_cast<int>(v % 100);
#else
		(void)token;
		(void)salt;
		return 0;
#endif
	}

	const File* OwnFile(const Release& r)
	{
		const std::string arch = sizeof(void*) == 8 ? "win64" : "win32";
		for (const File& f : r.files)
			if (f.arch == arch)
				return &f;
		return nullptr;
	}

	bool ShouldTake(const Release& r, const Facts& f, std::string& why)
	{
		if (r.seq <= f.lastSeq)
			return why = "the release document isn't newer than the last one taken", false;
		if (f.now >= r.expires)
			return why = "the release document has expired", false;
		if (pack_versions::Compare(r.version, f.running) <= 0)
			return why = "up to date (" + f.running + ")", false;
		if (std::find(f.bad.begin(), f.bad.end(), r.version) != f.bad.end())
			return why = r.version + " failed to start here before", false;
		if (!f.policyAllows)
			return why = r.version + " isn't allowed by Meowy Network's policy", false;
		if (f.stub < 1 || f.stub < r.minStub)
			return why = r.version + " needs a newer start-up stub: reinstall the pack", false;
		if (RolloutSlot(f.token, r.salt) >= r.rollout)
			return why = r.version + " is rolling out (" + std::to_string(r.rollout) + "%); not here yet", false;
		if (!OwnFile(r))
			return why = r.version + " has no file for this game build", false;
		why = "update to " + r.version;
		return true;
	}

	namespace
	{
		std::mutex g_lock;
		std::string g_state = "not started";
		std::string g_ready;
		bool g_readyShown = false;
		std::atomic<bool> g_started{ false };

		void SetState(const std::string& s)
		{
			std::lock_guard<std::mutex> lock(g_lock);
			g_state = s;
		}
	}

	std::string Describe()
	{
		std::lock_guard<std::mutex> lock(g_lock);
		return g_state;
	}

	std::string TakeReadyNotice()
	{
		std::lock_guard<std::mutex> lock(g_lock);
		if (g_ready.empty() || g_readyShown)
			return std::string();
		g_readyShown = true;
		return g_ready;
	}

#ifdef _WIN32
	namespace
	{
		std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }   // ASCII paths and versions

		// GET https://meowynetwork.com<path>: the body for a 200 of at most `cap` bytes, else "".
		std::string Get(const std::string& path, size_t cap)
		{
			std::string out;
			HINTERNET session = WinHttpOpen(L"MeowyClientPack", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			if (!session)
				return out;
			WinHttpSetTimeouts(session, 5000, 5000, 15000, 30000);
			HINTERNET connect = WinHttpConnect(session, L"meowynetwork.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
			HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", Widen(path).c_str(), nullptr, WINHTTP_NO_REFERER,
				WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
			bool ok = false;
			if (request)
			{
				DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
				WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof(never));
				if (WinHttpSendRequest(request, L"Cache-Control: no-cache\r\n", static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
					&& WinHttpReceiveResponse(request, nullptr))
				{
					DWORD status = 0, size = sizeof(status);
					WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
						&status, &size, WINHTTP_NO_HEADER_INDEX);
					ok = status == 200;
					DWORD avail = 0;
					while (ok && WinHttpQueryDataAvailable(request, &avail) && avail > 0)
					{
						if (out.size() + avail > cap)
						{
							ok = false;
							break;
						}
						const size_t was = out.size();
						out.resize(was + avail);
						DWORD read = 0;
						if (!WinHttpReadData(request, &out[was], avail, &read))
							ok = false;
						out.resize(was + read);
						if (read == 0)
							break;
					}
				}
				WinHttpCloseHandle(request);
			}
			if (connect)
				WinHttpCloseHandle(connect);
			WinHttpCloseHandle(session);
			return ok ? out : std::string();
		}

		std::string Trim(std::string s)
		{
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
				s.pop_back();
			return s;
		}

		std::string Token(const std::wstring& dataDir)
		{
			const std::wstring path = dataDir + L"\\state\\install_token";
			std::string t = Trim(file_util::ReadSmall(path, 128));
			if (Hex(t, 32, 32))
				return t;
			unsigned char raw[16];
			if (BCryptGenRandom(nullptr, raw, sizeof raw, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
				return std::string();
			static const char* hex = "0123456789abcdef";
			t.clear();
			for (unsigned char b : raw)
			{
				t += hex[b >> 4];
				t += hex[b & 15];
			}
			CreateDirectoryW((dataDir + L"\\state").c_str(), nullptr);
			file_util::WriteAtomic(path, t + "\n");
			return t;
		}

		std::vector<std::string> BadList(const std::wstring& dataDir)
		{
			std::vector<std::string> out;
			const std::string text = file_util::ReadSmall(pack_versions::StatePath(dataDir, L"bad"), 4096);
			size_t at = 0;
			while (at < text.size())
			{
				size_t end = text.find('\n', at);
				if (end == std::string::npos)
					end = text.size();
				const std::string v = Trim(text.substr(at, end - at));
				if (!v.empty())
					out.push_back(v);
				at = end + 1;
			}
			return out;
		}

		void RemoveFolder(const std::wstring& dir)
		{
			WIN32_FIND_DATAW f;
			HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &f);
			if (h != INVALID_HANDLE_VALUE)
			{
				do
				{
					if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
						DeleteFileW((dir + L"\\" + f.cFileName).c_str());
				} while (FindNextFileW(h, &f));
				FindClose(h);
			}
			RemoveDirectoryW(dir.c_str());
		}

		// bin\<version> folders nothing points to any more (current, previous, pending kept).
		void CleanUp(const std::wstring& dataDir)
		{
			std::vector<std::string> keep;
			for (const wchar_t* which : { L"current", L"previous", L"pending" })
			{
				pack_versions::Pointer p;
				if (pack_versions::ReadPointer(pack_versions::StatePath(dataDir, which), &p))
					keep.push_back(p.version);
			}
			WIN32_FIND_DATAW f;
			HANDLE h = FindFirstFileW((dataDir + L"\\bin\\*").c_str(), &f);
			if (h == INVALID_HANDLE_VALUE)
				return;
			do
			{
				if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || f.cFileName[0] == L'.')
					continue;
				std::string name;
				for (const wchar_t* c = f.cFileName; *c; ++c)
					name += *c < 128 ? static_cast<char>(*c) : '?';
				const bool partial = name.size() > 8 && name.compare(name.size() - 8, 8, ".partial") == 0;
				if (partial || std::find(keep.begin(), keep.end(), name) == keep.end())
					RemoveFolder(dataDir + L"\\bin\\" + f.cFileName);
			} while (FindNextFileW(h, &f));
			FindClose(h);
		}

		// Downloads, checks and stages `r` for the next start. False and why on any failure.
		bool Stage(const std::wstring& dataDir, const Release& r, std::string& why)
		{
			const File* file = OwnFile(r);
			const std::string bytes = Get(file->path, static_cast<size_t>(file->size));
			if (bytes.size() != file->size)
				return why = "couldn't download " + r.version, false;
			const std::wstring bin = dataDir + L"\\bin", partial = bin + L"\\" + Widen(r.version) + L".partial",
				final = bin + L"\\" + Widen(r.version);
			CreateDirectoryW(bin.c_str(), nullptr);
			RemoveFolder(partial);
			CreateDirectoryW(partial.c_str(), nullptr);
			const std::wstring dll = partial + L"\\" + pack_versions::DllName();
			if (!file_util::WriteAtomic(dll, bytes) || pack_versions::Sha256File(dll) != file->sha256)
			{
				RemoveFolder(partial);
				return why = "the download of " + r.version + " didn't match its signed hash", false;
			}
			RemoveFolder(final);
			if (!MoveFileExW(partial.c_str(), final.c_str(), 0))
			{
				RemoveFolder(partial);
				return why = "couldn't put " + r.version + " in place", false;
			}
			pack_versions::Pointer p;
			p.version = r.version;
			p.sha256 = file->sha256;
			if (!pack_versions::WritePointer(pack_versions::StatePath(dataDir, L"pending"), p))
				return why = "couldn't record " + r.version, false;
			file_util::WriteAtomic(dataDir + L"\\state\\release_seq.txt", std::to_string(r.seq) + "\n");
			CleanUp(dataDir);
			why = r.version + " is ready: it starts next time you open Garry's Mod";
			return true;
		}

		void CheckOnce(const std::wstring& dataDir, const std::string& running, int stub)
		{
			const std::string body = Get("/pack/v1/release-stable.txt", kMaxBody);
			const std::string sig = body.empty() ? std::string() : Trim(Get("/pack/v1/release-stable.sig", 256));
			if (body.empty() || sig.empty())
				return SetState("no release published yet (or the site couldn't be reached)");
			if (body.compare(0, 21, "meowy-pack-release 1\n") != 0)
				return SetState("no release published yet");   // the site answers unknown paths with its own page
			if (policy::SignedBy(body, sig) != policy::Key::Release)
				return SetState("the release document isn't signed by the release key");
			Release r;
			const char* reason = nullptr;
			if (!Parse(body, &r, &reason))
				return SetState(std::string("the release document doesn't parse (") + reason + ")");
			Facts f;
			f.running = running;
			f.stub = stub;
			const std::string seqText = Trim(file_util::ReadSmall(dataDir + L"\\state\\release_seq.txt", 64));
			f.lastSeq = Digits(seqText, 19) ? std::stoull(seqText) : 0;
			f.now = static_cast<uint64_t>(std::time(nullptr));
			f.token = Token(dataDir);
			f.bad = BadList(dataDir);
			f.policyAllows = policy::VersionAllowed(policy::Effective(), r.version);
			std::string why;
			if (f.token.empty() || !ShouldTake(r, f, why))
				return SetState(f.token.empty() ? std::string("no install token") : why);
			pack_versions::Pointer pending;
			if (pack_versions::ReadPointer(pack_versions::StatePath(dataDir, L"pending"), &pending) && pending.version == r.version)
				return SetState(r.version + " is ready: it starts next time you open Garry's Mod");
			SetState("downloading " + r.version);
			const bool staged = Stage(dataDir, r, why);
			SetState(why);
			if (staged)
			{
				std::lock_guard<std::mutex> lock(g_lock);
				g_ready = r.version;
			}
		}
	}

	void Start(const std::wstring& dataDir, const std::string& running, int stubVersion)
	{
		if (g_started.exchange(true))
			return;
		if (!policy::HaveKeys())
			return SetState("no release key in this build");
		if (stubVersion < 1)
			return SetState("the installed start-up stub can't load updates: reinstall the pack once");
		SetState("checking in a minute");
		std::thread([dataDir, running, stubVersion]
		{
			Sleep(60 * 1000);   // not while the game is starting
			for (;;)
			{
				CheckOnce(dataDir, running, stubVersion);
				Sleep(6 * 60 * 60 * 1000);
			}
		}).detach();
	}
#else
	void Start(const std::wstring&, const std::string&, int) {}
#endif
}
