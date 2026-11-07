#pragma once
// Which copy of the pack the start-up stub loads (Windows). Shared by the stub
// (src/boot/boot.cpp) and the pack's updater (updater.h); no network, no Lua.
//
// The installer puts the pack in garrysmod\lua\bin. The updater (signed
// release documents, updater.h) puts newer versions in
//   %LOCALAPPDATA%\MeowyClientPack\bin\<version>\gmcl_meowy_<arch>.dll
// and points to them from state\ (one set per architecture):
//   pending_<arch>.txt   downloaded and verified; taken at the next start
//   current_<arch>.txt   what the stub loads
//   previous_<arch>.txt  what it loaded before (the way back)
//   bad_<arch>.txt       versions that died while loading; never loaded again
// A pointer is "version <x.y.z>\nsha256 <64 hex>\nok <n>\n"; ok counts starts
// that got through loading. Nothing is ever overwritten while loaded: a new
// version is a new folder, and switching is rewriting a small pointer file.
//
// At every start the stub, before loading anything:
//   1. promotes pending to current (previous <- current) if its file's hash matches;
//   2. if the last start died while loading a version that hasn't got through
//      twice yet, marks it bad and goes back to previous;
//   3. loads current if its hash matches and the policy (state\policy.bin,
//      policy.h) allows its version, else previous, else the installed copy.
// No pointer, no folder: the installed copy, as before updates existed.
#include <cstddef>
#include <string>
#include <vector>

namespace pack_versions
{
	constexpr size_t kMaxDll = 32 * 1024 * 1024;

	struct Pointer
	{
		std::string version;   // "1.7.0"
		std::string sha256;    // 64 lowercase hex digits
		int ok = 0;            // starts that got through loading
	};

	bool ValidVersion(const std::string& v);   // 1 to 4 numbers of up to 5 digits, dot-separated
	bool ValidSha256(const std::string& h);    // 64 lowercase hex digits
	int Compare(const std::string& a, const std::string& b);

	const wchar_t* Arch();      // L"win64" or L"win32": this build's
	std::wstring DllName();     // gmcl_meowy_<arch>.dll

	std::wstring StatePath(const std::wstring& dataDir, const wchar_t* which);   // state\<which>_<arch>.txt
	std::wstring VersionDll(const std::wstring& dataDir, const std::string& version);

	bool ReadPointer(const std::wstring& path, Pointer* out);
	bool WritePointer(const std::wstring& path, const Pointer& p);

	// SHA-256 of a whole file (at most kMaxDll), lowercase hex; "" if it can't be read.
	std::string Sha256File(const std::wstring& path);

	bool IsBad(const std::wstring& dataDir, const std::string& version);
	void MarkBad(const std::wstring& dataDir, const std::string& version);

	// What the stub may load, from state\policy.bin (written by the pack from a
	// signed policy): the pack on or off, min_version, revoked versions.
	struct StubPolicy
	{
		bool enabled = true;
		std::string minVersion;
		std::vector<std::string> revoked;
	};
	StubPolicy ReadStubPolicy(const std::wstring& dataDir);
	bool Allowed(const StubPolicy& p, const std::string& version);

	// Step 1. Returns what happened ("" for nothing to do).
	std::string Promote(const std::wstring& dataDir);

	// Step 2, for a start that died while loading `version` (from boot.lock):
	// true (and what happened) if it was reverted, false if the stub should
	// just skip the pack once as before (the installed copy, or a version that
	// has got through loading at least twice: the crash wasn't the update).
	bool RevertAfterCrash(const std::wstring& dataDir, const std::string& version, std::string& said);

	// Step 3. path: the DLL to load ("" means the installed copy), version: its
	// version ("installed" for the installed copy), why: when a pointer was passed over.
	struct Choice
	{
		std::wstring path;
		std::string version = "installed";
		std::string why;
	};
	Choice Choose(const std::wstring& dataDir);

	// After a start got through loading `version`: counts it (ok, capped).
	void NoteLoaded(const std::wstring& dataDir, const std::string& version);
}
