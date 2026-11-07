#include "font_loads.h"

#include <atomic>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#include "pe_image.h"

namespace
{
	using LoadLibraryAFn = HMODULE(WINAPI*)(LPCSTR);
	using FreeLibraryFn = BOOL(WINAPI*)(HMODULE);

	struct Slot
	{
		const char* name;
		void** at = nullptr;       // the import table entry
		void* original = nullptr;
	};

	std::mutex g_mutex;            // install state
	Slot g_slots[2] = { { "LoadLibraryA" }, { "FreeLibrary" } };
	HMODULE g_module = nullptr;    // whose imports are hooked
	LoadLibraryAFn g_loadLibraryA = nullptr;
	FreeLibraryFn g_freeLibrary = nullptr;
	std::atomic<HMODULE> g_gdi32{ nullptr };
	std::atomic<long> g_lent{ 0 };  // gdi32 handles handed out instead of loading, not yet "freed"
	std::atomic<long long> g_saved{ 0 };

	// The last font pass: a burst of gdi32 loads less than a second apart, from
	// its first load to its last free.
	std::atomic<long long> g_passStart{ 0 }, g_passEnd{ 0 }, g_lastLoad{ 0 };
	std::atomic<long> g_passFiles{ 0 };

	long long Now()
	{
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		return t.QuadPart;
	}

	long long Frequency()
	{
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		return f.QuadPart;
	}

	HMODULE WINAPI HookLoadLibraryA(LPCSTR name)
	{
		if (name && _stricmp(name, "gdi32.dll") == 0)
		{
			const long long now = Now();
			if (now - g_lastLoad.load() > Frequency())
			{
				g_passStart = now;
				g_passFiles = 0;
			}
			g_lastLoad = now;
			++g_passFiles;
			HMODULE gdi32 = GetModuleHandleA("gdi32.dll");
			if (gdi32)
			{
				g_gdi32 = gdi32;
				++g_lent;
				++g_saved;
				return gdi32;
			}
		}
		return g_loadLibraryA(name);
	}

	BOOL WINAPI HookFreeLibrary(HMODULE module)
	{
		if (module && module == g_gdi32.load())
		{
			g_passEnd = Now();
			long n = g_lent.load();
			while (n > 0)
			{
				if (g_lent.compare_exchange_weak(n, n - 1))
					return TRUE;   // it was ours to lend: nothing was loaded
			}
		}
		return g_freeLibrary(module);
	}

	bool WriteSlot(void** at, void* value)
	{
		DWORD old;
		if (!VirtualProtect(at, sizeof(void*), PAGE_READWRITE, &old))
			return false;
		*at = value;
		DWORD unused;
		VirtualProtect(at, sizeof(void*), old, &unused);
		return true;
	}

	// The module's import table entries for LoadLibraryA and FreeLibrary,
	// imported by name from kernel32 (or its loader API set).
	bool FindSlots(HMODULE module, std::string& why)
	{
		const IMAGE_NT_HEADERS* nt = pe_image::Headers(module);
		if (!nt)
		{
			why = "not a PE image";
			return false;
		}
		auto* base = reinterpret_cast<uint8_t*>(module);
		const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!dir.VirtualAddress)
		{
			why = "no import table";
			return false;
		}
		for (Slot& s : g_slots)
			s.at = nullptr;
		for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d)
		{
			const char* dll = reinterpret_cast<const char*>(base + d->Name);
			if (_stricmp(dll, "kernel32.dll") != 0 && _strnicmp(dll, "api-ms-win-core-libraryloader-", 30) != 0)
				continue;
			if (!d->OriginalFirstThunk)
				continue;   // bound without names: can't tell the entries apart
			auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
			auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
			for (; names->u1.AddressOfData; ++names, ++iat)
			{
				if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
					continue;
				auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
				for (Slot& s : g_slots)
				{
					if (!s.at && std::strcmp(reinterpret_cast<const char*>(byName->Name), s.name) == 0)
						s.at = reinterpret_cast<void**>(&iat->u1.Function);
				}
			}
		}
		for (const Slot& s : g_slots)
		{
			if (!s.at)
			{
				why = std::string("doesn't import ") + s.name + " from kernel32 by name";
				return false;
			}
		}
		return true;
	}
}
#endif

namespace font_loads
{
	namespace detail
	{
#ifdef _WIN32
		bool InstallFor(HMODULE module, std::string& why)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (g_module == module)
				return true;
			if (g_module)
			{
				why = "already installed for another module";
				return false;
			}
			if (!FindSlots(module, why))
				return false;
			for (Slot& s : g_slots)
				s.original = *s.at;
			g_loadLibraryA = reinterpret_cast<LoadLibraryAFn>(g_slots[0].original);
			g_freeLibrary = reinterpret_cast<FreeLibraryFn>(g_slots[1].original);
			// FreeLibrary first: it only skips handles LoadLibraryA lent.
			if (!WriteSlot(g_slots[1].at, reinterpret_cast<void*>(&HookFreeLibrary)) ||
				!WriteSlot(g_slots[0].at, reinterpret_cast<void*>(&HookLoadLibraryA)))
			{
				WriteSlot(g_slots[0].at, g_slots[0].original);
				WriteSlot(g_slots[1].at, g_slots[1].original);
				why = "couldn't write the import table";
				return false;
			}
			g_module = module;
			return true;
		}
#endif
	}

	bool Install(std::string& why)
	{
#ifdef _WIN32
		HMODULE vgui = GetModuleHandleW(L"vguimatsurface.dll");
		if (!vgui)
		{
			why = "vguimatsurface.dll not loaded";
			return false;
		}
		return detail::InstallFor(vgui, why);
#else
		why = "Windows only";
		return false;
#endif
	}

	bool Uninstall()
	{
#ifdef _WIN32
		std::lock_guard<std::mutex> lock(g_mutex);
		// Not while a lent handle is still to be "freed": the real FreeLibrary
		// would then free a reference that was never taken.
		if (!g_module || g_lent.load() != 0)
			return false;
		// LoadLibraryA first, so nothing new is lent while FreeLibrary goes back.
		WriteSlot(g_slots[0].at, g_slots[0].original);
		WriteSlot(g_slots[1].at, g_slots[1].original);
		g_module = nullptr;
		return true;
#else
		return false;
#endif
	}

	std::string Describe()
	{
#ifdef _WIN32
		std::lock_guard<std::mutex> lock(g_mutex);
		std::string out = std::string(g_module ? "on" : "off") + ": " + std::to_string(g_saved.load()) + " gdi32 loads saved";
		const long long start = g_passStart.load(), end = g_passEnd.load();
		if (start && end > start)
			out += "; last font pass " + std::to_string(g_passFiles.load()) + " files in " +
				std::to_string((end - start) * 1000 / Frequency()) + " ms";
		return out;
#else
		return "unsupported";
#endif
	}
}
