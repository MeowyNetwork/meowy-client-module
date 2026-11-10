#pragma once
// The pack's HTTPS requests to meowynetwork.com, in one place: the server
// index, the honest server list (verdicts and reports), the signed policy and
// the signed updater all go through here (Windows' WinHTTP, already linked).
//
//   - One WinHTTP session for the whole process (Session::Shared), opened the
//     first time it's needed and never closed: the menu realm pins this DLL, so
//     its handles live as long as the game. Requests to the same host reuse one
//     connection while the server keeps it open (keep-alive), so a pair fetched
//     back to back (policy.txt then policy.sig) pays for one TLS handshake.
//     Only the pinned menu realm uses it. The client realm, which may be
//     unloaded, uses a Session::Private that closes in its destructor.
//   - gzip and deflate answers are unpacked by WinHTTP for every request, and
//     HTTP/2 is used where Windows has it. Both are only asked for: older
//     Windows without them works as before (nothing new is imported).
//   - Never a redirect: the answer must come from the host asked.
//   - Timeouts per request, plus a total deadline over the whole request, so a
//     server trickling a byte at a time can't hold a worker for ever.
//   - A body counts as `complete` only when WinHTTP said the response ended
//     and, for a body sent as is, it has its Content-Length (WinHTTP takes a
//     connection closed early as the end of such a body; a packed or chunked
//     one cut off it reports itself). A connection reset, a timeout or a close
//     in the middle leaves it incomplete, with `error` saying so, never a
//     cut-off body that looks whole.
//
// Every call blocks: worker threads only, never the game's thread.
//
// Also here: whether a join is loading (SetJoining), which background network
// work (policy refresh, updater) waits out.

#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <map>
#include <mutex>

namespace net_http
{
	// Milliseconds. 0: WinHTTP's own default for that step (no limit on name
	// resolution, 60 s to connect, 30 s to send or receive), or no total deadline.
	struct Timeouts
	{
		int resolve = 0, connect = 0, send = 0, receive = 0, totalMs = 0;
	};

	struct Request
	{
		const wchar_t* verb = L"GET";
		std::wstring host;
		std::wstring path;
		std::wstring headers;        // extra request headers, each ending in \r\n
		std::string body;            // sent as is (set the Content-Type in headers)
		size_t maxBody = 0;          // the most the response body may be, unpacked
		Timeouts t;
		std::string etag;            // sent as If-None-Match when set
		std::wstring readHeader;     // a response header to hand back (Response::header), or none
		// Tests: a local stub over plain HTTP. Everything else: HTTPS on 443.
		INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
		bool secure = true;
	};

	struct Response
	{
		DWORD status = 0;            // 0: no answer (`error` says why)
		std::string body;            // as much of the body as arrived (complete or not)
		std::string etag;            // the response's ETag, printable ASCII, or ""
		std::string header;          // Request::readHeader's value, printable ASCII, or ""
		bool complete = false;       // the whole body arrived (WinHTTP said it ended)
		std::string error;           // "" or what went wrong: "no answer (n)", "cut off (n)", "too big", "too slow", ...
	};

	class Session
	{
	public:
		// The process-wide session: never closed. The pinned menu realm only.
		static Session& Shared();
		// A session of its own, closed when it goes: for the client realm, whose
		// DLL may be unloaded after the fetch.
		static Session Private();

		~Session();
		Session(const Session&) = delete;
		Session& operator=(const Session&) = delete;

		// One request (see the top of this file). Blocks up to the timeouts.
		Response Send(const Request& request);

	private:
		Session() = default;
		// The connect handle for host:port (made on first use), or nullptr.
		HINTERNET Connect(const std::wstring& host, INTERNET_PORT port, bool& unpacks);

		std::mutex m_lock;
		HINTERNET m_session = nullptr;
		bool m_opened = false;
		bool m_unpacks = false;   // WinHTTP unpacks gzip/deflate for this session
		std::map<std::wstring, HINTERNET> m_connects;   // "host:port" -> connect handle
	};

	// Whether a join is loading now (set by the loading-screen bindings).
	// Background network work waits while it is.
	void SetJoining(bool joining);
	bool Joining();

	// Blocks while Joining(), checking every `stepMs` (background threads only).
	void WaitWhileJoining(unsigned long stepMs = 30 * 1000);
}

#endif
