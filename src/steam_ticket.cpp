#include "steam_ticket.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
	struct Entry
	{
		bool done = false;
		std::string hex;
		std::string why;
	};

	std::mutex g_mutex;
	std::condition_variable g_cv;
	std::map<uint32_t, Entry> g_tickets;   // asked for and not waited for yet
	std::vector<uint32_t> g_cancel;        // released, for the next Pump
	constexpr int kResultOK = 1;            // EResult k_EResultOK
}

namespace steam_ticket
{
	namespace detail
	{
		std::string Hex(const uint8_t* bytes, size_t n)
		{
			static const char digits[] = "0123456789abcdef";
			std::string out;
			out.reserve(n * 2);
			for (size_t i = 0; i < n; ++i)
			{
				out += digits[bytes[i] >> 4];
				out += digits[bytes[i] & 15];
			}
			return out;
		}

		std::string WithTicket(const std::string& json, const std::string& hex)
		{
			if (hex.empty() || hex.size() % 2 != 0 || hex.size() > static_cast<size_t>(kMaxTicket) * 2)
				return json;
			for (char c : hex)
				if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
					return json;
			size_t end = json.find_last_not_of(" \t\r\n");
			if (json.empty() || json[0] != '{' || end == std::string::npos || json[end] != '}')
				return json;
			// "{}" takes no comma; anything else already holds a member.
			size_t inner = json.find_last_not_of(" \t\r\n", end - 1);
			const bool empty = inner == 0;
			return json.substr(0, end) + (empty ? "" : ",") + "\"ticket\":\"" + hex + "\"" + json.substr(end);
		}

		void Arrived(uint32_t handle, int result, const uint8_t* ticket, int size)
		{
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				auto it = g_tickets.find(handle);
				if (it == g_tickets.end())
					return;   // not one of ours, or its report stopped waiting
				Entry& e = it->second;
				e.done = true;
				if (result == kResultOK && ticket && size > 0 && size <= kMaxTicket)
					e.hex = Hex(ticket, static_cast<size_t>(size));
				else
					e.why = "Steam answered " + std::to_string(result);
			}
			g_cv.notify_all();
		}

		void Expect(uint32_t handle)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_tickets[handle] = Entry{};
		}

		size_t PendingCancels()
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			return g_cancel.size();
		}
	}

	std::string Wait(uint32_t handle, int ms, std::string* why)
	{
		std::unique_lock<std::mutex> lock(g_mutex);
		auto it = g_tickets.find(handle);
		if (it == g_tickets.end())
		{
			if (why)
				*why = "not asked for";
			return "";
		}
		// std::map's iterators stay valid while other entries come and go.
		g_cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return it->second.done; });
		const Entry e = it->second;
		g_tickets.erase(it);
		if (why)
			*why = e.done ? e.why : "Steam didn't answer in time";
		return e.done ? e.hex : "";
	}

	void Release(uint32_t handle)
	{
		if (!handle)
			return;
		std::lock_guard<std::mutex> lock(g_mutex);
		g_cancel.push_back(handle);
	}
}

#ifdef _WIN32
namespace
{
	// --- Steam's flat API (steam_api_flat.h), __cdecl on 32-bit ---------------------------
	using AccessorFn = void* (__cdecl*)();
	using GetTicketFn = uint32_t (__cdecl*)(void* user, const char* identity);
	using CancelFn = void (__cdecl*)(void* user, uint32_t handle);
	class CCallbackBase;
	using RegisterCallbackFn = void (__cdecl*)(CCallbackBase* cb, int id);

#ifdef _WIN64
	constexpr const wchar_t* kSteamApi = L"steam_api64.dll";
#else
	constexpr const wchar_t* kSteamApi = L"steam_api.dll";
#endif

	struct Api
	{
		AccessorFn user = nullptr;
		GetTicketFn getTicket = nullptr;
		CancelFn cancel = nullptr;
		RegisterCallbackFn registerCallback = nullptr;
	};
	Api g_api;
	bool g_resolved = false, g_broken = false, g_registered = false;

	// GetTicketForWebApiResponse_t (isteamuser.h, k_iSteamUserCallbacks + 68).
	// Windows builds of steam_api pack callback structs to 8 bytes.
#pragma pack(push, 8)
	struct TicketResponse
	{
		uint32_t handle;
		int32_t result;
		int32_t size;
		uint8_t ticket[steam_ticket::kMaxTicket];
	};
#pragma pack(pop)
	constexpr int kTicketResponse = 168;
	static_assert(sizeof(TicketResponse) == 12 + steam_ticket::kMaxTicket, "Steam's layout");

