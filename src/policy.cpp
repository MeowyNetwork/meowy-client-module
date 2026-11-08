#include "policy.h"
#include "file_util.h"
#include "notify_catalogue.h"   // VerifyWithKey: ECDSA P-256 / SHA-256 through BCrypt

#include <algorithm>
#include <atomic>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <thread>
#endif

namespace policy
{
	namespace
	{
		// The production public keys (X || Y of each P-256 point), made by
		// tools/policy/new-keys.ps1 on 2026-11-07: the release key is in
		// Catualus's TPM (PIN on use), the ops and recovery keys are files he keeps.
		const uint8_t kRelease[64] = {
			0xd5, 0x50, 0x20, 0x38, 0x03, 0x67, 0x4f, 0x59, 0xea, 0xde, 0x15, 0x47, 0x14, 0xb2, 0xa1, 0x7a,
			0xce, 0xeb, 0x67, 0xc3, 0x9d, 0x6e, 0xd3, 0x5b, 0x48, 0xd1, 0x9f, 0xc2, 0xe2, 0x72, 0x36, 0x18,
			0x7f, 0x12, 0x04, 0x09, 0x5b, 0x94, 0x97, 0xc2, 0x8f, 0x62, 0x32, 0x9e, 0x71, 0xc4, 0xdd, 0x87,
			0x64, 0xb5, 0x30, 0xc0, 0x6d, 0x55, 0x32, 0x0c, 0xde, 0x7a, 0x49, 0xef, 0x61, 0xb7, 0xe7, 0x5f,
		};
		const uint8_t kOps[64] = {
			0x08, 0x28, 0xb9, 0xe2, 0xcd, 0x57, 0xa0, 0x4f, 0xcc, 0x1d, 0x72, 0xf2, 0x45, 0x31, 0x5e, 0x36,
			0x97, 0xfb, 0x8a, 0xac, 0xad, 0x6c, 0x75, 0x97, 0x5c, 0xa5, 0xdf, 0xe7, 0xf1, 0x05, 0xed, 0x22,
			0x2d, 0x41, 0xb0, 0x61, 0xa8, 0x8c, 0x17, 0xa8, 0xe4, 0xc5, 0x47, 0x41, 0x63, 0x86, 0x27, 0xe2,
			0x03, 0x86, 0x79, 0x6c, 0xcf, 0xed, 0x72, 0x34, 0x7b, 0x66, 0xd8, 0xf7, 0x0d, 0x3f, 0x86, 0xc9,
		};
		const uint8_t kRecovery[64] = {
			0x4c, 0x04, 0x8a, 0x00, 0xfd, 0x73, 0x14, 0x5a, 0x0d, 0x89, 0x5e, 0x39, 0x55, 0x93, 0x60, 0x80,
			0x9e, 0x1e, 0x8e, 0x63, 0x9b, 0xb0, 0xd1, 0x1e, 0x61, 0x79, 0x2a, 0x06, 0xf6, 0xc8, 0xff, 0xbf,
			0x93, 0x94, 0x4f, 0x3c, 0xb2, 0xc1, 0x0a, 0xec, 0x84, 0x18, 0x7b, 0xe6, 0x59, 0x3f, 0xd7, 0xa0,
			0x41, 0x29, 0x1e, 0xd0, 0xc9, 0x22, 0x23, 0xe6, 0x63, 0xab, 0xf2, 0x86, 0x46, 0x38, 0x87, 0x81,
		};
		const uint8_t* const kReleaseKey = kRelease;
		const uint8_t* const kOpsKey = kOps;
		const uint8_t* const kRecoveryKey = kRecovery;

		struct Keys
		{
			const uint8_t* release;
			const uint8_t* ops;
			const uint8_t* recovery;
		};
		Keys g_keys = { kReleaseKey, kOpsKey, kRecoveryKey };

