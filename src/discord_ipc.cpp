#include "discord_ipc.h"

#include <cstdlib>

namespace discord
{
	bool OffInMenuSettings(const std::string& text)
	{
		const std::string key = "meowy_discord=";
		size_t at = 0;
		while (at < text.size())
		{
			size_t end = text.find('\n', at);
			if (end == std::string::npos)
				end = text.size();
			std::string line = text.substr(at, end - at);
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			at = end + 1;
			if (line.compare(0, key.size(), key) != 0)
				continue;
			// settings.lua keeps a protected setting only as a number
			// (^%-?%d+%.?%d*$); anything else starts from the default (on).
			const std::string value = line.substr(key.size());
			size_t i = value.size() > 0 && value[0] == '-' ? 1 : 0;
			const size_t digits = i;
			while (i < value.size() && value[i] >= '0' && value[i] <= '9')
				++i;
			if (i == digits)
				return false;
			if (i < value.size() && value[i] == '.')
				++i;
			while (i < value.size() && value[i] >= '0' && value[i] <= '9')
				++i;
			return i == value.size() && std::strtod(value.c_str(), nullptr) == 0.0;
		}
		return false;
	}
}

#ifndef _WIN32

namespace discord
{
	void Start(const std::string&) {}
	void SetActivity(const std::string&) {}
	void Shutdown() {}
	void SetAllowed(bool) {}
	bool Allowed() { return true; }
	const char* State() { return "unsupported"; }
	std::string LastError() { return std::string(); }
	namespace detail
	{
		void SetPipePrefix(const std::wstring&) {}
	}
}

#else

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "thread_util.h"

namespace
{
	using Clock = std::chrono::steady_clock;

	// Discord rate-limits SET_ACTIVITY to 5 per 20 s; one per 4 s stays under it.
	constexpr auto MIN_UPDATE_INTERVAL = std::chrono::seconds(4);
	constexpr auto RECONNECT_DELAY = std::chrono::seconds(5);
	constexpr int HANDSHAKE_TIMEOUT_MS = 5000;

	enum Opcode : uint32_t { OP_HANDSHAKE = 0, OP_FRAME = 1, OP_CLOSE = 2, OP_PING = 3, OP_PONG = 4 };
	enum StateId : int { STOPPED = 0, CONNECTING = 1, READY = 2 };

	std::mutex g_mutex;
	std::condition_variable g_wake;
	// The worker's thread, and one that didn't stop in time and was let go
	// (StopWorker). Plain handles, never a std::thread: nothing is joined or
	// terminated when the DLL's statics go at exit. Game thread only.
	HANDLE g_thread = nullptr;
	HANDLE g_stale = nullptr;
	std::atomic<bool> g_stop{ false };
	std::atomic<int> g_state{ STOPPED };

	// Guarded by g_mutex.
	std::string g_clientId;
	std::string g_activity = "null";   // latest requested activity
	bool g_dirty = false;              // g_activity not yet sent on this connection
	std::string g_lastError;           // raw payload of Discord's last error or close frame

	// Game thread only: the player's switch, and whether a server asked for the
	// status since the client realm last closed (Start, until Shutdown).
	bool g_allowed = true;
	bool g_wanted = false;

	// Discord's pipes, "<prefix>0".."<prefix>9" (tests: detail::SetPipePrefix).
	std::wstring g_pipePrefix = L"\\\\.\\pipe\\discord-ipc-";

	constexpr uint32_t kMaxFrame = 64 * 1024;
	constexpr ULONGLONG kReadWaitMs = 2000;

	class Pipe
	{
	public:
		~Pipe() { Close(); }

		bool Open()
		{
			for (int i = 0; i < 10; ++i)
			{
				std::wstring name = g_pipePrefix + std::to_wstring(i);
				// Identification only: whatever owns the pipe (Discord, or something
				// squatting its name) can't act as this user through it.
				m_handle = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
					SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
				if (m_handle != INVALID_HANDLE_VALUE)
					return true;
			}
			return false;
		}

		void Close()
		{
			if (m_handle != INVALID_HANDLE_VALUE)
			{
				CloseHandle(m_handle);
				m_handle = INVALID_HANDLE_VALUE;
			}
		}

		bool IsOpen() const { return m_handle != INVALID_HANDLE_VALUE; }

		bool Write(uint32_t op, const std::string& json)
		{
			std::string frame(8 + json.size(), '\0');
			uint32_t len = static_cast<uint32_t>(json.size());
			memcpy(&frame[0], &op, 4);
			memcpy(&frame[4], &len, 4);
			memcpy(&frame[8], json.data(), json.size());
			return WriteAll(frame.data(), static_cast<DWORD>(frame.size()));
		}

