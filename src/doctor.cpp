#include "doctor.h"

#ifndef _WIN32

namespace doctor
{
	Facts Gather() { Facts f; f.arch = sizeof(void*) == 8 ? "x64" : "x86"; return f; }
	bool Open(const std::string&) { return false; }
}

#else

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>   // NTSTATUS, for d3dkmthk.h
#include <d3dkmthk.h>
#include <dxgi1_6.h>
#include <shellapi.h>
#pragma comment(lib, "dxgi.lib")

#include <atomic>
#include <cstring>
#include <memory>

namespace
{
	// One adapter as Windows ranks it. A GPU can show up under more than one
	// LUID (seen on a hybrid laptop: the dGPU twice, same name and memory, same
	// counters); those are merged and their GPU time counted once.
	struct Found
	{
		std::vector<LUID> luids;
		doctor::Adapter info;
	};

	bool Has(const Found& f, const LUID& luid)
	{
		for (const LUID& l : f.luids)
			if (l.LowPart == luid.LowPart && l.HighPart == luid.HighPart)
				return true;
		return false;
	}

	std::string Utf8(const wchar_t* w)
	{
		int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
		if (n <= 1)
			return std::string();
		std::string s(static_cast<size_t>(n - 1), '\0');
		WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
		return s.size() > 128 ? s.substr(0, 128) : s;
	}

