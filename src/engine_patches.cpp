#include "engine_patches.h"

#include <cstdint>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
#if defined(_WIN32) && defined(_WIN64)
	// client.dll, x86-64 (GMod 2026.10.26, x86-64 branch):
	//
	//   sub  rsp, 28h
	//   mov  rcx, [rip+materials]      ; IMaterialSystem*
	//   mov  rax, [rcx]
	//   call [rax+228h]                ; UncacheAllMaterials   <- replaced by a NOP
	//   mov  rcx, [rip+materials]
	//   mov  rax, [rcx]
	//   add  rsp, 28h
	//   jmp  [rax+238h]                ; CacheUsedMaterials
	//
	// Its only caller is a thunk (call it, then jump on to BuildAmmoTypes) that is
	// itself called once, at the end of client Lua start-up.
	constexpr int WILD = -1;
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
	constexpr size_t kPatternLen = sizeof(kPattern) / sizeof(kPattern[0]);
	constexpr size_t kCallOffset = 14;     // the "call [rax+228h]"
	constexpr size_t kCallLen = 6;
	const uint8_t kNop6[kCallLen] = { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 };
	const uint8_t kOriginal[kCallLen] = { 0xFF, 0x90, 0x28, 0x02, 0x00, 0x00 };

	uint8_t* g_patchedAt = nullptr;

	bool TextSection(HMODULE mod, uint8_t*& start, size_t& size)
	{
		auto* base = reinterpret_cast<uint8_t*>(mod);
		auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;
		IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
		{
			if (std::memcmp(sec->Name, ".text", 6) == 0)
			{
				start = base + sec->VirtualAddress;
				size = sec->Misc.VirtualSize;
				return true;
			}
		}
		return false;
	}

	bool Matches(const uint8_t* p)
	{
		for (size_t i = 0; i < kPatternLen; ++i)
			if (kPattern[i] != WILD && p[i] != static_cast<uint8_t>(kPattern[i]))
				return false;
		return true;
	}

	// Target of the RIP-relative operand of the 7-byte "mov rcx, [rip+x]" at p.
	const uint8_t* RipTarget(const uint8_t* p)
	{
		int32_t rel;
		std::memcpy(&rel, p + 3, sizeof(rel));
		return p + 7 + rel;
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
#if defined(_WIN32) && defined(_WIN64)
		if (g_patchedAt)
			return true;

		HMODULE client = GetModuleHandleW(L"client.dll");
		uint8_t* text = nullptr;
		size_t size = 0;
		if (!client || !TextSection(client, text, size))
		{
			why = "client.dll not found";
			return false;
		}

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
		if (RipTarget(found + 4) != RipTarget(found + 20))
		{
			why = "the two calls use different objects";
			return false;
		}

		// Exactly one caller, and it is the expected thunk:
		//   sub rsp,28h / call <found> / add rsp,28h / jmp <BuildAmmoTypes>
		std::vector<const uint8_t*> refs = DirectRefs(text, size, found);
		if (refs.size() != 1 || *refs[0] != 0xE8)
		{
			why = "unexpected callers";
			return false;
		}
		const uint8_t* thunk = refs[0] - 4;
		static const uint8_t kThunkHead[] = { 0x48, 0x83, 0xEC, 0x28 };
		static const uint8_t kThunkTail[] = { 0x48, 0x83, 0xC4, 0x28, 0xE9 };
		if (std::memcmp(thunk, kThunkHead, sizeof(kThunkHead)) != 0 ||
			std::memcmp(refs[0] + 5, kThunkTail, sizeof(kThunkTail)) != 0)
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
		return true;
#else
		why = "only the 64-bit game is supported";
		return false;
#endif
	}

	bool RestoreMaterialReload()
	{
#if defined(_WIN32) && defined(_WIN64)
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
#if defined(_WIN32) && defined(_WIN64)
		return g_patchedAt ? "patched" : "not patched";
#else
		return "unsupported";
#endif
	}
}
