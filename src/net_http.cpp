#include "net_http.h"

#ifdef _WIN32

#include <atomic>
#include <cwchar>
#include <string>

// Older SDKs: the values are Windows' own (winhttp.h).
#ifndef WINHTTP_OPTION_DECOMPRESSION
#define WINHTTP_OPTION_DECOMPRESSION 118
#endif
#ifndef WINHTTP_DECOMPRESSION_FLAG_GZIP
#define WINHTTP_DECOMPRESSION_FLAG_GZIP 0x00000001
#endif
#ifndef WINHTTP_DECOMPRESSION_FLAG_DEFLATE
#define WINHTTP_DECOMPRESSION_FLAG_DEFLATE 0x00000002
#endif
#ifndef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
#define WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL 133
#endif
#ifndef WINHTTP_PROTOCOL_FLAG_HTTP2
#define WINHTTP_PROTOCOL_FLAG_HTTP2 0x1
#endif

namespace
{
	std::atomic<bool> g_joining{ false };

	// Closes a request handle however Send leaves (an exception included).
	struct RequestHandle
	{
		HINTERNET h = nullptr;
		~RequestHandle()
		{
			if (h)
				WinHttpCloseHandle(h);
		}
	};

	std::string Narrow(const wchar_t* w, size_t n)
	{
		std::string out;
		for (size_t i = 0; i < n && w[i]; ++i)
			if (w[i] >= 0x20 && w[i] < 0x7F)
				out += static_cast<char>(w[i]);
		return out;
	}

	std::wstring Widen(const std::string& s)
	{
		std::wstring out;
		for (char c : s)
			if (c >= 0x20 && c < 0x7F)
				out += static_cast<wchar_t>(c);
		return out;
	}

	// A response header (printable ASCII, at most ~250 characters), or "".
	std::string Header(HINTERNET request, DWORD info, const wchar_t* name)
	{
		wchar_t buf[256];
		DWORD size = sizeof(buf);
		if (!WinHttpQueryHeaders(request, info, name, buf, &size, WINHTTP_NO_HEADER_INDEX))
			return "";
		return Narrow(buf, size / sizeof(wchar_t));
	}

	std::string LastError()
	{
		return std::to_string(GetLastError());
	}
}

namespace net_http
{
	Session& Session::Shared()
	{
		// Never deleted: the pinned menu realm's handles live as long as the game,
		// and nothing is closed at static-destructor or DLL-detach time.
		static std::once_flag once;
		static Session* shared = nullptr;
		std::call_once(once, [] { shared = new Session(); });
		return *shared;
	}

	Session Session::Private()
	{
		return Session();
	}

	Session::~Session()
	{
		for (auto& c : m_connects)
			WinHttpCloseHandle(c.second);
		if (m_session)
			WinHttpCloseHandle(m_session);
	}

	HINTERNET Session::Connect(const std::wstring& host, INTERNET_PORT port, bool& unpacks)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		if (!m_session)
		{
			// Opened on first use (and tried again next time if it failed).
			m_session = WinHttpOpen(L"MeowyClientPack", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
				WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			if (!m_session)
				return nullptr;
			// Both only asked for: Windows without them works as before.
			DWORD unpack = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
			m_unpacks = WinHttpSetOption(m_session, WINHTTP_OPTION_DECOMPRESSION, &unpack, sizeof(unpack)) != FALSE;
			DWORD http2 = WINHTTP_PROTOCOL_FLAG_HTTP2;
			WinHttpSetOption(m_session, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &http2, sizeof(http2));
		}
		unpacks = m_unpacks;
		const std::wstring key = host + L":" + std::to_wstring(port);
		auto found = m_connects.find(key);
		if (found != m_connects.end())
			return found->second;
		HINTERNET connect = WinHttpConnect(m_session, host.c_str(), port, 0);
		if (connect)
			m_connects[key] = connect;
		return connect;
	}

