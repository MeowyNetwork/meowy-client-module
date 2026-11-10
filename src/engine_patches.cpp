#include "engine_patches.h"

#include <cstdint>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include "pe_image.h"
#endif

namespace
{
#ifdef _WIN32
	constexpr int WILD = -1;

	// The routine at the end of client Lua start-up that throws away every
	// material and loads them all again. Both builds have the same shape: load
	// the material system, call UncacheAllMaterials, load it again and
	// tail-jump to CacheUsedMaterials. Its only caller is a thunk (call it,
	// then jump on to BuildAmmoTypes) that is itself called once.
#ifdef _WIN64
	// client.dll, x86-64 (GMod 2026.10.26, x86-64 branch):
	//   sub  rsp, 28h
	//   mov  rcx, [rip+materials]      ; IMaterialSystem*
	//   mov  rax, [rcx]
	//   call [rax+228h]                ; UncacheAllMaterials   <- replaced by a NOP
	//   mov  rcx, [rip+materials]
	//   mov  rax, [rcx]
	//   add  rsp, 28h
	//   jmp  [rax+238h]                ; CacheUsedMaterials
	const int kPattern[] = {
		0x48, 0x83, 0xEC, 0x28,
		0x48, 0x8B, 0x0D, WILD, WILD, WILD, WILD,
		0x48, 0x8B, 0x01,
		0xFF, 0x90, 0x28, 0x02, 0x00, 0x00,
		0x48, 0x8B, 0x0D, WILD, WILD, WILD, WILD,
		0x48, 0x8B, 0x01,
		0x48, 0x83, 0xC4, 0x28,
		0x48, 0xFF, 0xA0, 0x38, 0x02, 0x00, 0x00,
	};
	constexpr size_t kCallOffset = 14;
	const uint8_t kOriginal[] = { 0xFF, 0x90, 0x28, 0x02, 0x00, 0x00 };
	constexpr size_t kLoad1 = 4, kLoad2 = 20;   // the two "mov rcx, [rip+x]"
#else
	// client.dll, x86 (same build, 32-bit):
	//   mov  ecx, [materials]          ; IMaterialSystem*
	//   mov  eax, [ecx]
	//   call [eax+114h]                ; UncacheAllMaterials   <- replaced by a NOP
	//   mov  ecx, [materials]
	//   mov  eax, [ecx]
	//   jmp  [eax+11Ch]                ; CacheUsedMaterials
	const int kPattern[] = {
		0x8B, 0x0D, WILD, WILD, WILD, WILD,
		0x8B, 0x01,
		0xFF, 0x90, 0x14, 0x01, 0x00, 0x00,
		0x8B, 0x0D, WILD, WILD, WILD, WILD,
		0x8B, 0x01,
		0xFF, 0xA0, 0x1C, 0x01, 0x00, 0x00,
	};
	constexpr size_t kCallOffset = 8;
	const uint8_t kOriginal[] = { 0xFF, 0x90, 0x14, 0x01, 0x00, 0x00 };
	constexpr size_t kLoad1 = 0, kLoad2 = 14;   // the two "mov ecx, [x]"
#endif
	constexpr size_t kPatternLen = sizeof(kPattern) / sizeof(kPattern[0]);
	constexpr size_t kCallLen = sizeof(kOriginal);
	// "nop word ptr [eax+eax+0]": one 6-byte instruction on both architectures.
	const uint8_t kNop6[kCallLen] = { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 };

	uint8_t* g_patchedAt = nullptr;
	// The routine found by the last full scan, and the client.dll it was found
	// in. RestoreMaterialReload puts the call back after every join, and
	// scanning client.dll's whole code section again (three times) at the
	// next one cost tens of ms on the main thread while it loads. client.dll
	// stays loaded for the whole game, so the routine is where it was: the
	// next call checks it's still the same module and the same bytes, then
	// patches straight away.
	HMODULE g_validatedClient = nullptr;
	uint8_t* g_validatedFound = nullptr;

	bool Matches(const uint8_t* p)
	{
		for (size_t i = 0; i < kPatternLen; ++i)
			if (kPattern[i] != WILD && p[i] != static_cast<uint8_t>(kPattern[i]))
				return false;
		return true;
	}

