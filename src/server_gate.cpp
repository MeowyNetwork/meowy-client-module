#include "server_gate.h"

#include <cstdint>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
	// Meowy Network's servers: address and port. Only these ports: the same
	// host also runs throwaway test servers (27016, ...), which aren't ours to
	// rejoin, show our loading screen for, or restore the start-up line from.
	struct Server { const char* ip; int port; };
	const Server kAllowed[] = {
		{ "138.201.62.52", 27015 },
	};

	// IVEngineClient015 (Source SDK 2013 layout; the version string is the
	// engine's promise that this layout holds, on both branches).
	constexpr int kGetNetChannelInfo = 72;
	// INetChannelInfo (Source SDK 2013's inetchannelinfo.h): GetName 0,
	// GetAddress 1, GetTime 2, GetTimeConnected 3, GetBufferSize 4,
	// GetDataRate 5, IsLoopback 6, IsTimingOut 7, ... GetStreamProgress 20,
	// GetTimeSinceLastReceived 21.
	constexpr int kGetAddress = 1;
	constexpr int kGetTimeConnected = 3;
	constexpr int kIsTimingOut = 7;
	constexpr int kGetAvgLatency = 10;   // (int flow)
	constexpr int kGetAvgLoss = 11;      // (int flow)
	constexpr int kGetTimeSinceLastReceived = 21;
	constexpr int kFlowOutgoing = 0, kFlowIncoming = 1;

	constexpr size_t kMaxAddress = 64;

#ifdef _WIN32
	// engine.dll's executable sections, as [start, end) addresses.
	struct CodeRanges
	{
		static constexpr int kMax = 8;
		uintptr_t start[kMax];
		uintptr_t end[kMax];
		int count;
	};

	// Does p point into one of those sections? Two compares a section, where
	// it used to walk the PE headers on every call.
	bool InCode(const CodeRanges& code, const void* p)
	{
		const uintptr_t at = reinterpret_cast<uintptr_t>(p);
		for (int i = 0; i < code.count; ++i)
			if (at >= code.start[i] && at < code.end[i])
				return true;
		return false;
	}

	// The executable sections of `mod`, from its PE headers. Free of C++
	// objects so it can use structured exception handling: headers that can't
	// be read give no ranges, so nothing counts as engine code (fails closed).
	// More sections than kMax (engine.dll has one or two) are left out, which
	// can only refuse, never allow.
	bool ReadCodeRanges(HMODULE mod, CodeRanges& out)
	{
		out.count = 0;
		__try
		{
			auto* base = reinterpret_cast<const unsigned char*>(mod);
			auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE)
				return false;
			auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE)
				return false;
			const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
			for (WORD i = 0; i < nt->FileHeader.NumberOfSections && out.count < CodeRanges::kMax; ++i, ++sec)
			{
				if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE))
					continue;
				const uintptr_t start = reinterpret_cast<uintptr_t>(base + sec->VirtualAddress);
				out.start[out.count] = start;
				out.end[out.count] = start + sec->Misc.VirtualSize;
				++out.count;
			}
			return out.count > 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out.count = 0;
			return false;
		}
	}

	// engine.dll, its VEngineClient015 and its code ranges, resolved once:
	// engine.dll is never unloaded while the game runs and the interface is a
	// singleton, so this used to repeat CreateInterface's walk of the
	// interface list and a PE walk several times a frame for the same answer.
	// Resolved again only when engine.dll's base isn't the cached one or
	// nothing was found yet. The net channel itself is still read live on
	// every call. Main thread only: every caller is a Lua binding.
	struct Engine
	{
		HMODULE module;
		void* client;
		CodeRanges code;
	};

	const Engine* ResolveEngine()
	{
		static Engine cached{};
		HMODULE module = GetModuleHandleW(L"engine.dll");
		if (!module)
			return nullptr;
		if (module == cached.module && cached.client)
			return &cached;

		cached = Engine{};
		using CreateInterfaceFn = void* (*)(const char* name, int* code);
		auto create = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface"));
		if (!create)
			return nullptr;
		void* client = create("VEngineClient015", nullptr);
		if (!client)
			return nullptr;
		CodeRanges code{};
		if (!ReadCodeRanges(module, code))
			return nullptr;
		cached.code = code;
		cached.client = client;
		cached.module = module;
		return &cached;
	}

	// On x86 the engine's methods are __thiscall; __fastcall with an unused
	// second argument passes `this` in ecx the same way. On x64 both are the
	// normal convention and the extra argument is ignored.
	using GetNetChannelInfoFn = void* (__fastcall*)(void* self, void* unused);
	using GetAddressFn = const char* (__fastcall*)(void* self, void* unused);
	using GetFloatFn = float (__fastcall*)(void* self, void* unused);
	using GetBoolFn = bool (__fastcall*)(void* self, void* unused);
	// A method taking one int (the flow). x86: `this` in ecx, the int on the
	// stack, which __fastcall's unused edx slot leaves in the same place. x64:
	// the int is the second argument (rdx), so no unused slot.