	// Steam's CCallbackBase (steam_api_common.h), as party_steam.cpp declares it.
	class CCallbackBase
	{
	public:
		CCallbackBase() : m_nCallbackFlags(0), m_iCallback(0) {}
		virtual void Run(void* pvParam) = 0;
		virtual void Run(void* pvParam, bool bIOFailure, uint64_t hSteamAPICall) = 0;
		int GetICallback() { return m_iCallback; }
		virtual int GetCallbackSizeBytes() = 0;

	protected:
		uint8_t m_nCallbackFlags;
		int m_iCallback;
	};

	class TicketCallback final : public CCallbackBase
	{
	public:
		void Run(void* p) override
		{
			const TicketResponse* r = static_cast<const TicketResponse*>(p);
			if (r)
				steam_ticket::detail::Arrived(r->handle, r->result, r->ticket, r->size);
		}
		void Run(void* p, bool, uint64_t) override { Run(p); }
		int GetCallbackSizeBytes() override { return sizeof(TicketResponse); }
	};
	TicketCallback g_callback;

	int OnFault(unsigned long code)
	{
		return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
	}

	// Runs `f` (plain calls into Steam) with a fault turning tickets off for the
	// session instead of taking the game down.
	template <typename F>
	bool Guard(F& f)
	{
		__try
		{
			f();
			return true;
		}
		__except (OnFault(GetExceptionCode()))
		{
			g_broken = true;
			return false;
		}
	}

	template <typename T>
	bool Find(HMODULE dll, T& out, const char* name)
	{
		out = reinterpret_cast<T>(GetProcAddress(dll, name));
		return out != nullptr;
	}

	// ISteamUser, with the callback registered once; nullptr and why if not.
	void* User(std::string& why)
	{
		if (g_broken)
		{
			why = "Steam faulted earlier";
			return nullptr;
		}
		if (!g_resolved)
		{
			HMODULE dll = GetModuleHandleW(kSteamApi);
			if (!dll)
			{
				why = "Steam's API isn't loaded";
				return nullptr;
			}
			if (!(Find(dll, g_api.user, "SteamAPI_SteamUser_v023") && Find(dll, g_api.getTicket, "SteamAPI_ISteamUser_GetAuthTicketForWebApi")
				&& Find(dll, g_api.cancel, "SteamAPI_ISteamUser_CancelAuthTicket") && Find(dll, g_api.registerCallback, "SteamAPI_RegisterCallback")))
			{
				g_broken = true;
				why = "this steam_api lacks GetAuthTicketForWebApi";
				return nullptr;
			}
			g_resolved = true;
		}
		void* user = nullptr;
		auto f = [&] {
			user = g_api.user();
			if (user && !g_registered)
			{
				g_api.registerCallback(&g_callback, kTicketResponse);
				g_registered = true;
			}
		};
		if (!Guard(f))
		{
			why = "Steam faulted";
			return nullptr;
		}
		if (!user)
			why = "the Steam client doesn't offer ISteamUser";
		return user;
	}
}

namespace steam_ticket
{
	uint32_t Request(std::string* why)
	{
		std::string reason;
		void* user = User(reason);
		uint32_t handle = 0;
		if (user)
		{
			auto f = [&] { handle = g_api.getTicket(user, kIdentity); };
			if (!Guard(f))
				reason = "Steam faulted";
			else if (!handle)
				reason = "Steam refused a ticket";
		}
		if (handle)
			detail::Expect(handle);
		else if (why)
			*why = reason;
		return handle;
	}

	void Pump()
	{
		std::vector<uint32_t> cancel;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (g_cancel.empty())
				return;
			cancel.swap(g_cancel);
		}
		std::string why;
		void* user = User(why);
		if (!user)
			return;
		for (uint32_t h : cancel)
		{
			auto f = [&] { g_api.cancel(user, h); };
			if (!Guard(f))
				return;
		}
	}
}
#else
namespace steam_ticket
{
	uint32_t Request(std::string* why)
	{
		if (why)
			*why = "Windows only";
		return 0;
	}

	void Pump()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_cancel.clear();
	}
}
#endif
