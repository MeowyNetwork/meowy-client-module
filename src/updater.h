#pragma once
// The pack's signed updater (review 2026-11-06, security-platform U1).
//
// A release document, signed offline with the policy's release key (policy.h;
// the ops and recovery keys can't sign one), names one version and its files:
//
//   meowy-pack-release 1
//   seq <decimal>                       higher than any taken before (no replay, no downgrade)
//   version <x.y.z>
//   expires <unix seconds>              an old document can't hold players back for ever
//   min_stub <n>                        the start-up stub that version needs
//   rollout <0..100>                    percent of installs that take it
//   salt <8..64 hex>                    with this install's random token, picks those installs
//   file win64 <size> <sha256> <path>   path on meowynetwork.com, under /pack/v1/
//   file win32 <size> <sha256> <path>
//   notes <path>                        optional
//
// It's fetched from https://meowynetwork.com/pack/v1/release-stable.txt (+ .sig)
// a minute after start and every 6 hours. If it's newer than the running
// version, allowed by the policy, not known bad, and this install is in its
// rollout, this architecture's file is downloaded (exactly `size` bytes,
// SHA-256 checked) into %LOCALAPPDATA%\MeowyClientPack\bin\<version>\ and
// state\pending_<arch>.txt points at it. The start-up stub takes it at the next
// start (pack_versions.h): no swap while the game runs. The install token is
// random, made here, and never sent anywhere.
#include <cstdint>
#include <string>
#include <vector>

namespace updater
{
	constexpr size_t kMaxBody = 4 * 1024;

	struct File
	{
		std::string arch;      // "win64" / "win32"
		uint64_t size = 0;
		std::string sha256;
		std::string path;      // /pack/v1/...
	};

	struct Release
	{
		uint64_t seq = 0;
		std::string version;
		uint64_t expires = 0;
		int minStub = 0;
		int rollout = 0;
		std::string salt;
		std::vector<File> files;
		std::string notes;
	};

	bool Parse(const std::string& body, Release* out, const char** why);

	struct Facts
	{
		std::string running;         // this pack's version
		int stub = 0;                // the start-up stub's version (0: an old stub that can't load updates)
		uint64_t lastSeq = 0;        // the highest seq taken so far (this architecture's)
		uint64_t now = 0;            // unix seconds
		std::string token;           // this install's random token (hex)
		std::vector<std::string> bad;
		bool policyAllows = true;    // the policy allows that version (policy.h VersionAllowed)
		// This architecture's copy of the document's version is missing or doesn't
		// match its signed hash (something removed it after it was staged, such as
		// the 1.7.0 launcher, which emptied the whole bin\<version>): the same
		// document (seq == lastSeq) may be taken again. Never an older one.
		bool stagedCopyGone = false;
	};

	// Whether this install should take `r`; `why` says why not (or "update to x").
	bool ShouldTake(const Release& r, const Facts& f, std::string& why);

	// This install's place in a rollout: SHA-256(token || salt), first 4 bytes, mod 100.
	int RolloutSlot(const std::string& token, const std::string& salt);

	// The file for this build's architecture, or nullptr.
	const File* OwnFile(const Release& r);

	// Starts the background check (once): dataDir is %LOCALAPPDATA%\MeowyClientPack.
	void Start(const std::wstring& dataDir, const std::string& running, int stubVersion);
	std::string Describe();
	// A version staged this session that starts next time ("" if none), once.
	std::string TakeReadyNotice();

	namespace detail
	{
		// Staging's file steps, for the tests. bytes (checked against sha256) go to
		// bin\<version>\gmcl_meowy_<arch>.dll for this build's architecture, leaving
		// the other architecture's file in that folder alone.
		bool PutInPlace(const std::wstring& dataDir, const std::string& version, const std::string& bytes,
			const std::string& sha256, std::string& why);
		// bin\<version> folders no pointer of either architecture names, and partials, removed.
		void CleanUp(const std::wstring& dataDir);
	}
}
