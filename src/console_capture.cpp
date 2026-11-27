#include "console_capture.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <mutex>
#endif

namespace
{
	// Allocated once and never freed: tier0 can still log while the process
	// exits, after this DLL's statics are gone.
	struct State
	{
#ifdef _WIN32
		// A critical section, not std::mutex (a slim reader/writer lock): the
		// engine logs from several threads, and when one is killed holding the
		// lock as the process exits, a critical section lets the next taker in
		// while a slim lock would hang the exit. Never deleted, like State.
		CRITICAL_SECTION lock;
		State() { InitializeCriticalSectionAndSpinCount(&lock, 4000); }
#else
		std::mutex lock;
#endif
		std::deque<console_capture::Line> lines;
		size_t bytes = 0;                 // text kept in `lines`
		uint64_t nextId = 1;
		std::string partial;              // a line still being written
		int partialSeverity = 0;
		uint32_t partialColor = 0xFFFFFFFF;
		std::string how;
	};
	State& S()
	{
		static State* s = new State();
		return *s;
	}
	std::atomic<bool> g_installed{ false };

	// Holds S()'s lock for its scope.
	class Locked
	{
	public:
#ifdef _WIN32
		Locked() { EnterCriticalSection(&S().lock); }
		~Locked() { LeaveCriticalSection(&S().lock); }
#else
		Locked() { S().lock.lock(); }
		~Locked() { S().lock.unlock(); }
#endif
		Locked(const Locked&) = delete;
		Locked& operator=(const Locked&) = delete;
	};

	int64_t NowMs()
	{
#ifdef _WIN32
		FILETIME ft;
		GetSystemTimeAsFileTime(&ft);
		const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
		return static_cast<int64_t>((t - 116444736000000000ull) / 10000ull);
#else
		return 0;
#endif
	}

	size_t SequenceLength(const std::string& s, size_t i)
	{
		const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
		const auto cont = [&](size_t k) { return k < s.size() && (b(k) & 0xC0) == 0x80; };
		const unsigned char c = b(i);
		const size_t left = s.size() - i;
		if (c < 0x80) return 1;
		if (c >= 0xC2 && c <= 0xDF) return (left >= 2 && cont(i + 1)) ? 2 : 0;
		if (c >= 0xE0 && c <= 0xEF)
		{
			if (left < 3 || !cont(i + 1) || !cont(i + 2)) return 0;
			if (c == 0xE0 && b(i + 1) < 0xA0) return 0;
			if (c == 0xED && b(i + 1) > 0x9F) return 0;
			return 3;
		}
		if (c >= 0xF0 && c <= 0xF4)
		{
			if (left < 4 || !cont(i + 1) || !cont(i + 2) || !cont(i + 3)) return 0;
			if (c == 0xF0 && b(i + 1) < 0x90) return 0;
			if (c == 0xF4 && b(i + 1) > 0x8F) return 0;
			return 4;
		}
		return 0;
	}

	bool Ascii(const std::string& s)
	{
		for (const char c : s)
			if (static_cast<unsigned char>(c) >= 0x80)
				return false;
		return true;
	}