	std::vector<Found> EnumerateAdapters()
	{
		std::vector<Found> out;
		IDXGIFactory1* factory = nullptr;
		if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
			return out;
		IDXGIAdapter1* adapter = nullptr;
		for (UINT i = 0; i < 16 && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
		{
			DXGI_ADAPTER_DESC1 desc{};
			if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
			{
				std::string name = Utf8(desc.Description);
				unsigned long long vram = static_cast<unsigned long long>(desc.DedicatedVideoMemory) / (1024 * 1024);
				Found* same = nullptr;
				for (Found& f : out)
					if (Has(f, desc.AdapterLuid) || (f.info.name == name && f.info.vramMb == vram))
						same = &f;
				if (same)
				{
					if (!Has(*same, desc.AdapterLuid))
						same->luids.push_back(desc.AdapterLuid);
				}
				else
				{
					Found f;
					f.luids.push_back(desc.AdapterLuid);
					f.info.name = name;
					f.info.vramMb = vram;
					out.push_back(f);
				}
			}
			adapter->Release();
		}

		// Windows' own ranking (Graphics settings uses the same): DXGI 1.6, Windows 10 1803+.
		IDXGIFactory6* f6 = nullptr;
		if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&f6))))
		{
			for (int pref = 0; pref < 2; ++pref)
			{
				DXGI_GPU_PREFERENCE p = pref == 0 ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE : DXGI_GPU_PREFERENCE_MINIMUM_POWER;
				int rank = 0;
				for (UINT i = 0; i < 16; ++i)
				{
					IDXGIAdapter1* a = nullptr;
					if (FAILED(f6->EnumAdapterByGpuPreference(i, p, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&a))))
						break;
					DXGI_ADAPTER_DESC1 desc{};
					if (SUCCEEDED(a->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
					{
						for (Found& f : out)
						{
							int& slot = pref == 0 ? f.info.perfRank : f.info.powerRank;
							if (Has(f, desc.AdapterLuid) && slot == 0)
								slot = ++rank;
						}
					}
					a->Release();
				}
			}
			f6->Release();
		}
		factory->Release();
		return out;
	}

	// D3DKMTQueryStatistics from gdi32, looked up at run time. The struct is
	// given a generous buffer: a newer kernel's version of it may be larger.
	using QueryStatisticsFn = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYSTATISTICS*);

	bool Query(QueryStatisticsFn fn, D3DKMT_QUERYSTATISTICS_TYPE type, const LUID& luid, ULONG node, D3DKMT_QUERYSTATISTICS& out)
	{
		alignas(16) static unsigned char buffer[sizeof(D3DKMT_QUERYSTATISTICS) + 4096];
		std::memset(buffer, 0, sizeof(buffer));
		auto* q = reinterpret_cast<D3DKMT_QUERYSTATISTICS*>(buffer);
		q->Type = type;
		q->AdapterLuid = luid;
		q->hProcess = GetCurrentProcess();
		q->QueryProcessNode.NodeId = node;
		if (fn(q) < 0)
			return false;
		std::memcpy(&out, q, sizeof(out));
		return true;
	}

	// GPU time this process has used on each adapter, summed over its engines.
	bool GpuTime(std::vector<Found>& adapters)
	{
		HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
		if (!gdi)
			gdi = LoadLibraryW(L"gdi32.dll");
		auto fn = gdi ? reinterpret_cast<QueryStatisticsFn>(GetProcAddress(gdi, "D3DKMTQueryStatistics")) : nullptr;
		if (!fn)
			return false;
		std::vector<double> time(adapters.size(), 0);
		double total = 0;
		bool any = false;
		for (size_t i = 0; i < adapters.size(); ++i)
		{
			for (const LUID& luid : adapters[i].luids)
			{
				D3DKMT_QUERYSTATISTICS q;
				if (!Query(fn, D3DKMT_QUERYSTATISTICS_ADAPTER, luid, 0, q))
					continue;
				double sum = 0;
				ULONG nodes = q.QueryResult.AdapterInformation.NodeCount;
				for (ULONG n = 0; n < nodes && n < 64; ++n)
				{
					if (!Query(fn, D3DKMT_QUERYSTATISTICS_PROCESS_NODE, luid, n, q))
						continue;
					any = true;
					double t = static_cast<double>(q.QueryResult.ProcessNodeInformation.RunningTime.QuadPart);
					if (t > 0)
						sum += t;
				}
				if (sum > time[i])
					time[i] = sum;   // the same GPU under another LUID: counted once
			}
			total += time[i];
		}
		if (!any || total <= 0)
			return false;
		for (size_t i = 0; i < adapters.size(); ++i)
			adapters[i].info.share = time[i] / total;
		return true;
	}

	// Windows' effective power mode: the callback gets the current mode right
	// after registering; then it's unregistered.
	std::atomic<int> g_mode{ -1 };
	HANDLE g_modeEvent = nullptr;

	void WINAPI ModeCallback(int mode, void*)
	{
		g_mode = mode;
		if (g_modeEvent)
			SetEvent(g_modeEvent);
	}

	std::string PowerMode()
	{
		HMODULE pp = LoadLibraryW(L"powrprof.dll");
		if (!pp)
			return std::string();
		using Callback = void(WINAPI*)(int, void*);
		using RegisterFn = HRESULT(WINAPI*)(ULONG, Callback, void*, void**);
		using UnregisterFn = HRESULT(WINAPI*)(void*);
		auto reg = reinterpret_cast<RegisterFn>(GetProcAddress(pp, "PowerRegisterForEffectivePowerModeNotifications"));
		auto unreg = reinterpret_cast<UnregisterFn>(GetProcAddress(pp, "PowerUnregisterFromEffectivePowerModeNotifications"));
		std::string out;
		if (reg && unreg)
		{
			if (!g_modeEvent)
				g_modeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			ResetEvent(g_modeEvent);
			g_mode = -1;
			void* handle = nullptr;
			// Version 2 (Windows 10 1903+) knows game mode; 1 is the fallback.
			if (SUCCEEDED(reg(2, ModeCallback, nullptr, &handle)) || SUCCEEDED(reg(1, ModeCallback, nullptr, &handle)))
			{
				WaitForSingleObject(g_modeEvent, 300);
				unreg(handle);
			}
			static const char* const names[] = { "battery_saver", "better_battery", "balanced",
				"high_performance", "max_performance", "game_mode", "mixed_reality" };
			int m = g_mode;
			if (m >= 0 && m < 7)
				out = names[m];
		}
		FreeLibrary(pp);
		return out;
	}
}

namespace doctor
{
	Facts Gather()
	{
		Facts f;
		f.arch = sizeof(void*) == 8 ? "x64" : "x86";
		try
		{
			std::vector<Found> found = EnumerateAdapters();
			f.gpuTimeKnown = GpuTime(found);
			for (const Found& a : found)
				f.adapters.push_back(a.info);
		}
		catch (...)
		{
		}

		SYSTEM_POWER_STATUS ps{};
		if (GetSystemPowerStatus(&ps))
		{
			if (ps.ACLineStatus == 0) f.onBattery = 1;
			else if (ps.ACLineStatus == 1) f.onBattery = 0;
			f.batterySaver = ps.SystemStatusFlag == 1 ? 1 : 0;
		}
		f.powerMode = PowerMode();
		return f;
	}

	bool Open(const std::string& id)
	{
		const wchar_t* url = nullptr;
		if (id == "graphics_settings")
			url = L"ms-settings:display-advancedgraphics";
		else if (id == "branch_guide")
			url = L"https://wiki.facepunch.com/gmod/Dev_Branch";
		if (!url)
			return false;
		return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL)) > 32;
	}
}

#endif
