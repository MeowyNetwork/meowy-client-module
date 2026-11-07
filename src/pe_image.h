#pragma once

// Reading a loaded module's PE headers: where its code is and how big it is.
// Windows only.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pe_image
{
	inline const IMAGE_NT_HEADERS* Headers(HMODULE mod)
	{
		if (!mod)
			return nullptr;
		auto* base = reinterpret_cast<const uint8_t*>(mod);
		auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return nullptr;
		auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
		return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
	}

	// The module's .text section.
	inline bool TextSection(HMODULE mod, uint8_t*& start, size_t& size)
	{
		const IMAGE_NT_HEADERS* nt = Headers(mod);
		if (!nt)
			return false;
		const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
		{
			if (std::memcmp(sec->Name, ".text", 6) == 0)
			{
				start = reinterpret_cast<uint8_t*>(mod) + sec->VirtualAddress;
				size = sec->Misc.VirtualSize;
				return true;
			}
		}
		return false;
	}

	// The whole mapped image: [base, base + size).
	inline bool ImageRange(HMODULE mod, uintptr_t& base, size_t& size)
	{
		const IMAGE_NT_HEADERS* nt = Headers(mod);
		if (!nt)
			return false;
		base = reinterpret_cast<uintptr_t>(mod);
		size = nt->OptionalHeader.SizeOfImage;
		return true;
	}
}
#endif
