#include "loading_webview.h"

#ifdef _WIN32
#include <shlwapi.h>
#include <wrl.h>
#include "WebView2.h"

#include <cwchar>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace
{
	HWND g_parent = nullptr;
	ComPtr<ICoreWebView2Controller> g_controller;
	ComPtr<ICoreWebView2> g_view;
	bool g_ready = false, g_failed = false;
	char g_action = 0;
	std::wstring g_origin;   // "https://meowynetwork.com/"; every navigation must start with it

	std::wstring OriginOf(const std::wstring& url)
	{
		size_t scheme = url.find(L"://");
		if (scheme == std::wstring::npos)
			return L"";
		size_t slash = url.find(L'/', scheme + 3);
		return slash == std::wstring::npos ? url + L"/" : url.substr(0, slash + 1);
	}

	bool FromOurOrigin(LPCWSTR uri)
	{
		return uri && !g_origin.empty() && std::wcsncmp(uri, g_origin.c_str(), g_origin.size()) == 0;
	}

	HRESULT OnController(HRESULT result, ICoreWebView2Controller* controller, const std::wstring& url)
	{
		if (FAILED(result) || !controller || !g_parent)
		{
			g_failed = true;
			return S_OK;
		}
		g_controller = controller;
		g_controller->get_CoreWebView2(&g_view);
		if (!g_view)
		{
			g_failed = true;
			return S_OK;
		}

		ComPtr<ICoreWebView2Controller2> controller2;
		if (SUCCEEDED(g_controller.As(&controller2)))
			controller2->put_DefaultBackgroundColor({ 255, 11, 11, 13 });

		ComPtr<ICoreWebView2Settings> settings;
		if (SUCCEEDED(g_view->get_Settings(&settings)))
		{
			settings->put_AreDevToolsEnabled(FALSE);
			settings->put_AreDefaultContextMenusEnabled(FALSE);
			settings->put_AreDefaultScriptDialogsEnabled(FALSE);
			settings->put_IsStatusBarEnabled(FALSE);
			settings->put_IsZoomControlEnabled(FALSE);
			ComPtr<ICoreWebView2Settings3> settings3;
			if (SUCCEEDED(settings.As(&settings3)))
				settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
		}

		RECT rc;
		GetClientRect(g_parent, &rc);
		g_controller->put_Bounds(rc);
		g_controller->put_IsVisible(FALSE);

		EventRegistrationToken token;
		g_view->add_NavigationStarting(
			Callback<ICoreWebView2NavigationStartingEventHandler>(
				[](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
					LPWSTR uri = nullptr;
					args->get_Uri(&uri);
					if (!FromOurOrigin(uri))
						args->put_Cancel(TRUE);
					CoTaskMemFree(uri);
					return S_OK;
				})
				.Get(),
			&token);
		g_view->add_NewWindowRequested(
			Callback<ICoreWebView2NewWindowRequestedEventHandler>(
				[](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
					args->put_Handled(TRUE);   // no pop-ups, no new windows
					return S_OK;
				})
				.Get(),
			&token);
		g_view->add_NavigationCompleted(
			Callback<ICoreWebView2NavigationCompletedEventHandler>(
				[](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
					BOOL ok = FALSE;
					args->get_IsSuccess(&ok);
					if (!ok && !g_ready)
						g_failed = true;
					return S_OK;
				})
				.Get(),
			&token);
		g_view->add_WebMessageReceived(
			Callback<ICoreWebView2WebMessageReceivedEventHandler>(
				[](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
					LPWSTR source = nullptr, json = nullptr;
					args->get_Source(&source);
					bool ours = FromOurOrigin(source);
					CoTaskMemFree(source);
					if (!ours || FAILED(args->get_WebMessageAsJson(&json)) || !json)
						return S_OK;
					if (std::wcsstr(json, L"\"type\":\"ready\""))
					{
						if (!g_ready && !g_failed && g_controller)
							g_ready = true;   // shown by SetVisible when a join is on screen
					}
					else if (std::wcsstr(json, L"\"type\":\"cancel\""))
						g_action = 'c';
					else if (std::wcsstr(json, L"\"type\":\"hide\""))
						g_action = 'h';
					else if (std::wcsstr(json, L"\"type\":\"hold\""))
						g_action = 'k';
					else if (std::wcsstr(json, L"\"type\":\"focus\""))
						g_action = 'f';
					CoTaskMemFree(json);
					return S_OK;
				})
				.Get(),
			&token);

		if (FAILED(g_view->Navigate(url.c_str())))
			g_failed = true;
		return S_OK;
	}
}

namespace loading_webview
{
	bool Start(HWND parent, const std::wstring& url, const std::wstring& userDataDir)
	{
		Stop();
		g_parent = parent;
		g_origin = OriginOf(url);
		g_ready = g_failed = false;
		g_action = 0;
		if (g_origin.empty())
			return false;
		HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, userDataDir.c_str(), nullptr,
			Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
				[url](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
					if (FAILED(result) || !env || !g_parent)
					{
						g_failed = true;
						return S_OK;
					}
					return env->CreateCoreWebView2Controller(g_parent,
						Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
							[url](HRESULT r, ICoreWebView2Controller* c) -> HRESULT { return OnController(r, c, url); })
							.Get());
				})
				.Get());
		if (FAILED(hr))
		{
			g_failed = true;
			return false;
		}
		return true;
	}

	bool Ready() { return g_ready; }
	bool Failed() { return g_failed; }

	void Resize(int w, int h)
	{
		if (g_controller)
			g_controller->put_Bounds({ 0, 0, w, h });
	}

	void Post(const std::wstring& json)
	{
		if (g_ready && g_view)
			g_view->PostWebMessageAsJson(json.c_str());
	}

	double RasterScale()
	{
		ComPtr<ICoreWebView2Controller3> c3;
		double scale = 0;
		if (g_controller && SUCCEEDED(g_controller.As(&c3)))
			c3->get_RasterizationScale(&scale);
		return scale;
	}

	void SetVisible(bool visible)
	{
		if (g_controller)
			g_controller->put_IsVisible(visible ? TRUE : FALSE);
	}

	void Reload()
	{
		if (!g_view)
			return;
		g_ready = false;
		SetVisible(false);
		g_view->Reload();
	}

	void Capture(const std::wstring& path)
	{
		if (!g_ready || !g_view)
			return;
		ComPtr<IStream> file;
		if (FAILED(SHCreateStreamOnFileEx(path.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &file)))
			return;
		g_view->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, file.Get(),
			Callback<ICoreWebView2CapturePreviewCompletedHandler>([file](HRESULT) -> HRESULT { return S_OK; }).Get());
	}

	void Focus()
	{
		if (g_controller)
			g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
	}

	char TakeAction()
	{
		char a = g_action;
		g_action = 0;
		return a;
	}

	void Stop()
	{
		if (g_controller)
			g_controller->Close();
		g_view.Reset();
		g_controller.Reset();
		g_parent = nullptr;
		g_ready = false;
	}
}
#endif
