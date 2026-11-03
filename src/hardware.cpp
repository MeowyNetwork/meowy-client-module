#include "hardware.h"

#ifndef _WIN32

namespace hardware
{
	Info Get() { return Info(); }
}

#else

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include <intrin.h>
#pragma comment(lib, "dxgi.lib")

#include <cstring>
#include <string>

namespace
{
	std::string Trim(std::string s)
	{
		size_t a = s.find_first_not_of(' ');
		size_t b = s.find_last_not_of(' ');
		return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
	}

	std::string CpuBrand()
	{
		int regs[4] = { 0 };
		__cpuid(regs, 0x80000000);
		if (static_cast<unsigned>(regs[0]) < 0x80000004)
			return std::string();
		char brand[49] = { 0 };
		for (int i = 0; i < 3; ++i)
		{
			__cpuid(regs, 0x80000002 + i);
			std::memcpy(brand + i * 16, regs, sizeof(regs));
		}
		return Trim(brand);
	}

	std::string Narrow(const wchar_t* s)
	{
		int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
		if (n <= 1)
			return std::string();
		std::string out(static_cast<size_t>(n - 1), '\0');
		WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr);
		return out;
	}

	std::string WindowsVersion()
	{
		// GetVersionEx lies to unmanifested processes; RtlGetVersion doesn't.
		using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
		HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
		auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
		OSVERSIONINFOW v{};
		v.dwOSVersionInfoSize = sizeof(v);
		if (!fn || fn(&v) != 0)
			return "Windows";
		return "Windows " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." +
			std::to_string(v.dwBuildNumber);
	}
}

namespace hardware
{
	Info Get()
	{
		Info info;
		info.cpu = CpuBrand();

		SYSTEM_INFO si{};
		GetNativeSystemInfo(&si);
		info.threads = si.dwNumberOfProcessors;

		MEMORYSTATUSEX mem{};
		mem.dwLength = sizeof(mem);
		if (GlobalMemoryStatusEx(&mem))
			info.ramMb = mem.ullTotalPhys / (1024 * 1024);

		IDXGIFactory1* factory = nullptr;
		if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
		{
			IDXGIAdapter1* adapter = nullptr;
			for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
			{
				DXGI_ADAPTER_DESC1 desc{};
				if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
				{
					unsigned long long vram = desc.DedicatedVideoMemory / (1024 * 1024);
					if (vram >= info.vramMb)
					{
						info.vramMb = vram;
						info.gpu = Narrow(desc.Description);
					}
				}
				adapter->Release();
			}
			factory->Release();
		}

		info.os = WindowsVersion();
		return info;
	}
}

#endif
