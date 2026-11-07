#include "server_index.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

namespace
{
	const wchar_t* const kHost = L"meowynetwork.com";
	const wchar_t* const kPath = L"/api/pack/index";
	constexpr size_t kMaxBody = 4 * 1024 * 1024;   // unpacked; ~0.7 MB in 2026
	constexpr size_t kMaxPings = 256 * 1024;

	std::mutex g_mutex;
	server_index::Result g_result;   // waiting for Take
	std::string g_etag;              // of the last "ok" body, for If-None-Match
	std::string g_last = "never";    // for Describe
	std::atomic<bool> g_fetching{ false };

#ifndef WINHTTP_OPTION_DECOMPRESSION
#define WINHTTP_OPTION_DECOMPRESSION 118
#define WINHTTP_DECOMPRESSION_FLAG_GZIP 0x00000001
#endif

	std::string Narrow(const std::wstring& w)
	{
		std::string out;
		for (wchar_t c : w)
			if (c >= 0x20 && c < 0x7F)
				out += static_cast<char>(c);
		return out;
	}

	std::string Header(HINTERNET request, DWORD info, const wchar_t* name)
	{
		wchar_t buf[256];
		DWORD size = sizeof(buf);
		if (!WinHttpQueryHeaders(request, info, name, buf, &size, WINHTTP_NO_HEADER_INDEX))
			return "";
		return Narrow(std::wstring(buf, size / sizeof(wchar_t)));
	}

	server_index::Result Get(const std::string& etag, std::string& etagOut)
	{
		server_index::Result r;
		r.ready = true;
		const ULONGLONG started = GetTickCount64();
		HINTERNET session = WinHttpOpen(L"MeowyClientPack", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session)
		{
			r.status = "error: no WinHTTP session";
			return r;
		}
		// The index must be quick or it's no use: Steam's own list is the fallback.
		WinHttpSetTimeouts(session, 3000, 3000, 4000, 6000);
		DWORD gzip = WINHTTP_DECOMPRESSION_FLAG_GZIP;
		const bool unpacks = WinHttpSetOption(session, WINHTTP_OPTION_DECOMPRESSION, &gzip, sizeof(gzip)) != FALSE;
		HINTERNET connect = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
		HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", kPath, nullptr, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
		if (!request)
			r.status = "error: no request";
		else
		{
			DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
			WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
			std::wstring headers = L"Accept: application/json\r\n";
			if (!etag.empty())
				headers += L"If-None-Match: " + std::wstring(etag.begin(), etag.end()) + L"\r\n";
			if (!WinHttpSendRequest(request, headers.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
				|| !WinHttpReceiveResponse(request, nullptr))
				r.status = "error: no answer (" + std::to_string(GetLastError()) + ")";
			else
			{
				DWORD status = 0, size = sizeof(status);
				WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
				r.geo = Header(request, WINHTTP_QUERY_CUSTOM, L"X-Meowy-Geo");
				if (r.geo.size() > 32)
					r.geo.clear();
				if (status == 304 && !etag.empty())
				{
					r.status = "same";
					etagOut = etag;
				}
				else if (status != 200)
					r.status = "error: HTTP " + std::to_string(status);
				else if (!unpacks && Header(request, WINHTTP_QUERY_CONTENT_ENCODING, WINHTTP_HEADER_NAME_BY_INDEX) == "gzip")
					r.status = "error: gzip body and no WinHTTP decompression";
				else
				{
					DWORD avail = 0;
					bool tooBig = false;
					while (WinHttpQueryDataAvailable(request, &avail) && avail > 0)
					{
						if (r.body.size() + avail > kMaxBody)
						{
							tooBig = true;
							break;
						}
						std::string chunk(avail, '\0');
						DWORD read = 0;
						if (!WinHttpReadData(request, chunk.data(), avail, &read) || read == 0)
							break;
						r.body.append(chunk.data(), read);
					}
					if (tooBig)
					{
						r.body.clear();
						r.status = "error: over 4 MB";
					}
					else if (r.body.empty() || r.body[0] != '{')
					{
						r.body.clear();
						r.status = "error: not JSON";
					}
					else
					{
						r.status = "ok";
						etagOut = Header(request, WINHTTP_QUERY_ETAG, WINHTTP_HEADER_NAME_BY_INDEX);
						if (etagOut.size() > 64)
							etagOut.clear();
					}
				}
			}
			WinHttpCloseHandle(request);
		}
		if (connect)
			WinHttpCloseHandle(connect);
		WinHttpCloseHandle(session);
		r.seconds = (GetTickCount64() - started) / 1000.0;
		return r;
	}

	std::wstring PingsPath(const std::wstring& dataDir)
	{
		return dataDir.empty() ? L"" : dataDir + L"\\pings.json";
	}
}

namespace server_index
{
	bool Fetch()
	{
		bool expected = false;
		if (!g_fetching.compare_exchange_strong(expected, true))
			return false;
		std::string etag;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			etag = g_etag;
		}
		std::thread([etag] {
			std::string etagOut;
			Result r = Get(etag, etagOut);
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				if (r.status == "ok" || r.status == "same")
					g_etag = etagOut;
				char line[160];
				snprintf(line, sizeof(line), "%s in %.2f s, %zu bytes%s", r.status.c_str(), r.seconds, r.body.size(), r.geo.empty() ? "" : ", located");
				g_last = line;
				g_result = std::move(r);
			}
			g_fetching = false;
		}).detach();
		return true;
	}

	Result Take()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		Result out;
		if (!g_result.ready)
			return out;
		out = std::move(g_result);
		g_result = Result();
		return out;
	}

	std::string Describe()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return std::string("server index: last fetch ") + g_last + (g_fetching ? " (fetching)" : "");
	}

	std::string LoadPings(const std::wstring& dataDir)
	{
		const std::wstring path = PingsPath(dataDir);
		if (path.empty())
			return "";
		FILE* f = _wfopen(path.c_str(), L"rb");
		if (!f)
			return "";
		std::string out(kMaxPings + 1, '\0');
		size_t n = fread(out.data(), 1, out.size(), f);
		fclose(f);
		if (n == 0 || n > kMaxPings || out[0] != '{')
			return "";
		out.resize(n);
		return out;
	}

	bool SavePings(const std::wstring& dataDir, const std::string& json)
	{
		const std::wstring path = PingsPath(dataDir);
		if (path.empty() || json.empty() || json.size() > kMaxPings || json[0] != '{')
			return false;
		// Written whole to a temporary file, then moved over the old one.
		const std::wstring tmp = path + L".tmp";
		FILE* f = _wfopen(tmp.c_str(), L"wb");
		if (!f)
			return false;
		const bool ok = fwrite(json.data(), 1, json.size(), f) == json.size();
		fclose(f);
		if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
		{
			DeleteFileW(tmp.c_str());
			return false;
		}
		return true;
	}
}
