#include "server_gate.h"

#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
	// Meowy Network's servers. Any port on these addresses.
	const char* const kAllowedIps[] = {
		"138.201.62.52",
	};

	// IVEngineClient015 (Source SDK 2013 layout; the version string is the
	// engine's promise that this layout holds, on both branches).
	constexpr int kGetNetChannelInfo = 72;
	// INetChannelInfo (Source SDK 2013's inetchannelinfo.h): GetName 0,
	// GetAddress 1, GetTime 2, GetTimeConnected 3, GetBufferSize 4,
	// GetDataRate 5, IsLoopback 6, IsTimingOut 7, ... GetStreamProgress 20,
	// GetTimeSinceLastReceived 21.
	constexpr int kGetAddress = 1;
	constexpr int kIsTimingOut = 7;
	constexpr int kGetTimeSinceLastReceived = 21;

	constexpr size_t kMaxAddress = 64;

#ifdef _WIN32
	// Does p point into the executable code of `mod`?
	bool InCode(HMODULE mod, const void* p)
	{
		auto* base = reinterpret_cast<const unsigned char*>(mod);
		auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;
		auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;
		const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
		{
			if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE))
				continue;
			const unsigned char* start = base + sec->VirtualAddress;
			if (p >= start && p < start + sec->Misc.VirtualSize)
				return true;
		}
		return false;
	}

	// On x86 the engine's methods are __thiscall; __fastcall with an unused
	// second argument passes `this` in ecx the same way. On x64 both are the
	// normal convention and the extra argument is ignored.
	using GetNetChannelInfoFn = void* (__fastcall*)(void* self, void* unused);
	using GetAddressFn = const char* (__fastcall*)(void* self, void* unused);
	using GetFloatFn = float (__fastcall*)(void* self, void* unused);
	using GetBoolFn = bool (__fastcall*)(void* self, void* unused);

	enum RawStatus { kRawOk, kRawNotConnected, kRawFailed };

	// Kept free of C++ objects so it can use structured exception handling: a
	// bad pointer anywhere in here means "can't tell", never a crash.
	RawStatus ReadAddress(HMODULE engine, void* client, char* out, size_t outSize)
	{
		__try
		{
			void** vtable = *reinterpret_cast<void***>(client);
			void* getNci = vtable[kGetNetChannelInfo];
			if (!InCode(engine, getNci))
				return kRawFailed;

			void* nci = reinterpret_cast<GetNetChannelInfoFn>(getNci)(client, nullptr);
			if (!nci)
				return kRawNotConnected;

			void** nciVtable = *reinterpret_cast<void***>(nci);
			void* getAddress = nciVtable[kGetAddress];
			if (!InCode(engine, getAddress))
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
	RawStatus ReadHealth(HMODULE engine, void* client, bool& timingOut, float& silence)
	{
		__try
		{
			void** vtable = *reinterpret_cast<void***>(client);
			void* getNci = vtable[kGetNetChannelInfo];
			if (!InCode(engine, getNci))
				return kRawFailed;
			void* nci = reinterpret_cast<GetNetChannelInfoFn>(getNci)(client, nullptr);
			if (!nci)
				return kRawNotConnected;
			void** nciVtable = *reinterpret_cast<void***>(nci);
			void* isTimingOut = nciVtable[kIsTimingOut];
			void* sinceReceived = nciVtable[kGetTimeSinceLastReceived];
			if (!InCode(engine, isTimingOut) || !InCode(engine, sinceReceived))
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

	void* EngineClient(HMODULE& engine)
	{
		engine = GetModuleHandleW(L"engine.dll");
		if (!engine)
			return nullptr;
		using CreateInterfaceFn = void* (*)(const char* name, int* code);
		auto create = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(engine, "CreateInterface"));
		return create ? create("VEngineClient015", nullptr) : nullptr;
	}
#endif

	// "a.b.c.d:port" with every part in range. Returns the "a.b.c.d" part.
	bool ParseIpv4WithPort(const char* s, std::string& ip)
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
		return digits > 0 && port > 0 && *p == '\0';
	}
}

namespace server_gate
{
	Result Check(std::string* address)
	{
#ifdef _WIN32
		HMODULE engine = GetModuleHandleW(L"engine.dll");
		if (!engine)
			return Result::Unavailable;
		using CreateInterfaceFn = void* (*)(const char* name, int* code);
		auto create = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(engine, "CreateInterface"));
		if (!create)
			return Result::Unavailable;
		void* client = create("VEngineClient015", nullptr);
		if (!client)
			return Result::Unavailable;

		char buf[kMaxAddress];
		switch (ReadAddress(engine, client, buf, sizeof(buf)))
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
		if (!ParseIpv4WithPort(buf, ip))
			return Result::OtherServer;   // "loopback" (singleplayer) and anything odd
		for (const char* allowed : kAllowedIps)
			if (ip == allowed)
				return Result::Allowed;
		return Result::OtherServer;
#else
		(void)address;
		return Result::Unavailable;
#endif
	}

	bool IsOurAddress(const std::string& address)
	{
		std::string ip;
		if (!ParseIpv4WithPort(address.c_str(), ip))
			return false;
		for (const char* allowed : kAllowedIps)
			if (ip == allowed)
				return true;
		return false;
	}

	bool NetHealth(bool& timingOut, double& silence)
	{
#ifdef _WIN32
		HMODULE engine = nullptr;
		void* client = EngineClient(engine);
		if (!client)
			return false;
		bool timing = false;
		float value = -1.0f;
		if (ReadHealth(engine, client, timing, value) != kRawOk)
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
