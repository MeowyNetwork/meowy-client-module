#include "console_capture.h"

#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
	// Allocated once and never freed: tier0 can still log while the process
	// exits, after this DLL's statics are gone.
	struct State
	{
		std::mutex mutex;
		std::deque<console_capture::Line> lines;
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

	// Under S().mutex: the partial line becomes a line.
	void Commit()
	{
		std::string text = S().partial;
		S().partial.clear();
		while (!text.empty() && (text.back() == '\r'))
			text.pop_back();
		console_capture::Line line;
		line.id = S().nextId++;
		line.timeMs = NowMs();
		line.severity = S().partialSeverity;
		line.color = S().partialColor;
		line.text = console_capture::detail::CleanUtf8(text);
		S().lines.push_back(std::move(line));
		while (S().lines.size() > console_capture::kMaxLines)
			S().lines.pop_front();
		S().partialSeverity = 0;
		S().partialColor = 0xFFFFFFFF;
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
			std::lock_guard<std::mutex> lock(S().mutex);
			for (const char* p = text; *p; ++p)
			{
				if (S().partial.empty())
				{
					S().partialColor = color;
					S().partialSeverity = severity;
				}
				else if (severity > S().partialSeverity)
				{
					S().partialSeverity = severity;   // a line is as bad as its worst piece
				}
				if (*p == '\n')
					Commit();
				else if (S().partial.size() < kMaxLineBytes)
					S().partial.push_back(*p);
			}
		}

		void Reset()
		{
			std::lock_guard<std::mutex> lock(S().mutex);
			S().lines.clear();
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
		std::lock_guard<std::mutex> lock(S().mutex);
		if (nextId)
			*nextId = S().nextId;
		if (max == 0)
			return out;
		size_t start = S().lines.size();
		while (start > 0 && S().lines[start - 1].id > sinceId && S().lines.size() - start < max)
			--start;
		for (size_t i = start; i < S().lines.size(); ++i)
			out.push_back(S().lines[i]);
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
		std::lock_guard<std::mutex> lock(S().mutex);
		char line[160];
		std::snprintf(line, sizeof(line), "%s; %zu lines kept (of %zu), %llu seen", g_installed ? S().how.c_str() : "not hooked",
			S().lines.size(), kMaxLines, static_cast<unsigned long long>(S().nextId - 1));
		return line;
	}
}