#ifdef _WIN64
	using GetFlowFloatFn = float (*)(void* self, int flow);
	float CallFlow(void* fn, void* self, int flow) { return reinterpret_cast<GetFlowFloatFn>(fn)(self, flow); }
#else
	using GetFlowFloatFn = float (__fastcall*)(void* self, void* unused, int flow);
	float CallFlow(void* fn, void* self, int flow) { return reinterpret_cast<GetFlowFloatFn>(fn)(self, nullptr, flow); }
#endif

	enum RawStatus { kRawOk, kRawNotConnected, kRawFailed };

	// Kept free of C++ objects so it can use structured exception handling: a
	// bad pointer anywhere in here means "can't tell", never a crash.
	RawStatus ReadAddress(const Engine& engine, char* out, size_t outSize)
	{
		__try
		{
			void* client = engine.client;
			void** vtable = *reinterpret_cast<void***>(client);
			void* getNci = vtable[kGetNetChannelInfo];
			if (!InCode(engine.code, getNci))
				return kRawFailed;

			void* nci = reinterpret_cast<GetNetChannelInfoFn>(getNci)(client, nullptr);
			if (!nci)
				return kRawNotConnected;

			void** nciVtable = *reinterpret_cast<void***>(nci);
			void* getAddress = nciVtable[kGetAddress];
			if (!InCode(engine.code, getAddress))
				return kRawFailed;

			const char* addr = reinterpret_cast<GetAddressFn>(getAddress)(nci, nullptr);
			if (!addr)
				return kRawFailed;

			size_t n = 0;
			while (n + 1 < outSize && addr[n])
			{
				out[n] = addr[n];
				++n;
			}
			if (addr[n] != '\0')
				return kRawFailed;   // longer than any real address
			out[n] = '\0';
			return kRawOk;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return kRawFailed;
		}
	}

	// Same guards as ReadAddress, for IsTimingOut and GetTimeSinceLastReceived.
	RawStatus ReadHealth(const Engine& engine, bool& timingOut, float& silence)
	{
		__try
		{
			void* client = engine.client;
			void** vtable = *reinterpret_cast<void***>(client);
			void* getNci = vtable[kGetNetChannelInfo];
			if (!InCode(engine.code, getNci))
				return kRawFailed;
			void* nci = reinterpret_cast<GetNetChannelInfoFn>(getNci)(client, nullptr);
			if (!nci)
				return kRawNotConnected;
			void** nciVtable = *reinterpret_cast<void***>(nci);
			void* isTimingOut = nciVtable[kIsTimingOut];
			void* sinceReceived = nciVtable[kGetTimeSinceLastReceived];
			if (!InCode(engine.code, isTimingOut) || !InCode(engine.code, sinceReceived))
				return kRawFailed;
			timingOut = reinterpret_cast<GetBoolFn>(isTimingOut)(nci, nullptr);
			silence = reinterpret_cast<GetFloatFn>(sinceReceived)(nci, nullptr);
			return kRawOk;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return kRawFailed;
		}
	}

	// Same guards, for GetAvgLatency (outgoing: the round trip, as net_graph
	// shows it), GetAvgLoss (incoming) and GetTimeConnected.
	RawStatus ReadStats(const Engine& engine, float& latency, float& loss, float& connected)
	{
		__try
		{
			void* client = engine.client;
			void** vtable = *reinterpret_cast<void***>(client);
			void* getNci = vtable[kGetNetChannelInfo];
			if (!InCode(engine.code, getNci))
				return kRawFailed;
			void* nci = reinterpret_cast<GetNetChannelInfoFn>(getNci)(client, nullptr);
			if (!nci)
				return kRawNotConnected;
			void** nciVtable = *reinterpret_cast<void***>(nci);
			void* avgLatency = nciVtable[kGetAvgLatency];
			void* avgLoss = nciVtable[kGetAvgLoss];
			void* timeConnected = nciVtable[kGetTimeConnected];
			if (!InCode(engine.code, avgLatency) || !InCode(engine.code, avgLoss) || !InCode(engine.code, timeConnected))
				return kRawFailed;
			latency = CallFlow(avgLatency, nci, kFlowOutgoing);
			loss = CallFlow(avgLoss, nci, kFlowIncoming);
			connected = reinterpret_cast<GetFloatFn>(timeConnected)(nci, nullptr);
			return kRawOk;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return kRawFailed;
		}
	}