		// Reads one frame if a whole header is waiting. Returns false on a broken
		// pipe; sets `got` when a frame was read.
		bool TryRead(uint32_t& op, std::string& payload, bool& got)
		{
			got = false;
			DWORD avail = 0;
			if (!PeekNamedPipe(m_handle, nullptr, 0, nullptr, &avail, nullptr))
				return false;
			if (avail < 8)
				return true;

			uint32_t header[2];
			if (!ReadAll(header, 8))
				return false;
			// Discord's frames are a few KB. A length past the cap is a broken or
			// hostile pipe: drop the connection rather than allocate it.
			if (header[1] > kMaxFrame)
				return false;
			op = header[0];
			payload.assign(header[1], '\0');
			if (header[1] > 0 && !ReadAll(&payload[0], header[1]))
				return false;
			got = true;
			return true;
		}

	private:
		bool WriteAll(const char* data, DWORD size)
		{
			while (size > 0)
			{
				DWORD written = 0;
				if (!WriteFile(m_handle, data, size, &written, nullptr) || written == 0)
					return false;
				data += written;
				size -= written;
			}
			return true;
		}

		// Reads only what the pipe already holds, waiting up to kReadWaitMs for
		// the rest of a frame: a frame that announces more than it sends must not
		// block this thread (Shutdown waits for it as the player leaves a
		// server). A stop ends the wait at once.
		bool ReadAll(void* out, DWORD size)
		{
			char* p = static_cast<char*>(out);
			const ULONGLONG deadline = GetTickCount64() + kReadWaitMs;
			while (size > 0)
			{
				DWORD avail = 0;
				if (!PeekNamedPipe(m_handle, nullptr, 0, nullptr, &avail, nullptr))
					return false;
				if (avail == 0)
				{
					if (g_stop || GetTickCount64() > deadline)
						return false;
					Sleep(5);
					continue;
				}
				DWORD got = 0;
				if (!ReadFile(m_handle, p, avail < size ? avail : size, &got, nullptr) || got == 0)
					return false;
				p += got;
				size -= got;
			}
			return true;
		}

		HANDLE m_handle = INVALID_HANDLE_VALUE;
	};

	// Handles PING and CLOSE; returns false when the connection is over.
	bool Pump(Pipe& pipe, uint32_t* lastOp = nullptr, std::string* lastPayload = nullptr)
	{
		for (;;)
		{
			uint32_t op = 0;
			std::string payload;
			bool got = false;
			if (!pipe.TryRead(op, payload, got))
				return false;
			if (!got)
				return true;
			if (op == OP_PING)
			{
				if (!pipe.Write(OP_PONG, payload))
					return false;
			}
			else if (op == OP_CLOSE)
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				g_lastError = payload;
				return false;
			}
			else if (op == OP_FRAME && payload.find("\"evt\":\"ERROR\"") != std::string::npos)
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				g_lastError = payload;
			}
			if (lastOp) *lastOp = op;
			if (lastPayload) *lastPayload = payload;
		}
	}

	bool Handshake(Pipe& pipe, const std::string& clientId)
	{
		// Discord client ids are numeric snowflakes; nothing to escape.
		if (!pipe.Write(OP_HANDSHAKE, "{\"v\":1,\"client_id\":\"" + clientId + "\"}"))
			return false;

		auto deadline = Clock::now() + std::chrono::milliseconds(HANDSHAKE_TIMEOUT_MS);
		while (Clock::now() < deadline && !g_stop)
		{
			uint32_t op = 0xFFFFFFFF;
			std::string payload;
			if (!Pump(pipe, &op, &payload))
				return false;
			if (op == OP_FRAME)
				return payload.find("\"READY\"") != std::string::npos;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		return false;
	}

	bool SendActivity(Pipe& pipe, const std::string& activity)
	{
		static unsigned long long nonce = 0;
		std::string msg = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) +
			",\"activity\":" + activity + "},\"nonce\":\"" + std::to_string(++nonce) + "\"}";
		return pipe.Write(OP_FRAME, msg);
	}

	void Worker()
	{
		Pipe pipe;
		std::string connectedId;
		Clock::time_point lastSend{};

		while (!g_stop)
		{
			std::string clientId;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				clientId = g_clientId;
			}

			if (pipe.IsOpen() && clientId != connectedId)
				pipe.Close();

			if (!pipe.IsOpen())
			{
				g_state = CONNECTING;
				if (!pipe.Open() || !Handshake(pipe, clientId))
				{
					pipe.Close();
					std::unique_lock<std::mutex> lock(g_mutex);
					g_wake.wait_for(lock, RECONNECT_DELAY, [] { return g_stop.load(); });
					continue;
				}
				connectedId = clientId;
				g_state = READY;
				std::lock_guard<std::mutex> lock(g_mutex);
				g_dirty = true;   // new connection: send the current activity again
			}

			if (!Pump(pipe))
			{
				pipe.Close();
				continue;
			}

			std::string toSend;
			bool send = false;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				if (g_dirty && Clock::now() - lastSend >= MIN_UPDATE_INTERVAL)
				{
					toSend = g_activity;
					g_dirty = false;
					send = true;
				}
			}
			if (send)
			{
				if (!SendActivity(pipe, toSend))
				{
					pipe.Close();
					std::lock_guard<std::mutex> lock(g_mutex);
					g_dirty = true;
					continue;
				}
				lastSend = Clock::now();
			}

			// Woken early by a stop, or by a new activity once the update interval
			// allows sending it (before that, waking for it would only spin).
			std::unique_lock<std::mutex> lock(g_mutex);
			g_wake.wait_for(lock, std::chrono::milliseconds(250),
				[&lastSend] { return g_stop.load() || (g_dirty && Clock::now() - lastSend >= MIN_UPDATE_INTERVAL); });
		}

		if (pipe.IsOpen())
			SendActivity(pipe, "null");
		g_state = STOPPED;
	}
}

