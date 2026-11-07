// The font pass's gdi32 loads, no game: this program hooks its own imports
// the way the pack hooks vguimatsurface.dll's, then does what
// AddCustomFontFile does, twice, with a real font file.
//   font_loads_smoke <scratch folder>
// Exit code 0 when every check passes.

#include "../src/font_loads.h"

#include <cstdio>
#include <string>

static int g_failures = 0, g_count = 0;

static void Check(const char* name, bool ok)
{
	++g_count;
	if (!ok)
	{
		++g_failures;
		std::printf("FAIL %s\n", name);
	}
}

using AddFontResourceExAFn = int(WINAPI*)(LPCSTR, DWORD, PVOID);

// vguimatsurface's sequence for one font file.
static int AddCustomFontFile(const std::string& path, HMODULE* loaded = nullptr)
{
	HMODULE gdi32 = LoadLibraryA("gdi32.dll");
	if (!gdi32)
		return -1;
	if (loaded)
		*loaded = gdi32;
	auto add = reinterpret_cast<AddFontResourceExAFn>(GetProcAddress(gdi32, "AddFontResourceExA"));
	const int r = add ? add(path.c_str(), FR_PRIVATE, nullptr) : -2;
	FreeLibrary(gdi32);
	return r;
}

static bool Contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

int main(int argc, char** argv)
{
	const std::string scratch = argc > 1 ? argv[1] : ".";
	char windir[MAX_PATH];
	GetWindowsDirectoryA(windir, MAX_PATH);
	const std::string font = scratch + "\\font_loads_smoke.ttf";
	Check("a font to copy", CopyFileA((std::string(windir) + "\\Fonts\\arial.ttf").c_str(), font.c_str(), FALSE) != 0);

	HMODULE gdi32 = GetModuleHandleA("gdi32.dll");
	Check("gdi32 loaded", gdi32 != nullptr);

	std::string why;
	Check("hooks its own imports", font_loads::detail::InstallFor(GetModuleHandleW(nullptr), why));
	if (!why.empty())
		std::printf("  %s\n", why.c_str());
	Check("installing again is fine", font_loads::detail::InstallFor(GetModuleHandleW(nullptr), why));

	HMODULE handed = nullptr;
	const int first = AddCustomFontFile(font, &handed);
	Check("registration goes to GDI", first > 0);
	Check("the loaded gdi32 is handed out", handed == gdi32);
	Check("registering again too", AddCustomFontFile(font) > 0);
	const std::string d = font_loads::Describe();
	Check("two loads saved", Contains(d, "on: 2 gdi32 loads saved"));
	Check("a pass of 2 files timed", Contains(d, "last font pass 2 files in "));
	Check("gdi32 still loaded", GetModuleHandleA("gdi32.dll") == gdi32);

	// Other libraries pass through: loaded and freed for real.
	HMODULE winmm = LoadLibraryA("winmm.dll");
	Check("other libraries load", winmm != nullptr && GetProcAddress(winmm, "timeGetTime") != nullptr);
	Check("and free", FreeLibrary(winmm) != 0);

	// A lent handle not yet freed blocks uninstalling.
	HMODULE lent = LoadLibraryA("gdi32.dll");
	Check("lent", lent == gdi32);
	Check("can't uninstall while lent", !font_loads::Uninstall());
	FreeLibrary(lent);
	Check("uninstalls", font_loads::Uninstall());
	Check("uninstalling twice is refused", !font_loads::Uninstall());
	Check("after uninstall loads are real", AddCustomFontFile(font) > 0 && Contains(font_loads::Describe(), "off: 3 gdi32 loads saved"));
	Check("gdi32 still loaded at the end", GetModuleHandleA("gdi32.dll") == gdi32);

	while (RemoveFontResourceExA(font.c_str(), FR_PRIVATE, nullptr)) {}
	DeleteFileA(font.c_str());
	std::printf("%d/%d checks passed\n", g_count - g_failures, g_count);
	return g_failures == 0 ? 0 : 1;
}