	// Where the pointer load at p reads from. x64: "mov rcx, [rip+rel32]";
	// x86: "mov ecx, [abs32]".
	const uint8_t* LoadTarget(const uint8_t* p)
	{
#ifdef _WIN64
		int32_t rel;
		std::memcpy(&rel, p + 3, sizeof(rel));
		return p + 7 + rel;
#else
		uint32_t abs;
		std::memcpy(&abs, p + 2, sizeof(abs));
		return reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(abs));
#endif
	}

	// Addresses of every "call rel32" or "jmp rel32" in [text, text+size) that lands on target.
	std::vector<const uint8_t*> DirectRefs(const uint8_t* text, size_t size, const uint8_t* target)
	{
		std::vector<const uint8_t*> out;
		for (size_t i = 0; i + 5 <= size; ++i)
		{
			if (text[i] != 0xE8 && text[i] != 0xE9)
				continue;
			int32_t rel;
			std::memcpy(&rel, text + i + 1, sizeof(rel));
			if (text + i + 5 + rel == target)
				out.push_back(text + i);
		}
		return out;
	}

	// The single caller must be the thunk: [sub rsp,28h] call <routine> [add rsp,28h] jmp <next>.
	// Returns the thunk's first byte, or nullptr if the shape doesn't match.
	const uint8_t* ThunkFor(const uint8_t* callSite)
	{
		if (*callSite != 0xE8)
			return nullptr;
#ifdef _WIN64
		static const uint8_t kHead[] = { 0x48, 0x83, 0xEC, 0x28 };
		static const uint8_t kTail[] = { 0x48, 0x83, 0xC4, 0x28, 0xE9 };
		const uint8_t* thunk = callSite - sizeof(kHead);
		if (std::memcmp(thunk, kHead, sizeof(kHead)) != 0 || std::memcmp(callSite + 5, kTail, sizeof(kTail)) != 0)
			return nullptr;
		return thunk;
#else
		if (callSite[5] != 0xE9)
			return nullptr;
		return callSite;   // the call is the thunk's first instruction
#endif
	}

	bool WriteCode(uint8_t* at, const uint8_t* bytes, size_t len)
	{
		DWORD old;
		if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old))
			return false;
		std::memcpy(at, bytes, len);
		DWORD unused;
		VirtualProtect(at, len, old, &unused);
		FlushInstructionCache(GetCurrentProcess(), at, len);
		return true;
	}
#endif
}

namespace engine_patches
{
	bool SkipMaterialReload(std::string& why)
	{
#ifdef _WIN32
		if (g_patchedAt)
			return true;

		HMODULE client = GetModuleHandleW(L"client.dll");
		uint8_t* text = nullptr;
		size_t size = 0;
		if (!client || !pe_image::TextSection(client, text, size))
		{
			why = "client.dll not found";
			return false;
		}

		// Found and validated before, in this same client.dll, and still the
		// routine with its call in place: the same write as below.
		if (client == g_validatedClient && g_validatedFound && g_validatedFound >= text &&
			kPatternLen <= size && g_validatedFound <= text + (size - kPatternLen) &&
			Matches(g_validatedFound) && LoadTarget(g_validatedFound + kLoad1) == LoadTarget(g_validatedFound + kLoad2))
		{
			uint8_t* at = g_validatedFound + kCallOffset;
			if (std::memcmp(at, kOriginal, kCallLen) == 0)
			{
				if (!WriteCode(at, kNop6, kCallLen))
				{
					why = "couldn't write the patch";
					return false;
				}
				g_patchedAt = at;
				return true;
			}
		}
		g_validatedClient = nullptr;
		g_validatedFound = nullptr;

		uint8_t* found = nullptr;
		int count = 0;
		for (size_t i = 0; i + kPatternLen <= size; ++i)
		{
			if (Matches(text + i))
			{
				found = text + i;
				++count;
			}
		}
		if (count != 1)
		{
			why = count == 0 ? "pattern not found (different GMod build)" : "pattern found more than once";
			return false;
		}
		if (LoadTarget(found + kLoad1) != LoadTarget(found + kLoad2))
		{
			why = "the two calls use different objects";
			return false;
		}

		std::vector<const uint8_t*> refs = DirectRefs(text, size, found);
		if (refs.size() != 1)
		{
			why = "unexpected callers";
			return false;
		}
		const uint8_t* thunk = ThunkFor(refs[0]);
		if (!thunk)
		{
			why = "unexpected caller shape";
			return false;
		}
		if (DirectRefs(text, size, thunk).size() != 1)
		{
			why = "the caller is called from more than one place";
			return false;
		}

		uint8_t* at = found + kCallOffset;
		if (std::memcmp(at, kOriginal, kCallLen) != 0 || !WriteCode(at, kNop6, kCallLen))
		{
			why = "couldn't write the patch";
			return false;
		}
		g_patchedAt = at;
		g_validatedClient = client;
		g_validatedFound = found;
		return true;
#else
		why = "Windows only";
		return false;
#endif
	}

	bool RestoreMaterialReload()
	{
#ifdef _WIN32
		if (!g_patchedAt || !WriteCode(g_patchedAt, kOriginal, kCallLen))
			return false;
		g_patchedAt = nullptr;
		return true;
#else
		return false;
#endif
	}

	const char* MaterialReloadState()
	{
#ifdef _WIN32
		return g_patchedAt ? "patched" : "not patched";
#else
		return "unsupported";
#endif
	}
}