	Response Session::Send(const Request& req)
	{
		Response r;
		const ULONGLONG started = GetTickCount64();
		bool unpacks = false;
		HINTERNET connect = Connect(req.host, req.port, unpacks);
		if (!connect)
		{
			r.error = "no connection (" + LastError() + ")";
			return r;
		}
		RequestHandle request;
		request.h = WinHttpOpenRequest(connect, req.verb, req.path.c_str(), nullptr, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, req.secure ? WINHTTP_FLAG_SECURE : 0);
		if (!request.h)
		{
			r.error = "no request (" + LastError() + ")";
			return r;
		}
		// 0 is WinHTTP's own default for each step (its 0 would mean no limit).
		WinHttpSetTimeouts(request.h, req.t.resolve, req.t.connect > 0 ? req.t.connect : 60000,
			req.t.send > 0 ? req.t.send : 30000, req.t.receive > 0 ? req.t.receive : 30000);
		// No redirects: the answer must come from the host itself.
		DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
		WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof(never));

		std::wstring headers = req.headers;
		if (!req.etag.empty())
			headers += L"If-None-Match: " + Widen(req.etag) + L"\r\n";
		const BOOL sent = WinHttpSendRequest(request.h,
			headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : static_cast<DWORD>(-1L),
			req.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(req.body.data()),
			static_cast<DWORD>(req.body.size()), static_cast<DWORD>(req.body.size()), 0);
		if (!sent || !WinHttpReceiveResponse(request.h, nullptr))
		{
			r.error = "no answer (" + LastError() + ")";
			return r;
		}
		DWORD status = 0, size = sizeof(status);
		WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
		r.status = status;
		r.etag = Header(request.h, WINHTTP_QUERY_ETAG, WINHTTP_HEADER_NAME_BY_INDEX);
		if (!req.readHeader.empty())
			r.header = Header(request.h, WINHTTP_QUERY_CUSTOM, req.readHeader.c_str());
		const std::string encoding = Header(request.h, WINHTTP_QUERY_CONTENT_ENCODING, WINHTTP_HEADER_NAME_BY_INDEX);
		const bool packed = !encoding.empty() && encoding != "identity";
		if (packed && !unpacks)
		{
			// WinHTTP asks for packed answers only when it can unpack them; a
			// server sending one anyway isn't read as if it were the real body.
			r.error = encoding + " body and no WinHTTP decompression";
			return r;
		}
		// The length a body sent as is must have. WinHTTP itself catches a packed
		// body or a chunked one cut off (the stream or the chunks don't end), but
		// takes a connection closed early as the end of a body with a length.
		DWORD length = 0;
		size = sizeof(length);
		const bool haveLength = !packed && WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &length, &size, WINHTTP_NO_HEADER_INDEX) != FALSE;

		// The body, read until WinHTTP says it ended (complete). A failed read
		// (reset, timeout) is "cut off", never the end of the body, and so is a
		// body sent as is that ends short of its Content-Length. Leaving
		// early closes the request, and with it the connection; a body read to
		// its end leaves the connection open for the next request.
		for (;;)
		{
			if (req.t.totalMs > 0 && GetTickCount64() - started > static_cast<ULONGLONG>(req.t.totalMs))
			{
				r.error = "too slow";
				break;
			}
			DWORD avail = 0;
			if (!WinHttpQueryDataAvailable(request.h, &avail))
			{
				r.error = "cut off (" + LastError() + ")";
				break;
			}
			if (avail == 0)
			{
				// A HEAD answer, a 204 or a 304 has a length but no body.
				const bool bodyless = status == 204 || status == 304 || (status >= 100 && status < 200) || wcscmp(req.verb, L"HEAD") == 0;
				if (haveLength && !bodyless && r.body.size() != length)
					r.error = "cut off (" + std::to_string(r.body.size()) + " of " + std::to_string(length) + " bytes)";
				else
					r.complete = true;
				break;
			}
			if (r.body.size() + avail > req.maxBody)
			{
				r.error = "too big";
				break;
			}
			const size_t was = r.body.size();
			r.body.resize(was + avail);
			DWORD read = 0;
			if (!WinHttpReadData(request.h, &r.body[was], avail, &read))
			{
				r.error = "cut off (" + LastError() + ")";
				r.body.resize(was);
				break;
			}
			r.body.resize(was + read);   // 0: the next query says it ended
		}
		return r;
	}

	void SetJoining(bool joining)
	{
		g_joining = joining;
	}

	bool Joining()
	{
		return g_joining.load();
	}

	void WaitWhileJoining(unsigned long stepMs)
	{
		while (g_joining.load())
			Sleep(stepMs);
	}
}

#endif
