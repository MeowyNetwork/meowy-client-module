// gmsv_ldr: the only file the observer keeps in garrysmod/lua/bin.
//
// Any server's client Lua can list and read lua/bin, so the observer's real
// module lives outside the game's folders (out of reach of every file path
// GMod gives Lua) and this stub loads it:
//
//   Linux:   $HOME/.meowy-observer/bin/meowy_observer_<arch>.so
//   Windows: %LOCALAPPDATA%\MeowyObserver\bin\meowy_observer_<arch>.dll
//
// (or $MEOWY_OBSERVER_DIR/bin when set). The observer itself is disclosed
// publicly (observer/OBSERVER.md): accounts, address, what it measures.
//
// It hands GMod's open/close calls straight to the real module.

#include "GarrysMod/Lua/Interface.h"

#include <cstdlib>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{
	using EntryFn = int (*)(lua_State*);

#if defined(_WIN64)
	const char* const kArch = "win64";
#elif defined(_WIN32)
	const char* const kArch = "win32";
#elif defined(__x86_64__)
	const char* const kArch = "linux64";
#else
	const char* const kArch = "linux";
#endif

	std::string RealModule()
	{
		std::string dir;
#ifdef _WIN32
		char buf[MAX_PATH];
		DWORD n = GetEnvironmentVariableA("MEOWY_OBSERVER_DIR", buf, MAX_PATH);
		if (n && n < MAX_PATH)
			dir = buf;
		else if ((n = GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH)) && n < MAX_PATH)
			dir = std::string(buf) + "\\MeowyObserver";
		return dir.empty() ? dir : dir + "\\bin\\meowy_observer_" + kArch + ".dll";
#else
		const char* d = std::getenv("MEOWY_OBSERVER_DIR");
		if (d && *d)
			dir = d;
		else if (const char* home = std::getenv("HOME"))
			dir = std::string(home) + "/.meowy-observer";
		return dir.empty() ? dir : dir + "/bin/meowy_observer_" + kArch + ".so";
#endif
	}

	EntryFn g_close = nullptr;

	EntryFn Entry(void* lib, const char* name)
	{
#ifdef _WIN32
		return reinterpret_cast<EntryFn>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
		return reinterpret_cast<EntryFn>(dlsym(lib, name));
#endif
	}
}

// Written out rather than with GMOD_MODULE_OPEN/CLOSE, which only hand the
// body an ILuaBase: the real module's entry points take the lua_State.
GMOD_DLL_EXPORT int gmod13_open(lua_State* L)
{
	std::string path = RealModule();
	if (path.empty())
		return 0;
#ifdef _WIN32
	void* lib = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
	void* lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
	if (!lib)
		return 0;
	EntryFn open = Entry(lib, "gmod13_open");
	g_close = Entry(lib, "gmod13_close");
	return open ? open(L) : 0;
}

GMOD_DLL_EXPORT int gmod13_close(lua_State* L)
{
	return g_close ? g_close(L) : 0;
}
