#include "content_advert.h"
#include "server_rep.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#endif

#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

namespace
{
	std::mutex g_mutex;
	std::vector<content_advert::Answer> g_done;
	std::set<std::string> g_asking;
	std::atomic<int> g_asked{ 0 }, g_answered{ 0 }, g_found{ 0 };
}

namespace content_advert
{
	namespace detail
	{
		bool RuleValue(const std::string& reply, const char* rule, std::string& value)
		{
			value.clear();
			if (reply.size() < 7 || reply.compare(0, 4, "\xFF\xFF\xFF\xFF") != 0 || reply[4] != 'E')
				return false;
			const size_t count = static_cast<unsigned char>(reply[5]) | (static_cast<unsigned char>(reply[6]) << 8);
			size_t i = 7;
			for (size_t n = 0; n < count; ++n)
			{
				const size_t nameEnd = reply.find('\0', i);
				if (nameEnd == std::string::npos)
					return false;
				const size_t valueEnd = reply.find('\0', nameEnd + 1);
				if (valueEnd == std::string::npos)
					return false;
				if (reply.compare(i, nameEnd - i, rule) == 0 && nameEnd - i == std::strlen(rule))
				{
					const size_t len = valueEnd - nameEnd - 1;
					if (len == 0 || len > kMaxValue)
						return false;
					for (size_t k = nameEnd + 1; k < valueEnd; ++k)
					{
						const unsigned char c = static_cast<unsigned char>(reply[k]);
						if (c < 0x20 || c > 0x7E)
							return false;
					}
					value.assign(reply, nameEnd + 1, len);
					return true;
				}
				i = valueEnd + 1;
			}
			return false;
		}
	}

	bool Ask(const std::string& address)
	{
#ifdef _WIN32
		if (!server_rep::IsPublicAddress(address))
			return false;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (g_asking.size() >= static_cast<size_t>(kMaxInFlight) || !g_asking.insert(address).second)
				return false;
		}
		++g_asked;
		std::thread([address] {
			static std::once_flag wsa;
			std::call_once(wsa, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
			Answer a;
			a.address = address;
			const std::string reply = server_rep::Exchange(address, 'V', a.answered);
			a.found = detail::RuleValue(reply, kRule, a.value);
			if (a.answered)
				++g_answered;
			if (a.found)
				++g_found;
			std::lock_guard<std::mutex> lock(g_mutex);
			g_asking.erase(address);
			g_done.push_back(std::move(a));
		}).detach();
		return true;
#else
		(void)address;
		return false;
#endif
	}

	std::vector<Answer> Take()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		std::vector<Answer> out;
		out.swap(g_done);
		return out;
	}

	std::string Describe()
	{
		return std::to_string(g_asked.load()) + " servers asked, " + std::to_string(g_answered.load()) + " answered, "
			+ std::to_string(g_found.load()) + " with an advert";
	}
}
