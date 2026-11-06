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
	// Everything WebView2 hands us, on the loading screen's thread only.
	//
	// Allocated once and never freed, on purpose: as plain globals the COM
	// pointers would be released by the CRT's static destructors when the game
	// exits, on the exiting thread, under the loader lock, after the screen's
	// thread was killed and after WebView2's own DLL (loaded after this one) had
	// already been detached. The browser processes notice the game is gone and
	// exit on their own.
	struct Web
	{
		HWND parent = nullptr;
		ComPtr<ICoreWebView2Environment> env;
		ComPtr<ICoreWebView2Controller> controller;
		ComPtr<ICoreWebView2> view;
		EventRegistrationToken browserExitToken = {};
		bool browserExitHooked = false;
		bool ready = false, failed = false;
		char action = 0;
		std::wstring origin;   // "https://meowynetwork.com/"; every navigation must start with it
		// Bumped by every Start and Stop: completions and events of an earlier
		// browser (a late creation, the exit of one we closed) are ignored.
		unsigned generation = 0;
	};

	Web& W()
	{
		static Web* const web = new Web();   // never deleted: see above
		return *web;
	}

	bool Current(unsigned generation)
	{
		return generation == W().generation;
	}

	void Fail(unsigned generation)
	{
		if (!Current(generation))
			return;
		W().failed = true;
		W().ready = false;
	}

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
		const std::wstring& origin = W().origin;
		return uri && !origin.empty() && std::wcsncmp(uri, origin.c_str(), origin.size()) == 0;
	}

	HRESULT OnController(unsigned generation, HRESULT result, ICoreWebView2Controller* controller, const std::wstring& url)
	{
		Web& w = W();
		if (!Current(generation) || !w.parent)
		{
			// Stopped (or restarted) while this one was being made.
			if (controller)
				controller->Close();
			return S_OK;
		}
		if (FAILED(result) || !controller)
		{
			Fail(generation);
			return S_OK;
		}
		w.controller = controller;
		if (FAILED(w.controller->get_CoreWebView2(&w.view)) || !w.view)
		{
			Fail(generation);
			return S_OK;
		}

		ComPtr<ICoreWebView2Controller2> controller2;
		if (SUCCEEDED(w.controller.As(&controller2)))
			controller2->put_DefaultBackgroundColor({ 255, 11, 11, 13 });

		ComPtr<ICoreWebView2Settings> settings;
		if (SUCCEEDED(w.view->get_Settings(&settings)))
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
		GetClientRect(w.parent, &rc);
		w.controller->put_Bounds(rc);
		w.controller->put_IsVisible(FALSE);

		// The page must never leave our origin or open windows: without these
		// two guards it isn't loaded at all.
		EventRegistrationToken token;
		HRESULT guards = w.view->add_NavigationStarting(
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
		if (SUCCEEDED(guards))
			guards = w.view->add_NewWindowRequested(
				Callback<ICoreWebView2NewWindowRequestedEventHandler>(
					[](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
						args->put_Handled(TRUE);   // no pop-ups, no new windows
						return S_OK;
					})
					.Get(),
				&token);
		if (FAILED(guards))
		{
			Fail(generation);
			return S_OK;
		}
		w.view->add_NavigationCompleted(
			Callback<ICoreWebView2NavigationCompletedEventHandler>(
				[generation](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
					BOOL ok = FALSE;
					args->get_IsSuccess(&ok);
					if (!ok && !W().ready)
						Fail(generation);
					return S_OK;
				})
				.Get(),
			&token);
		// The browser or the page's process died (crashed, or ended in Task
		// Manager): the page is gone for good, so the native drawing takes over
		// (also mid-join) and the next join starts a fresh browser.
		w.view->add_ProcessFailed(
			Callback<ICoreWebView2ProcessFailedEventHandler>(
				[generation](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
					COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
					if (args)
						args->get_ProcessFailedKind(&kind);
					if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED ||
						kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED ||
						kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE)
						Fail(generation);
					return S_OK;
				})
				.Get(),
			&token);
		w.view->add_WebMessageReceived(
			Callback<ICoreWebView2WebMessageReceivedEventHandler>(
				[generation](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
					if (!Current(generation))
						return S_OK;
					Web& web = W();
					LPWSTR source = nullptr, json = nullptr;
					args->get_Source(&source);
					bool ours = FromOurOrigin(source);
					CoTaskMemFree(source);
					if (!ours || FAILED(args->get_WebMessageAsJson(&json)) || !json)
						return S_OK;
					if (std::wcsstr(json, L"\"type\":\"ready\""))
					{
						if (!web.ready && !web.failed && web.controller)
							web.ready = true;   // shown by SetVisible when a join is on screen
					}
					else if (std::wcsstr(json, L"\"type\":\"cancel\""))
						web.action = 'c';
					else if (std::wcsstr(json, L"\"type\":\"hide\""))
						web.action = 'h';
					else if (std::wcsstr(json, L"\"type\":\"hold\""))
						web.action = 'k';
					else if (std::wcsstr(json, L"\"type\":\"focus\""))
						web.action = 'f';
					CoTaskMemFree(json);
					return S_OK;
				})
				.Get(),
			&token);

		if (FAILED(w.view->Navigate(url.c_str())))
			Fail(generation);
		return S_OK;
	}

	HRESULT OnEnvironment(unsigned generation, HRESULT result, ICoreWebView2Environment* env, const std::wstring& url)
	{
		Web& w = W();
		if (!Current(generation) || !w.parent)
			return S_OK;
		if (FAILED(result) || !env)
		{
			Fail(generation);
			return S_OK;
		}
		w.env = env;
		// The browser process can also go away without a ProcessFailed (before
		// the WebView exists, say); either way this browser is finished.
		ComPtr<ICoreWebView2Environment5> env5;
		if (SUCCEEDED(w.env.As(&env5)) &&
			SUCCEEDED(env5->add_BrowserProcessExited(
				Callback<ICoreWebView2BrowserProcessExitedEventHandler>(
					[generation](ICoreWebView2Environment*, ICoreWebView2BrowserProcessExitedEventArgs*) -> HRESULT {
						Fail(generation);
						return S_OK;
					})
					.Get(),
				&w.browserExitToken)))
			w.browserExitHooked = true;

		HRESULT hr = env->CreateCoreWebView2Controller(w.parent,
			Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
				[generation, url](HRESULT r, ICoreWebView2Controller* c) -> HRESULT { return OnController(generation, r, c, url); })
				.Get());
		// Refused straight away: the completion never comes, so fail now or the
		// screen would wait for this browser all session.
		if (FAILED(hr))
			Fail(generation);
		return S_OK;
	}
}

