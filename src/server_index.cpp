#include "server_index.h"
#include "net_http.h"
#include "thread_util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>

namespace
{
	constexpr size_t kMaxBody = 4 * 1024 * 1024;   // unpacked; ~0.7 MB in 2026
	constexpr size_t kMaxPings = 256 * 1024;

	std::mutex g_mutex;
	server_index::Result g_result;   // waiting for Take
	std::string g_etag;              // of the last whole "ok" body, for If-None-Match
	std::string g_last = "never";    // for Describe
	std::atomic<bool> g_fetching{ false };

	// Where the index is (tests point it at a local stub). Read on the worker,
	// set before any fetch.
	std::wstring g_host = L"meowynetwork.com";
	INTERNET_PORT g_port = INTERNET_DEFAULT_HTTPS_PORT;
	bool g_secure = true;
	const wchar_t* const kPath = L"/api/pack/index";

	server_index::Result Get(const std::string& etag, std::string& etagOut)
	{
		server_index::Result r;
		r.ready = true;
		net_http::Request req;
		req.host = g_host;
		req.port = g_port;
		req.secure = g_secure;
		req.path = kPath;
		req.headers = L"Accept: application/json\r\n";
		req.etag = etag;
		req.maxBody = kMaxBody;
		req.readHeader = L"X-Meowy-Geo";
		// The index must be quick or it's no use: Steam's own list is the fallback.
		req.t.resolve = 3000;
		req.t.connect = 3000;
		req.t.send = 4000;
		req.t.receive = 6000;
		req.t.totalMs = 20000;
		net_http::Response resp = net_http::Session::Shared().Send(req);
		if (resp.status == 0)
			r.status = "error: " + resp.error;
		else
		{
			r.geo = resp.header;
			if (r.geo.size() > 32)
				r.geo.clear();
			if (resp.status == 304 && !etag.empty())
			{
				r.status = "same";
				etagOut = etag;
			}
			else if (resp.status != 200)
				r.status = "error: HTTP " + std::to_string(resp.status);
			else if (!resp.complete)
				// Only a body that arrived whole is "ok", and only then is its ETag
				// kept: a cut-off one would make the next fetch's If-None-Match say
				// "same" to a body Lua couldn't read.
				r.status = resp.error == "too big" ? "error: over 4 MB" : "error: " + resp.error;
			else if (resp.body.empty() || resp.body[0] != '{')
				r.status = "error: not JSON";
			else
			{
				r.status = "ok";
				r.body = std::move(resp.body);
				etagOut = resp.etag;
				if (etagOut.size() > 64)
					etagOut.clear();
			}
		}
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
		const bool started = thread_util::Spawn(L"Meowy index", [etag] {
			// The flag is given back however the body ends (an exception too).
			thread_util::OnExit done([] { g_fetching = false; });
			const ULONGLONG start = GetTickCount64();
			std::string etagOut;
			Result r = Get(etag, etagOut);
			r.seconds = (GetTickCount64() - start) / 1000.0;
			std::lock_guard<std::mutex> lock(g_mutex);
			if (r.status == "ok" || r.status == "same")
				g_etag = etagOut;
			char line[160];
			snprintf(line, sizeof(line), "%s in %.2f s, %zu bytes%s", r.status.c_str(), r.seconds, r.body.size(), r.geo.empty() ? "" : ", located");
			g_last = line;
			g_result = std::move(r);
		}, thread_util::Priority::BelowNormal);
		if (!started)
		{
			g_fetching = false;
			std::lock_guard<std::mutex> lock(g_mutex);
			g_last = "couldn't start a thread for it";
		}
		return started;
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

	namespace detail
	{
		void SetEndpoint(const std::wstring& host, unsigned short port, bool secure)
		{
			g_host = host;
			g_port = port;
			g_secure = secure;
		}
	}
}