		std::mutex g_lock;
		// The kept files (policy.txt/.sig/.bin): read-check-replace (Offer), the
		// stub file's heal and the start's read are one step each, so neither
		// can put an older policy back over a newer one. Taken before g_lock.
		std::mutex g_fileLock;
		bool g_haveCurrent = false;
		Policy g_current;
		Key g_currentKey = Key::None;
		std::string g_lastFetch = "not fetched yet";

		bool Digits(const std::string& s, size_t maxLen)
		{
			if (s.empty() || s.size() > maxLen)
				return false;
			for (char c : s)
				if (c < '0' || c > '9')
					return false;
			return true;
		}

		// 1 to 4 dot-separated numbers of 1 to 5 digits.
		bool VersionText(const std::string& s)
		{
			size_t parts = 0, at = 0;
			while (at <= s.size())
			{
				size_t dot = s.find('.', at);
				if (dot == std::string::npos)
					dot = s.size();
				if (!Digits(s.substr(at, dot - at), 5) || ++parts > 4)
					return false;
				at = dot + 1;
			}
			return parts >= 1;
		}

		bool NameText(const std::string& s)
		{
			if (s.empty() || s.size() > 48)
				return false;
			for (char c : s)
				if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
					return false;
			return true;
		}

		// Valid UTF-8 with no control characters.
		bool PlainUtf8(const std::string& s)
		{
			for (size_t i = 0; i < s.size();)
			{
				const unsigned char c = static_cast<unsigned char>(s[i]);
				if (c < 0x20 || c == 0x7F)
					return false;
				if (c < 0x80)
				{
					++i;
					continue;
				}
				size_t n;
				uint32_t cp;
				if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1F; }
				else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
				else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; }
				else return false;
				if (i + n > s.size())
					return false;
				for (size_t k = 1; k < n; ++k)
				{
					const unsigned char cc = static_cast<unsigned char>(s[i + k]);
					if ((cc & 0xC0) != 0x80)
						return false;
					cp = (cp << 6) | (cc & 0x3F);
				}
				if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
					return false;
				i += n;
			}
			return true;
		}

		bool Has(const std::vector<std::string>& list, const std::string& v)
		{
			return std::find(list.begin(), list.end(), v) != list.end();
		}

		std::string Trim(std::string s)
		{
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
				s.pop_back();
			return s;
		}

		const char* KeyName(Key k)
		{
			return k == Key::Release ? "release key" : k == Key::Ops ? "ops key" : k == Key::Recovery ? "recovery key" : "no key";
		}
	}

	bool Parse(const std::string& body, Policy* out, const char** why)
	{
		const char* dummy;
		if (!why)
			why = &dummy;
		if (body.empty() || body.size() > kMaxBody)
			return *why = "size", false;
		if (body.back() != '\n' || body.find('\r') != std::string::npos || body.find('\0') != std::string::npos)
			return *why = "line ends", false;
		Policy p;
		bool haveSeq = false, haveEnabled = false, haveMin = false, haveNotice = false;
		size_t at = 0, lineNo = 0;
		while (at < body.size())
		{
			const size_t end = body.find('\n', at);
			const std::string line = body.substr(at, end - at);
			at = end + 1;
			if (lineNo++ == 0)
			{
				if (line != "meowy-pack-policy 1")
					return *why = "header", false;
				continue;
			}
			const size_t space = line.find(' ');
			if (space == std::string::npos || space == 0 || space + 1 >= line.size())
				return *why = "line", false;
			const std::string kw = line.substr(0, space), value = line.substr(space + 1);
			if (kw == "seq")
			{
				if (haveSeq || !Digits(value, 19) || value[0] == '0')
					return *why = "seq", false;
				haveSeq = true;
				p.seq = std::stoull(value);
			}
			else if (kw == "pack_enabled")
			{
				if (haveEnabled || (value != "0" && value != "1"))
					return *why = "pack_enabled", false;
				haveEnabled = true;
				p.packEnabled = value == "1";
			}
			else if (kw == "min_version")
			{
				if (haveMin || !VersionText(value))
					return *why = "min_version", false;
				haveMin = true;
				p.minVersion = value;
			}
			else if (kw == "off")
			{
				if (!NameText(value) || Has(p.off, value) || p.off.size() >= kMaxList)
					return *why = "off", false;
				p.off.push_back(value);
			}
			else if (kw == "revoked")
			{
				if (!VersionText(value) || Has(p.revoked, value) || p.revoked.size() >= kMaxList)
					return *why = "revoked", false;
				p.revoked.push_back(value);
			}
			else if (kw == "notice")
			{
				if (haveNotice || value.size() > kMaxNotice || !PlainUtf8(value))
					return *why = "notice", false;
				haveNotice = true;
				p.notice = value;
			}
			else
			{
				// A keyword from a newer policy: skipped, if it looks like one.
				for (char c : kw)
					if (!((c >= 'a' && c <= 'z') || c == '_'))
						return *why = "keyword", false;
			}
		}
		if (!haveSeq || !haveEnabled)
			return *why = haveSeq ? "no pack_enabled" : "no seq", false;
		if (out)
			*out = p;
		return true;
	}

	Key SignedBy(const std::string& body, const std::string& sigHex)
	{
		Keys keys;
		{
			std::lock_guard<std::mutex> lock(g_lock);
			keys = g_keys;
		}
		if (sigHex.size() != kSigHexLen)
			return Key::None;
		if (keys.release && notify_catalogue::VerifyWithKey(body, sigHex, keys.release))
			return Key::Release;
		if (keys.ops && notify_catalogue::VerifyWithKey(body, sigHex, keys.ops))
			return Key::Ops;
		if (keys.recovery && notify_catalogue::VerifyWithKey(body, sigHex, keys.recovery))
			return Key::Recovery;
		return Key::None;
	}

	int CompareVersions(const std::string& a, const std::string& b)
	{
		size_t i = 0, j = 0;
		while (i < a.size() || j < b.size())
		{
			unsigned long x = 0, y = 0;
			while (i < a.size() && a[i] != '.')
				x = x * 10 + static_cast<unsigned long>(a[i++] - '0');
			while (j < b.size() && b[j] != '.')
				y = y * 10 + static_cast<unsigned long>(b[j++] - '0');
			if (x != y)
				return x < y ? -1 : 1;
			if (i < a.size())
				++i;
			if (j < b.size())
				++j;
		}
		return 0;
	}

	bool VersionAllowed(const Policy& p, const std::string& version)
	{
		if (!p.packEnabled)
			return false;
		if (!p.minVersion.empty() && CompareVersions(version, p.minVersion) < 0)
			return false;
		for (const std::string& r : p.revoked)
			if (CompareVersions(version, r) == 0)
				return false;
		return true;
	}

	bool MayReplace(const Policy& cur, bool haveCur, const Policy& next, Key key, const char** why)
	{
		const char* dummy;
		if (!why)
			why = &dummy;
		if (key == Key::None)
			return *why = "not signed by one of our keys", false;
		if (haveCur && next.seq <= cur.seq)
			return *why = "not newer than the one kept", false;
		if (key == Key::Release)
			return true;   // the release key may change anything
		// The ops and recovery keys: a seq the release key can always beat (below
		// kReleaseOnlySeq, at most kMaxStepSeq above the kept one), so a stolen
		// key can't freeze the policy for good; and no notice of their own.
		if (next.seq >= kReleaseOnlySeq || next.seq - (haveCur ? cur.seq : 0) > kMaxStepSeq)
			return *why = "the ops key can't raise the seq that far", false;
		if (!next.notice.empty() && (!haveCur || next.notice != cur.notice))
			return *why = "the ops key can't set a notice", false;
		if (!haveCur)
			return true;   // anything else is as strict as "all on"
		// Only stricter.
		if (next.packEnabled && !cur.packEnabled)
			return *why = "the ops key can't switch the pack back on", false;
		for (const std::string& o : cur.off)
			if (!Has(next.off, o))
				return *why = "the ops key can't switch a feature back on", false;
		for (const std::string& r : cur.revoked)
			if (!Has(next.revoked, r))
				return *why = "the ops key can't un-revoke a version", false;
		if (!cur.minVersion.empty() && (next.minVersion.empty() || CompareVersions(next.minVersion, cur.minVersion) < 0))
			return *why = "the ops key can't lower min_version", false;
		return true;
	}

	namespace
	{
		std::wstring PathIn(const std::wstring& dir, const wchar_t* name) { return dir + L"\\" + name; }

		std::string StubText(const Policy& p)
		{
			std::string text = std::string("enabled ") + (p.packEnabled ? "1" : "0") + "\nseq " + std::to_string(p.seq) + "\n";
			// The stub also passes over an update this policy doesn't allow (pack_versions.h).
			if (!p.minVersion.empty())
				text += "min_version " + p.minVersion + "\n";
			for (const std::string& r : p.revoked)
				text += "revoked " + r + "\n";
			if (!p.notice.empty())
				text += "notice " + p.notice + "\n";
			return text;
		}

		void WriteStubFile(const std::wstring& stateDir, const Policy& p)
		{
			file_util::WriteAtomic(PathIn(stateDir, L"policy.bin"), StubText(p));
		}

		bool ReadKept(const std::wstring& stateDir, Policy* out, Key* key)
		{
			const std::string body = file_util::ReadSmall(PathIn(stateDir, L"policy.txt"), kMaxBody + 1);
			const std::string sig = Trim(file_util::ReadSmall(PathIn(stateDir, L"policy.sig"), 256));
			if (body.empty() || body.size() > kMaxBody)
				return false;
			const Key k = SignedBy(body, sig);
			if (k == Key::None || !Parse(body, out, nullptr))
				return false;
			if (key)
				*key = k;
			return true;
		}
	}

	bool Current(const std::wstring& stateDir, Policy* out)
	{
		return ReadKept(stateDir, out, nullptr);
	}

	bool Offer(const std::wstring& stateDir, const std::string& body, const std::string& sigHex, std::string& why)
	{
		const std::string sig = Trim(sigHex);
		const Key key = SignedBy(body, sig);
		if (key == Key::None)
			return why = "not signed by one of our keys", false;
		Policy next;
		const char* reason = nullptr;
		if (!Parse(body, &next, &reason))
			return why = std::string("doesn't parse (") + reason + ")", false;
		std::lock_guard<std::mutex> files(g_fileLock);
		Policy cur;
		const bool haveCur = ReadKept(stateDir, &cur, nullptr);
		if (!MayReplace(cur, haveCur, next, key, &reason))
			return why = reason, false;
#ifdef _WIN32
		CreateDirectoryW(stateDir.c_str(), nullptr);
#endif
		if (!file_util::WriteAtomic(PathIn(stateDir, L"policy.txt"), body) || !file_util::WriteAtomic(PathIn(stateDir, L"policy.sig"), sig + "\n"))
			return why = "couldn't be kept", false;
		WriteStubFile(stateDir, next);
		{
			std::lock_guard<std::mutex> lock(g_lock);
			g_current = next;
			g_haveCurrent = true;
			g_currentKey = key;
		}
		why = "policy " + std::to_string(next.seq) + " taken (" + KeyName(key) + ")";
		return true;
	}

	bool HaveKeys()
	{
		std::lock_guard<std::mutex> lock(g_lock);
		return g_keys.release != nullptr;
	}

	Policy Effective()
	{
		std::lock_guard<std::mutex> lock(g_lock);
		return g_haveCurrent ? g_current : Policy();
	}

	std::string Describe()
	{
		std::lock_guard<std::mutex> lock(g_lock);
		if (!g_keys.release)
			return "no policy keys in this build yet: everything on";
		std::string s = g_haveCurrent
			? "policy " + std::to_string(g_current.seq) + " (" + KeyName(g_currentKey) + "): pack " + (g_current.packEnabled ? "on" : "OFF")
			: std::string("no policy kept: everything on");
		if (g_haveCurrent)
		{
			if (!g_current.off.empty())
			{
				s += "; off:";
				for (const std::string& o : g_current.off)
					s += " " + o;
			}
			if (!g_current.minVersion.empty())
				s += "; min_version " + g_current.minVersion;
			if (!g_current.revoked.empty())
				s += "; " + std::to_string(g_current.revoked.size()) + " versions revoked";
		}
		return s + "; last check: " + g_lastFetch;
	}

	namespace detail
	{
		void SetKeys(const uint8_t* release, const uint8_t* ops, const uint8_t* recovery)
		{
			std::lock_guard<std::mutex> lock(g_lock);
			g_keys = { release, ops, recovery };
		}
	}