namespace
{
	// How long StopWorker waits for the worker (game thread), and how long a
	// new start waits for one that was let go.
	constexpr DWORD kStopWaitMs = 1000;
	constexpr DWORD kStaleWaitMs = 250;

	void StartWorker()
	{
		if (g_thread)
			return;
		// A worker that didn't stop in time still runs on the same state (g_stop,
		// the activity): never two at once. Given a moment more; still there, no
		// worker this time (the next Start or switch on tries again).
		if (g_stale)
		{
			if (WaitForSingleObject(g_stale, kStaleWaitMs) == WAIT_TIMEOUT)
			{
				g_state = STOPPED;
				std::lock_guard<std::mutex> lock(g_mutex);
				g_lastError = "{\"message\":\"the previous connection to Discord is still stopping\"}";
				return;
			}
			CloseHandle(g_stale);
			g_stale = nullptr;
		}
		g_stop = false;
		g_state = CONNECTING;
		g_thread = thread_util::SpawnHandle(L"Meowy Discord", Worker, thread_util::Priority::BelowNormal);
		if (!g_thread)
			g_state = STOPPED;
	}

	// The worker clears what Discord shows on its way out (a "null" activity),
	// whatever the update interval. Waits at most about kStopWaitMs (the game's
	// thread is waiting here as the player leaves a server): a worker stuck in a
	// pipe write (Discord not reading) has that write cancelled, and one that
	// still doesn't stop is let go. It holds its own reference to this DLL
	// (thread_util), so that's safe even as the DLL is freed.
	void StopWorker()
	{
		if (!g_thread)
			return;
		{
			// Under the lock the worker waits with, so the wake-up can't fall
			// between its check of g_stop and its wait (and be lost for up to the
			// reconnect delay, with the game's main thread waiting here).
			std::lock_guard<std::mutex> lock(g_mutex);
			g_stop = true;
		}
		g_wake.notify_one();
		bool done = false;
		const ULONGLONG deadline = GetTickCount64() + kStopWaitMs;
		for (;;)
		{
			if (WaitForSingleObject(g_thread, 50) == WAIT_OBJECT_0)
			{
				done = true;
				break;
			}
			if (GetTickCount64() >= deadline)
				break;
			// Breaks a synchronous WriteFile/ReadFile on the pipe (Discord hung
			// with its end full); nothing when the worker isn't in one.
			CancelSynchronousIo(g_thread);
		}
		if (done)
			CloseHandle(g_thread);
		else
		{
			if (g_stale)
				CloseHandle(g_stale);   // can't be: StartWorker doesn't start beside one
			g_stale = g_thread;
			g_state = STOPPED;
		}
		g_thread = nullptr;
	}
}

namespace discord
{
	void Start(const std::string& clientId)
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_clientId = clientId;
		}
		g_wanted = true;
		if (g_allowed)
			StartWorker();
	}

	void SetActivity(const std::string& activityJson)
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (activityJson == g_activity)
				return;
			g_activity = activityJson;
			g_dirty = true;
		}
		g_wake.notify_one();
	}

	void Shutdown()
	{
		g_wanted = false;
		StopWorker();
	}

	void SetAllowed(bool on)
	{
		if (on == g_allowed)
			return;
		g_allowed = on;
		if (!on)
			StopWorker();
		else if (g_wanted)
			StartWorker();   // what the server asked for last is sent once connected
	}

	bool Allowed()
	{
		return g_allowed;
	}

	const char* State()
	{
		switch (g_state.load())
		{
		case READY: return "ready";
		case CONNECTING: return "connecting";
		default: return "stopped";
		}
	}

	std::string LastError()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_lastError;
	}

	namespace detail
	{
		void SetPipePrefix(const std::wstring& prefix)
		{
			g_pipePrefix = prefix;
		}
	}
}

#endif
