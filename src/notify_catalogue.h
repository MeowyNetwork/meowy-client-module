#pragma once
// The signed notification catalogue (1.6+): how notification kinds added on
// the server after a pack release reach the pack, without letting anyone but
// us choose the words a toast shows.
//
// The server's client Lua hands the pack a catalogue body and a signature
// (meowy.NotifyCatalogue). The body lists kinds with their lock-screen-safe
// text; it is built and signed offline by DarkRP's
// tools/notify_catalogue/build.py with our private key. The pack accepts it
// only when:
//   1. the signature verifies against the public key compiled in here
//      (ECDSA P-256 over SHA-256 of the exact body bytes, via Windows' own
//      BCrypt; no third-party crypto code);
//   2. the body parses strictly (format below);
//   3. its version is not older than the one already accepted (a captured
//      old catalogue can't be replayed to bring back old words).
// The accepted catalogue is kept in %LOCALAPPDATA%\MeowyClientPack and checked
// again when read back, so a file edited on disk is ignored. Without one, the
// list compiled into notify.cpp applies.
//
// Body (UTF-8, LF line ends, ends with LF):
//   meowy-notify-catalogue 1
//   version <decimal, 1..19 digits, no leading zero>
//   kind <id> <text>          (1..64 lines)
// id: [a-z][a-z0-9_]{0,31}; text: 1..160 bytes of valid UTF-8, no control
// characters. Kind ids are unique.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace notify_catalogue
{
	constexpr size_t kMaxBody = 16 * 1024;
	constexpr size_t kMaxKinds = 64;   // notify::RateLimiter tracks at most 64
	constexpr size_t kMaxIdLen = 32;
	constexpr size_t kMaxTextBytes = 160;
	constexpr size_t kSigHexLen = 128;  // r || s, 32 bytes each, as hex

	struct Entry
	{
		std::string id;
		std::wstring text;
	};

	struct Catalogue
	{
		uint64_t version = 0;
		std::vector<Entry> kinds;
	};

	// Strict parse of a body. On failure returns false and sets *why.
	bool Parse(const std::string& body, Catalogue* out, const char** why);

	// Whether `sigHex` (128 lowercase or uppercase hex digits) is our
	// signature of exactly `body`. Always false off Windows.
	bool Verify(const std::string& body, const std::string& sigHex);

	// Verify against another key: `pubXY` is the 64-byte uncompressed point
	// without its 0x04 prefix. For tests/notify_catalogue_smoke.cpp.
	bool VerifyWithKey(const std::string& body, const std::string& sigHex, const uint8_t pubXY[64]);
}