#ifdef _WIN32
	namespace
	{
		std::atomic<bool> g_fetching{ false };

		// GET https://meowynetwork.com<path>: the body for a 200 (at most kMaxBody), else "".
		std::string Get(const wchar_t* path)
		{
			std::string out;
			HINTERNET session = WinHttpOpen(L"MeowyClientPack", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			if (!session)
				return out;
			WinHttpSetTimeouts(session, 5000, 5000, 8000, 8000);
			HINTERNET connect = WinHttpConnect(session, L"meowynetwork.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
			HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
				WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
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
					DWORD avail = 0;
					while (status == 200 && WinHttpQueryDataAvailable(request, &avail) && avail > 0 && out.size() <= kMaxBody)
					{
						std::string chunk(avail, '\0');
						DWORD read = 0;
						if (!WinHttpReadData(request, &chunk[0], avail, &read) || read == 0)
							break;
						out.append(chunk.data(), read);
					}
					if (status != 200 || out.size() > kMaxBody)
						out.clear();
				}
				WinHttpCloseHandle(request);
			}
			if (connect)
				WinHttpCloseHandle(connect);
			WinHttpCloseHandle(session);
			return out;
		}

		std::string Now()
		{
			SYSTEMTIME t;
			GetLocalTime(&t);
			char s[16];
			std::snprintf(s, sizeof s, "%02u:%02u", t.wHour, t.wMinute);
			return s;
		}

		void FetchOnce(const std::wstring& stateDir)
		{
			const std::string body = Get(L"/pack/v1/policy.txt");
			const std::string sig = body.empty() ? std::string() : Get(L"/pack/v1/policy.sig");
			std::string result;
			if (body.empty() || sig.empty())
				result = "couldn't fetch";
			else if (body.compare(0, 20, "meowy-pack-policy 1\n") != 0)
				result = "none published yet";   // the site answers unknown paths with its own page
			else
				Offer(stateDir, body, sig, result);
			std::lock_guard<std::mutex> lock(g_lock);
			g_lastFetch = result + " at " + Now();
		}
	}

	bool HealStubFile(const std::wstring& stateDir)
	{
		std::lock_guard<std::mutex> files(g_fileLock);
		Policy kept;
		Key key = Key::None;
		if (!ReadKept(stateDir, &kept, &key))
			return false;
		const std::string want = StubText(kept);
		if (file_util::ReadSmall(PathIn(stateDir, L"policy.bin"), kMaxBody + 1024) == want)
			return false;
		return file_util::WriteAtomic(PathIn(stateDir, L"policy.bin"), want);
	}

	void StartFetching(const std::wstring& stateDir)
	{
		{
			// The fetch thread a first call started may be taking a newer one.
			std::lock_guard<std::mutex> files(g_fileLock);
			Policy kept;
			Key key = Key::None;
			if (ReadKept(stateDir, &kept, &key))
			{
				std::lock_guard<std::mutex> lock(g_lock);
				g_current = kept;
				g_haveCurrent = true;
				g_currentKey = key;
			}
		}
		HealStubFile(stateDir);
		if (!HaveKeys() || g_fetching.exchange(true))
			return;
		std::thread([stateDir]
		{
			for (;;)
			{
				FetchOnce(stateDir);
				Sleep(30 * 60 * 1000);
			}
		}).detach();
	}
#else
	bool HealStubFile(const std::wstring&) { return false; }
	void StartFetching(const std::wstring&) {}
#endif
}
