// PC doctor's native facts on this PC, no game:
//   doctor_smoke
// Prints the adapters, Windows' ranking, the power state and which adapter
// this process's GPU time is on. To exercise the per-process GPU time, it first
// renders a little with Direct3D 11 on Windows' default adapter, so that
// adapter should come out with (nearly) all of it.
#include "../src/doctor.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#include <cstdio>
#include <cstring>

// doctor_smoke power: render on Windows' "power saving" adapter instead (on a
// hybrid laptop, the integrated GPU): the doctor should then name that one.
int main(int argc, char** argv)
{
	IDXGIAdapter1* chosen = nullptr;
	if (argc > 1 && std::strcmp(argv[1], "power") == 0)
	{
		IDXGIFactory6* f6 = nullptr;
		if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&f6))))
		{
			f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_MINIMUM_POWER, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&chosen));
			f6->Release();
		}
	}
	ID3D11Device* dev = nullptr;
	ID3D11DeviceContext* ctx = nullptr;
	if (SUCCEEDED(D3D11CreateDevice(chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
	{
		D3D11_TEXTURE2D_DESC td{};
		td.Width = td.Height = 2048;
		td.MipLevels = td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.BindFlags = D3D11_BIND_RENDER_TARGET;
		ID3D11Texture2D* tex = nullptr;
		ID3D11RenderTargetView* rtv = nullptr;
		if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &tex)) && SUCCEEDED(dev->CreateRenderTargetView(tex, nullptr, &rtv)))
		{
			const float color[4] = { 0.2f, 0.3f, 0.4f, 1.0f };
			for (int i = 0; i < 2000; ++i)
				ctx->ClearRenderTargetView(rtv, color);
			ctx->Flush();
			Sleep(500);
		}
		if (rtv) rtv->Release();
		if (tex) tex->Release();
	}
	else
	{
		std::printf("(no D3D11 device: GPU time will be unknown)\n");
	}

	doctor::Facts f = doctor::Gather();
	std::printf("arch %s, gpu time known %d, on battery %d, battery saver %d, power mode '%s'\n",
		f.arch.c_str(), f.gpuTimeKnown ? 1 : 0, f.onBattery, f.batterySaver, f.powerMode.c_str());
	for (const doctor::Adapter& a : f.adapters)
		std::printf("  %s: %llu MB, perf rank %d, power rank %d, share %.3f\n",
			a.name.c_str(), a.vramMb, a.perfRank, a.powerRank, a.share);
	if (chosen) chosen->Release();
	if (ctx) ctx->Release();
	if (dev) dev->Release();
	return 0;
}
