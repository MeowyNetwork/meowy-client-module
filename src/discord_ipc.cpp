#include "discord_ipc.h"

#ifndef _WIN32

namespace discord
{
	void Start(const std::string&) {}
	void SetActivity(const std::string&) {}
	void Shutdown() {}
	const char* State() { return "unsupported"; }
	std::string LastError() { return std::string(); }
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
	std::thread g_thread;
	std::atomic<bool> g_stop{ false };
	std::atomic<int> g_state{ STOPPED };

	// Guarded by g_mutex.
	std::string g_clientId;
	std::string g_activity = "null";   // latest requested activity
	bool g_dirty = false;              // g_activity not yet sent on this connection
	std::string g_lastError;           // raw payload of Discord's last error or close frame

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
				std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
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
		// block this thread (Shutdown joins it as the player leaves a server).
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
					if (GetTickCount64() > deadline)
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

			std::unique_lock<std::mutex> lock(g_mutex);
			g_wake.wait_for(lock, std::chrono::milliseconds(250), [] { return g_stop.load(); });
		}

		if (pipe.IsOpen())
			SendActivity(pipe, "null");
		g_state = STOPPED;
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
		if (g_thread.joinable())
			return;
		g_stop = false;
		g_state = CONNECTING;
		g_thread = std::thread(Worker);
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
		if (!g_thread.joinable())
			return;
		{
			// Under the lock the worker waits with, so the wake-up can't fall
			// between its check of g_stop and its wait (and be lost for up to the
			// reconnect delay, with the game's main thread waiting here).
			std::lock_guard<std::mutex> lock(g_mutex);
			g_stop = true;
		}
		g_wake.notify_one();
		g_thread.join();
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
}

#endif