	// Under the lock: the partial line becomes a line (its text moved, not
	// copied; cleaned only when it has a byte that isn't ASCII), and the oldest
	// go while there are too many lines or too much text.
	void Commit()
	{
		State& s = S();
		console_capture::Line line;
		line.text = std::move(s.partial);
		s.partial.clear();   // moved from: empty again, whatever the library left
		while (!line.text.empty() && line.text.back() == '\r')
			line.text.pop_back();
		if (!Ascii(line.text))
			line.text = console_capture::detail::CleanUtf8(line.text);
		line.id = s.nextId++;
		line.timeMs = NowMs();
		line.severity = s.partialSeverity;
		line.color = s.partialColor;
		s.bytes += line.text.size();
		s.lines.push_back(std::move(line));
		while (!s.lines.empty() && (s.lines.size() > console_capture::kMaxLines || s.bytes > console_capture::kMaxTextBytes))
		{
			s.bytes -= s.lines.front().text.size();
			s.lines.pop_front();
		}
		s.partialSeverity = 0;
		s.partialColor = 0xFFFFFFFF;
	}

#ifdef _WIN32
	// --- Source's logging system: one listener (logging.h ABI, minimal) ---------
	struct Color4 { unsigned char r, g, b, a; };
	struct LoggingContext_t { int channelId; int flags; int severity; Color4 color; };
	struct ILoggingListener { virtual void Log(const LoggingContext_t* context, const char* message) = 0; };
	struct Listener : ILoggingListener
	{
		void Log(const LoggingContext_t* context, const char* message) override
		{
			uint32_t color = 0xFFFFFFFF;
			if (context && (context->color.r | context->color.g | context->color.b))
				color = (context->color.r << 16) | (context->color.g << 8) | context->color.b;
			console_capture::detail::Feed(context ? context->severity : 0, message, color);
		}
	};
	Listener g_listener;

	// --- Source 2013's spew function, chained ------------------------------------
	enum SpewRetval { SPEW_DEBUGGER = 0, SPEW_CONTINUE, SPEW_ABORT };
	using SpewFn = SpewRetval (*)(int type, const char* message);
	using SetSpewFn = void (*)(SpewFn);
	using GetSpewFn = SpewFn (*)();
	struct SpewColor { unsigned char r, g, b, a; };
	using GetColorFn = const SpewColor* (*)();
	SpewFn g_previous = nullptr;
	GetColorFn g_getColor = nullptr;

	SpewRetval Spew(int type, const char* message)
	{
		uint32_t color = 0xFFFFFFFF;
		if (g_getColor)
		{
			const SpewColor* c = g_getColor();
			if (c && (c->r | c->g | c->b))
				color = (c->r << 16) | (c->g << 8) | c->b;
		}
		// SPEW_MESSAGE 0, WARNING 1, ASSERT 2, ERROR 3, LOG 4 (a message).
		console_capture::detail::Feed(type == 4 ? 0 : type, message, color);
		return g_previous ? g_previous(type, message) : SPEW_CONTINUE;
	}
#endif
}

namespace console_capture
{
	namespace detail
	{
		std::string CleanUtf8(const std::string& s)
		{
			std::string out;
			out.reserve(s.size());
			for (size_t i = 0; i < s.size();)
			{
				const size_t n = SequenceLength(s, i);
				if (n)
				{
					out.append(s, i, n);
					i += n;
				}
				else
				{
					out += "\xEF\xBF\xBD";
					++i;
				}
			}
			return out;
		}

		void Feed(int severity, const char* text, uint32_t color)
		{
			if (!text)
				return;
			const char* p = text;
			const char* const end = text + std::strlen(text);
			Locked lock;
			State& s = S();
			// A piece at a time up to each line break: the line takes the colour
			// of its first piece, and the worst severity of all of them.
			while (p < end)
			{
				const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
				const char* stop = nl ? nl : end;
				if (s.partial.empty())
				{
					s.partialColor = color;
					s.partialSeverity = severity;
				}
				else if (severity > s.partialSeverity)
				{
					s.partialSeverity = severity;   // a line is as bad as its worst piece
				}
				const size_t room = kMaxLineBytes - s.partial.size();   // partial never passes kMaxLineBytes
				const size_t n = static_cast<size_t>(stop - p);
				s.partial.append(p, n < room ? n : room);
				if (!nl)
					break;
				Commit();
				p = nl + 1;
			}
		}

		void Reset()
		{
			Locked lock;
			S().lines.clear();
			S().bytes = 0;
			S().partial.clear();
			S().nextId = 1;
			S().partialSeverity = 0;
			S().partialColor = 0xFFFFFFFF;
		}
	}

