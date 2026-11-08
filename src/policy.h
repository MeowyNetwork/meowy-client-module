#pragma once
// The pack's signed policy: Meowy Network's kill switch (review 2026-11-06,
// security-platform K1). A small text document, signed offline, that can turn
// the whole pack or single features off, or stop versions from running:
//
//   meowy-pack-policy 1
//   seq <decimal, 1..19 digits>            every new policy has a higher one
//   pack_enabled <0|1>
//   min_version <x.y.z>                    optional; older versions don't run
//   off <setting name>                     0..64, e.g. "off meowy_pak_cache"
//   revoked <x.y.z>                        0..64 versions that don't run
//   notice <text>                          optional, one line for players
//
// UTF-8, LF line ends. Keywords this version doesn't know are skipped (so a
// newer policy still switches off what this one understands); everything else
// is strict. The signature is ECDSA P-256 over SHA-256 of the exact bytes, as
// 128 hex digits (r || s), checked with Windows' own BCrypt against keys
// compiled in here:
//   - the release key (in Catualus's PC's TPM: not exportable, a PIN on every
//     use) may sign any policy;
//   - the ops key (meant for the backend's automatic pausing) and the recovery
//     key (meant to be kept offline, off the PC that holds the release key) are
//     plain PEM key files, so a copy can be stolen. They may only make it
//     stricter: switch the pack off, add to off or revoked, raise min_version.
//     Their seq stays below kReleaseOnlySeq and at most kMaxStepSeq above the
//     kept one, and they can't set a notice (only keep or drop the kept one).
//     So a stolen ops or recovery key can switch things off and no more, and
//     the release key can always undo it with a higher seq.
// A policy replaces the one kept only with a higher seq (an old one can't be
// replayed). It's fetched from https://meowynetwork.com/pack/v1/policy.txt
// (+ policy.sig) at start and every 30 minutes, kept in
// %LOCALAPPDATA%\MeowyClientPack\state, and checked again when read back.
// state\policy.bin ("enabled 0|1") is what the start-up stub obeys before it
// loads anything; it never touches the network or crypto.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace policy
{
	constexpr size_t kMaxBody = 8 * 1024;
	constexpr size_t kMaxList = 64;
	constexpr size_t kMaxNotice = 200;
	// Seqs from 10^15 up are the release key's alone; the ops and recovery keys
	// step at most kMaxStepSeq above the kept seq (above 0 when none is kept).
	// The same numbers are in launcher/core/src/policy.rs.
	constexpr uint64_t kReleaseOnlySeq = 1000000000000000ull;
	constexpr uint64_t kMaxStepSeq = 1000;
	constexpr size_t kSigHexLen = 128;

	enum class Key { None, Release, Ops, Recovery };

	struct Policy
	{
		uint64_t seq = 0;
		bool packEnabled = true;
		std::string minVersion;              // "" = none
		std::vector<std::string> off;        // setting names
		std::vector<std::string> revoked;    // versions
		std::string notice;                  // UTF-8, or ""
	};

	// Strict parse (see above). On failure false and *why.
	bool Parse(const std::string& body, Policy* out, const char** why);

	// Which compiled-in key signed exactly `body`, or None.
	Key SignedBy(const std::string& body, const std::string& sigHex);

	// Whether `next`, signed by `key`, may replace `cur` (haveCur: one is kept).
	bool MayReplace(const Policy& cur, bool haveCur, const Policy& next, Key key, const char** why);

	// "1.6.0" vs "1.10": numeric parts, missing ones are 0. <0, 0, >0.
	int CompareVersions(const std::string& a, const std::string& b);

	// Whether `version` may run under `p` (on, not revoked, not below min_version).
	bool VersionAllowed(const Policy& p, const std::string& version);

	// A policy offered (fetched): verified, parsed and checked against the one
	// kept in `stateDir`; kept if it may replace it. `why` says what happened.
	bool Offer(const std::wstring& stateDir, const std::string& body, const std::string& sigHex, std::string& why);

	// The policy kept in `stateDir`, verified again. False if none (or it no
	// longer verifies).
	bool Current(const std::wstring& stateDir, Policy* out);

	// Whether this build has its production keys (until then nothing is fetched).
	bool HaveKeys();

	// state\policy.bin written again from the kept policy when it differs (the
	// 1.7.0 launcher wrote it without min_version and revoked). Whether it was.
	bool HealStubFile(const std::wstring& stateDir);

	// Reads the kept policy, heals policy.bin (above), and starts the background
	// fetch: now, then every 30 minutes. Once.
	void StartFetching(const std::wstring& stateDir);

	// The policy in effect now (the kept one, or the default: all on), for Lua.
	Policy Effective();
	std::string Describe();

	namespace detail
	{
		// Tests: use these keys (64-byte X||Y each; nullptr = none) instead of the built-in ones.
		void SetKeys(const uint8_t* release, const uint8_t* ops, const uint8_t* recovery);
	}
}
