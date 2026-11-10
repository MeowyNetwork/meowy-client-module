#include "notify_catalogue.h"

#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <mutex>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace
{
	// Our catalogue signing key's public half (P-256, X then Y). The private
	// half never leaves the owner's machine (DarkRP
	// tools/notify_catalogue/.catalogue_key.pem, not in git); the PEM of this
	// key is tools/notify_catalogue/catalogue_pub.pem there.
	const uint8_t kPublicKey[64] = {
		0x9c, 0x53, 0x8a, 0xea, 0x79, 0x60, 0x42, 0x7f, 0xeb, 0x3e, 0x10, 0x90, 0xef, 0x9e, 0x7e, 0x1a,
		0xa1, 0x5c, 0x99, 0xb5, 0x63, 0x15, 0xbf, 0xa3, 0x8a, 0x04, 0xd7, 0xc5, 0x5e, 0x26, 0xfb, 0x14,
		0xed, 0x48, 0x65, 0x3d, 0xe6, 0xf8, 0x9c, 0xe8, 0xde, 0x11, 0x0e, 0xe6, 0xc2, 0x7c, 0xb9, 0xe8,
		0x04, 0xb9, 0x64, 0x34, 0x9b, 0x69, 0xbb, 0xa1, 0x6c, 0xfc, 0x0a, 0x61, 0xf3, 0xed, 0x4f, 0x3d,
	};

	const char kHeader[] = "meowy-notify-catalogue 1";

	bool IsIdStart(char c) { return c >= 'a' && c <= 'z'; }
	bool IsIdChar(char c) { return IsIdStart(c) || (c >= '0' && c <= '9') || c == '_'; }

	// Decodes strict UTF-8 (no overlongs, surrogates or code points past
	// U+10FFFF) with no control characters into UTF-16. False on anything else.
	bool Utf8ToWide(const char* s, size_t n, std::wstring* out)
	{
		out->clear();
		size_t i = 0;
		while (i < n)
		{
			unsigned char c = static_cast<unsigned char>(s[i]);
			uint32_t cp;
			size_t len;
			if (c < 0x80) { cp = c; len = 1; }
			else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
			else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
			else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
			else return false;
			if (i + len > n)
				return false;
			for (size_t k = 1; k < len; ++k)
			{
				unsigned char cc = static_cast<unsigned char>(s[i + k]);
				if ((cc & 0xC0) != 0x80)
					return false;
				cp = (cp << 6) | (cc & 0x3F);
			}
			static const uint32_t kMin[] = { 0, 0, 0x80, 0x800, 0x10000 };
			if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
				return false;
			if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0))
				return false;
			if (cp >= 0x10000)
			{
				cp -= 0x10000;
				out->push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
				out->push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
			}
			else
				out->push_back(static_cast<wchar_t>(cp));
			i += len;
		}
		return true;
	}

	int HexVal(char c)
	{
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	}
}