	bool Install(std::string& why)
	{
		if (g_installed)
			return true;
#ifdef _WIN32
		HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
		if (!tier0)
		{
			why = "tier0 isn't loaded";
			return false;
		}
		using RegisterFn = void (*)(ILoggingListener*);
		if (auto reg = reinterpret_cast<RegisterFn>(GetProcAddress(tier0, "LoggingSystem_RegisterLoggingListener")))
		{
			reg(&g_listener);
			S().how = "logging listener";
			g_installed = true;
			return true;
		}
		auto set = reinterpret_cast<SetSpewFn>(GetProcAddress(tier0, "SpewOutputFunc"));
		if (!set)
			set = reinterpret_cast<SetSpewFn>(GetProcAddress(tier0, "SetSpewOutputFunc"));
		auto get = reinterpret_cast<GetSpewFn>(GetProcAddress(tier0, "GetSpewOutputFunc"));
		if (set && get)
		{
			g_getColor = reinterpret_cast<GetColorFn>(GetProcAddress(tier0, "GetSpewOutputColor"));
			g_previous = get();
			set(&Spew);
			S().how = "spew function";
			g_installed = true;
			return true;
		}
		why = "tier0 exports neither a logging listener nor a spew function";
		return false;
#else
		why = "Windows only";
		return false;
#endif
	}

	bool Installed() { return g_installed; }

	std::vector<Line> Read(uint64_t sinceId, size_t max, uint64_t* nextId)
	{
		std::vector<Line> out;
		Locked lock;
		const State& s = S();
		if (nextId)
			*nextId = s.nextId;
		const size_t size = s.lines.size();
		if (max == 0 || size == 0)
			return out;
		// Ids follow one another in the ring (a line is only ever added at the
		// back and dropped at the front), so the first line after `sinceId` is
		// found by arithmetic, not by walking back.
		const uint64_t front = s.lines.front().id;
		size_t start = 0;
		if (sinceId >= front)
		{
			const uint64_t skip = sinceId - front + 1;
			start = skip >= size ? size : static_cast<size_t>(skip);
		}
		if (size - start > max)
			start = size - max;
		out.reserve(size - start);
		for (size_t i = start; i < size; ++i)
			out.push_back(s.lines[i]);
		return out;
	}

#ifdef _WIN32
	namespace
	{
		constexpr int kClientCmdUnrestricted = 106;
#ifdef _WIN64
		using ClientCmdFn = void (*)(void* self, const char* line);
#else
		using ClientCmdFn = void (__thiscall*)(void* self, const char* line);
#endif
		bool g_engineBroken = false;

		// No C++ objects in here: structured exception handling.
		bool CallClientCmd(void* client, const char* line)
		{
			__try
			{
				void** vtable = *reinterpret_cast<void***>(client);
				reinterpret_cast<ClientCmdFn>(vtable[kClientCmdUnrestricted])(client, line);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				g_engineBroken = true;
				return false;
			}
		}
	}
#endif

	bool RunLine(const char* line)
	{
#ifdef _WIN32
		if (!line || !*line || g_engineBroken)
			return false;
		static void* client = nullptr;
		if (!client)
		{
			HMODULE engine = GetModuleHandleW(L"engine.dll");
			using CreateInterfaceFn = void* (*)(const char* name, int* code);
			auto create = engine ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(engine, "CreateInterface")) : nullptr;
			client = create ? create("VEngineClient015", nullptr) : nullptr;
			if (!client)
				return false;
		}
		// One line, no line breaks: what the console's own input box gives.
		std::string text;
		for (const char* p = line; *p && text.size() < 1024; ++p)
			if (*p != '\n' && *p != '\r')
				text.push_back(*p);
		return !text.empty() && CallClientCmd(client, text.c_str());
#else
		(void)line;
		return false;
#endif
	}

	std::string Describe()
	{
		Locked lock;
		char line[200];
		std::snprintf(line, sizeof(line), "%s; %zu lines kept (of %zu), %zu KB of text (of %zu), %llu seen",
			g_installed ? S().how.c_str() : "not hooked", S().lines.size(), kMaxLines, S().bytes / 1024, kMaxTextBytes / 1024,
			static_cast<unsigned long long>(S().nextId - 1));
		return line;
	}
}