#endif

	// "a.b.c.d:port" with every part in range, as "a.b.c.d" and the port.
	bool ParseIpv4WithPort(const char* s, std::string& ip, int& portOut)
	{
		const char* p = s;
		for (int part = 0; part < 4; ++part)
		{
			int value = 0, digits = 0;
			while (*p >= '0' && *p <= '9')
			{
				value = value * 10 + (*p - '0');
				if (++digits > 3 || value > 255)
					return false;
				++p;
			}
			if (digits == 0)
				return false;
			if (part < 3)
			{
				if (*p != '.')
					return false;
				++p;
			}
		}
		ip.assign(s, static_cast<size_t>(p - s));
		if (*p != ':')
			return false;
		++p;
		int port = 0, digits = 0;
		while (*p >= '0' && *p <= '9')
		{
			port = port * 10 + (*p - '0');
			if (++digits > 5 || port > 65535)
				return false;
			++p;
		}
		portOut = port;
		return digits > 0 && port > 0 && *p == '\0';
	}

	bool Allowed(const std::string& ip, int port)
	{
		for (const Server& s : kAllowed)
			if (ip == s.ip && port == s.port)
				return true;
		return false;
	}
}

namespace server_gate
{
	Result Check(std::string* address)
	{
#ifdef _WIN32
		const Engine* engine = ResolveEngine();
		if (!engine)
			return Result::Unavailable;

		char buf[kMaxAddress];
		switch (ReadAddress(*engine, buf, sizeof(buf)))
		{
		case kRawNotConnected:
			return Result::NotConnected;
		case kRawFailed:
			return Result::Unavailable;
		case kRawOk:
			break;
		}
		if (address)
			*address = buf;

		std::string ip;
		int port = 0;
		if (!ParseIpv4WithPort(buf, ip, port))
			return Result::OtherServer;   // "loopback" (singleplayer) and anything odd
		return Allowed(ip, port) ? Result::Allowed : Result::OtherServer;
#else
		(void)address;
		return Result::Unavailable;
#endif
	}

	bool IsOurAddress(const std::string& address)
	{
		std::string ip;
		int port = 0;
		return ParseIpv4WithPort(address.c_str(), ip, port) && Allowed(ip, port);
	}

	std::string PrimaryAddress()
	{
		return std::string(kAllowed[0].ip) + ":" + std::to_string(kAllowed[0].port);
	}

	bool NetHealth(bool& timingOut, double& silence)
	{
#ifdef _WIN32
		const Engine* engine = ResolveEngine();
		if (!engine)
			return false;
		bool timing = false;
		float value = -1.0f;
		if (ReadHealth(*engine, timing, value) != kRawOk)
			return false;
		// Anything outside a sane range means the layout isn't what we expect.
		if (!(value >= 0.0f && value <= 3600.0f))
			return false;
		timingOut = timing;
		silence = value;
		return true;
#else
		(void)timingOut;
		(void)silence;
		return false;
#endif
	}

	bool NetStats(double& latency, double& loss, double& connected)
	{
#ifdef _WIN32
		const Engine* engine = ResolveEngine();
		if (!engine)
			return false;
		float lat = -1.0f, ls = -1.0f, conn = -1.0f;
		if (ReadStats(*engine, lat, ls, conn) != kRawOk)
			return false;
		// Outside these, the layout isn't what we expect: say nothing.
		if (!(lat >= 0.0f && lat <= 10.0f) || !(ls >= 0.0f && ls <= 1.0f) || !(conn >= 0.0f && conn <= 31.0f * 86400.0f))
			return false;
		latency = lat;
		loss = ls;
		connected = conn;
		return true;
#else
		(void)latency;
		(void)loss;
		(void)connected;
		return false;
#endif
	}

	const char* Describe(Result r)
	{
		switch (r)
		{
		case Result::Allowed: return "allowed";
		case Result::NotConnected: return "not connected to a server";
		case Result::OtherServer: return "only works on Meowy Network servers";
		case Result::Unavailable: return "couldn't check which server this is";
		}
		return "unknown";
	}
}
