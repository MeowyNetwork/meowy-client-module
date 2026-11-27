#include "audio_watch.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <atomic>
#include <mutex>
#include <thread>

#include "thread_util.h"

namespace audio_watch
{
	namespace
	{
		// {a45c254e-df1c-4efd-8020-67d146a850e0}: devpkey.h's device properties,
		// defined here so nothing needs INITGUID.
		const PROPERTYKEY kFriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
		const PROPERTYKEY kEnumeratorName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 24 };

		std::atomic<unsigned> g_changes{ 0 };
		std::atomic<int> g_state{ 0 };   // 0 not started, 1 starting, 2 watching, 3 failed
		std::mutex g_startLock;

		// The endpoint notification sink: a static object, never freed (Windows
		// may call it until the process ends), so its reference count is a formality.
		class Sink final : public IMMNotificationClient
		{
		public:
			ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
			ULONG STDMETHODCALLTYPE Release() override { return 1; }
			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
			{
				if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient))
				{
					*out = static_cast<IMMNotificationClient*>(this);
					return S_OK;
				}
				*out = nullptr;
				return E_NOINTERFACE;
			}
			HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
			{
				// One per change: Windows reports each role (console, multimedia, communications).
				if (flow == eRender && role == eConsole)
					++g_changes;
				return S_OK;
			}
			HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
			HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
			HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
			HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
		};
		Sink g_sink;

		std::string Narrow(const wchar_t* w)
		{
			if (!w || !*w)
				return std::string();
			int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
			if (n <= 1)
				return std::string();
			std::string out(static_cast<size_t>(n - 1), '\0');
			WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
			return out;
		}

		std::wstring StringProp(IPropertyStore* props, const PROPERTYKEY& key)
		{
			PROPVARIANT v;
			PropVariantInit(&v);
			std::wstring out;
			if (SUCCEEDED(props->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
				out = v.pwszVal;
			PropVariantClear(&v);
			return out;
		}

		// The default endpoint for `flow`/`role`: its name, and whether it's a
		// Bluetooth hands-free one (enumerated by BTHHFENUM, or so named).
		bool DefaultEndpoint(IMMDeviceEnumerator* e, EDataFlow flow, ERole role, std::string& name, bool& handsFree)
		{
			IMMDevice* device = nullptr;
			if (FAILED(e->GetDefaultAudioEndpoint(flow, role, &device)) || !device)
				return false;
			IPropertyStore* props = nullptr;
			if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props)
			{
				const std::wstring friendly = StringProp(props, kFriendlyName);
				const std::wstring enumerator = StringProp(props, kEnumeratorName);
				name = Narrow(friendly.c_str());
				handsFree = _wcsicmp(enumerator.c_str(), L"BTHHFENUM") == 0 || friendly.find(L"Hands-Free") != std::wstring::npos;
				props->Release();
			}
			device->Release();
			return true;
		}

		void Watch()
		{
			const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			IMMDeviceEnumerator* e = nullptr;
			if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&e))) || !e
				|| FAILED(e->RegisterEndpointNotificationCallback(&g_sink)))
			{
				if (e)
					e->Release();
				if (SUCCEEDED(com))
					CoUninitialize();
				g_state = 3;
				return;
			}
			g_state = 2;
			// The enumerator stays registered (and is never released) for the rest
			// of the process, and its apartment (the MTA) must stay alive with it.
			// CoIncrementMTAUsage (Windows 8 and later; looked up, so nothing new is
			// imported) does that without a thread: this one ends. Without it, this
			// thread stays in the MTA, parked, as before.
			using IncrementFn = HRESULT(WINAPI*)(CO_MTA_USAGE_COOKIE*);
			HMODULE com32 = GetModuleHandleW(L"combase.dll");
			if (!com32)
				com32 = GetModuleHandleW(L"ole32.dll");
			IncrementFn increment = com32
				? reinterpret_cast<IncrementFn>(reinterpret_cast<void*>(GetProcAddress(com32, "CoIncrementMTAUsage"))) : nullptr;
			static CO_MTA_USAGE_COOKIE keepMta = nullptr;   // never given back
			if (increment && SUCCEEDED(increment(&keepMta)))
			{
				if (SUCCEEDED(com))
					CoUninitialize();
				return;
			}
			for (;;)
				Sleep(INFINITE);
		}
	}

	// Starts the watcher and returns at once (game thread): "starting" counts as
	// watching, and Changes() stays 0 until the watcher counts a change. Only a
	// watcher that couldn't start (or couldn't register) is false.
	bool Start()
	{
		std::lock_guard<std::mutex> lock(g_startLock);
		if (g_state == 0)
		{
			g_state = 1;
			if (!thread_util::Spawn(L"Meowy audio", Watch, thread_util::Priority::BelowNormal))
			{
				g_state = 0;
				return false;
			}
		}
		return g_state != 3;
	}

	unsigned Changes()
	{
		return g_changes.load();
	}

	Devices Read()
	{
		Devices d;
		std::thread([&d]
		{
			const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			IMMDeviceEnumerator* e = nullptr;
			if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&e))) && e)
			{
				d.ok = true;
				bool unused = false;
				DefaultEndpoint(e, eRender, eConsole, d.output, d.outputHandsFree);
				DefaultEndpoint(e, eCapture, eConsole, d.input, d.inputHandsFree);
				DefaultEndpoint(e, eCapture, eCommunications, d.commsInput, unused);
				IMMDeviceCollection* inputs = nullptr;
				UINT n = 0;
				if (SUCCEEDED(e->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &inputs)) && inputs && SUCCEEDED(inputs->GetCount(&n)))
					d.inputs = static_cast<int>(n);
				if (inputs)
					inputs->Release();
				e->Release();
			}
			if (SUCCEEDED(com))
				CoUninitialize();
		}).join();
		return d;
	}

	std::string Describe()
	{
		const int s = g_state.load();
		const char* state = s == 2 ? "following Windows' default output" : s == 3 ? "can't watch Windows' sound devices"
			: s == 1 ? "starting" : "not started";
		return std::string(state) + "; default output changed " + std::to_string(Changes()) + " times";
	}
}

#else

namespace audio_watch
{
	bool Start() { return false; }
	unsigned Changes() { return 0; }
	Devices Read() { return Devices(); }
	std::string Describe() { return "unsupported"; }
}

#endif