namespace notify_catalogue
{
	bool Parse(const std::string& body, Catalogue* out, const char** why)
	{
		const char* dummy;
		if (!why)
			why = &dummy;
		if (!out)
		{
			*why = "no output";
			return false;
		}
		*out = Catalogue();
		if (body.empty() || body.size() > kMaxBody)
		{
			*why = "bad size";
			return false;
		}
		if (body.back() != '\n')
		{
			*why = "must end with a newline";
			return false;
		}
		if (std::memchr(body.data(), '\0', body.size()) || body.find('\r') != std::string::npos)
		{
			*why = "bad characters";
			return false;
		}

		size_t pos = 0;
		int lineNo = 0;
		while (pos < body.size())
		{
			size_t end = body.find('\n', pos);
			std::string line = body.substr(pos, end - pos);
			pos = end + 1;
			++lineNo;

			if (lineNo == 1)
			{
				if (line != kHeader)
				{
					*why = "bad header";
					return false;
				}
				continue;
			}
			if (lineNo == 2)
			{
				const char kV[] = "version ";
				if (line.compare(0, sizeof(kV) - 1, kV) != 0)
				{
					*why = "bad version line";
					return false;
				}
				std::string digits = line.substr(sizeof(kV) - 1);
				if (digits.empty() || digits.size() > 19 || digits[0] == '0')
				{
					*why = "bad version";
					return false;
				}
				uint64_t v = 0;
				for (char c : digits)
				{
					if (c < '0' || c > '9')
					{
						*why = "bad version";
						return false;
					}
					v = v * 10 + static_cast<uint64_t>(c - '0');
				}
				out->version = v;
				continue;
			}

			const char kK[] = "kind ";
			if (line.compare(0, sizeof(kK) - 1, kK) != 0)
			{
				*why = "bad line";
				return false;
			}
			size_t idStart = sizeof(kK) - 1;
			size_t space = line.find(' ', idStart);
			if (space == std::string::npos)
			{
				*why = "kind without text";
				return false;
			}
			std::string id = line.substr(idStart, space - idStart);
			if (id.empty() || id.size() > kMaxIdLen || !IsIdStart(id[0]))
			{
				*why = "bad kind id";
				return false;
			}
			for (char c : id)
				if (!IsIdChar(c))
				{
					*why = "bad kind id";
					return false;
				}
			std::string text = line.substr(space + 1);
			if (text.empty() || text.size() > kMaxTextBytes)
			{
				*why = "bad text length";
				return false;
			}
			Entry e;
			e.id = id;
			if (!Utf8ToWide(text.data(), text.size(), &e.text))
			{
				*why = "bad text";
				return false;
			}
			for (const Entry& prev : out->kinds)
				if (prev.id == id)
				{
					*why = "duplicate kind";
					return false;
				}
			if (out->kinds.size() >= kMaxKinds)
			{
				*why = "too many kinds";
				return false;
			}
			out->kinds.push_back(std::move(e));
		}

		if (lineNo < 3 || out->version == 0)
		{
			*why = "no kinds";
			return false;
		}
		return true;
	}

	bool VerifyWithKey(const std::string& body, const std::string& sigHex, const uint8_t pubXY[64])
	{
#ifdef _WIN32
		if (sigHex.size() != kSigHexLen || body.empty() || body.size() > kMaxBody || !pubXY)
			return false;
		uint8_t sig[64];
		for (size_t i = 0; i < 64; ++i)
		{
			int hi = HexVal(sigHex[2 * i]), lo = HexVal(sigHex[2 * i + 1]);
			if (hi < 0 || lo < 0)
				return false;
			sig[i] = static_cast<uint8_t>((hi << 4) | lo);
		}

		// The algorithm providers are opened once and kept for the rest of the
		// process (never closed: a verify can run on any thread, up to the end).
		// Opening them is the slow part, and Windows lets threads share them; the
		// hash and key objects stay per call. One that failed to open is tried
		// again next call.
		static std::mutex providerLock;
		static BCRYPT_ALG_HANDLE sha256Provider = nullptr, ecdsaProvider = nullptr;
		BCRYPT_ALG_HANDLE hashAlg = nullptr, ecdsa = nullptr;
		{
			std::lock_guard<std::mutex> lock(providerLock);
			if (!sha256Provider && BCryptOpenAlgorithmProvider(&sha256Provider, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
				sha256Provider = nullptr;
			if (!ecdsaProvider && BCryptOpenAlgorithmProvider(&ecdsaProvider, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0)
				ecdsaProvider = nullptr;
			hashAlg = sha256Provider;
			ecdsa = ecdsaProvider;
		}
		if (!hashAlg || !ecdsa)
			return false;

		bool ok = false;
		BCRYPT_HASH_HANDLE hash = nullptr;
		BCRYPT_KEY_HANDLE key = nullptr;
		uint8_t digest[32];
		do
		{
			if (BCryptCreateHash(hashAlg, &hash, nullptr, 0, nullptr, 0, 0) != 0)
				break;
			if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(body.data())),
				static_cast<ULONG>(body.size()), 0) != 0)
				break;
			if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0)
				break;

			uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
			BCRYPT_ECCKEY_BLOB* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
			header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
			header->cbKey = 32;
			std::memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), pubXY, 64);
			if (BCryptImportKeyPair(ecdsa, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob, sizeof(blob), 0) != 0)
				break;
			ok = BCryptVerifySignature(key, nullptr, digest, sizeof(digest), sig, sizeof(sig), 0) == 0;
		} while (false);

		if (key) BCryptDestroyKey(key);
		if (hash) BCryptDestroyHash(hash);
		return ok;
#else
		(void)body;
		(void)sigHex;
		(void)pubXY;
		return false;
#endif
	}

	bool Verify(const std::string& body, const std::string& sigHex)
	{
		return VerifyWithKey(body, sigHex, kPublicKey);
	}
}