namespace loading_webview
{
	bool Start(HWND parent, const std::wstring& url, const std::wstring& userDataDir)
	{
		Stop();
		Web& w = W();
		const unsigned generation = w.generation;
		w.parent = parent;
		w.origin = OriginOf(url);
		w.ready = w.failed = false;
		w.action = 0;
		if (w.origin.empty())
		{
			w.failed = true;
			return false;
		}
		HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, userDataDir.c_str(), nullptr,
			Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
				[generation, url](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
					return OnEnvironment(generation, result, env, url);
				})
				.Get());
		if (FAILED(hr))
		{
			w.failed = true;
			return false;
		}
		return true;
	}

	bool Ready() { return W().ready; }
	bool Failed() { return W().failed; }

	void Resize(int w, int h)
	{
		if (W().controller)
			W().controller->put_Bounds({ 0, 0, w, h });
	}

	void Post(const std::wstring& json)
	{
		if (W().ready && W().view)
			W().view->PostWebMessageAsJson(json.c_str());
	}

	double RasterScale()
	{
		ComPtr<ICoreWebView2Controller3> c3;
		double scale = 0;
		if (W().controller && SUCCEEDED(W().controller.As(&c3)))
			c3->get_RasterizationScale(&scale);
		return scale;
	}

	void SetVisible(bool visible)
	{
		if (W().controller)
			W().controller->put_IsVisible(visible ? TRUE : FALSE);
	}

	void Reload()
	{
		Web& w = W();
		if (!w.view)
			return;
		w.ready = false;
		SetVisible(false);
		if (FAILED(w.view->Reload()))
			Fail(w.generation);
	}

	void Capture(const std::wstring& path)
	{
		Web& w = W();
		if (!w.ready || !w.view)
			return;
		ComPtr<IStream> file;
		if (FAILED(SHCreateStreamOnFileEx(path.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &file)))
			return;
		w.view->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, file.Get(),
			Callback<ICoreWebView2CapturePreviewCompletedHandler>([file](HRESULT) -> HRESULT { return S_OK; }).Get());
	}

	void Focus()
	{
		if (W().controller)
			W().controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
	}

	char TakeAction()
	{
		char a = W().action;
		W().action = 0;
		return a;
	}

	void Stop()
	{
		Web& w = W();
		++w.generation;   // whatever the old browser still reports is ignored
		if (w.browserExitHooked && w.env)
		{
			ComPtr<ICoreWebView2Environment5> env5;
			if (SUCCEEDED(w.env.As(&env5)))
				env5->remove_BrowserProcessExited(w.browserExitToken);
		}
		w.browserExitHooked = false;
		if (w.controller)
			w.controller->Close();
		w.view.Reset();
		w.controller.Reset();
		w.env.Reset();
		w.parent = nullptr;
		w.ready = false;
		w.action = 0;
	}
}
#endif
