#pragma once
// Starting the pack's own threads (Windows). Every new thread in this module
// goes through Spawn or SpawnHandle here, never a bare std::thread or
// CreateThread:
//
//   - It never throws. A thread that can't be created (a 32-bit game short of
//     address space for another stack, say) is `false` / nullptr, so the
//     caller takes its single-flight flag or slot only after a successful
//     start, or puts it back.
//   - The thread holds its own reference to this DLL and drops it as its very
//     last step (FreeLibraryAndExitThread). In the client realm without the
//     start-up stub, the DLL isn't pinned and the game FreeLibrary's it after
//     the client realm closes: a thread of ours can never be left running code
//     from an unloaded image. (The DLL stays mapped until the thread ends, so a
//     thread that never ends keeps it loaded: only the pinned menu realm runs
//     endless loops.)
//   - The body runs inside try/catch: a C++ exception ends the thread, never
//     the game.
//   - It's named (SetThreadDescription, looked up at run time, so there's no
//     new import) for profiler and crash-dump captures, and its priority is
//     set explicitly.
//
// Header-only, so every target that compiles a file using it (the DLL, the
// smoke tests) needs nothing else.

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <functional>
#include <string>
#include <utility>

namespace thread_util
{
	enum class Priority
	{
		Normal,
		BelowNormal,
		Background,   // below normal, and EcoQoS (Windows may run it on efficiency cores, slower)
	};

	// Names the calling thread, where Windows can (Windows 10 1607 and later).
	inline void NameThisThread(const wchar_t* name)
	{
		using Fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
		HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
		Fn set = kernel ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(kernel, "SetThreadDescription"))) : nullptr;
		if (set && name)
			set(GetCurrentThread(), name);
	}

	// EcoQoS for the calling thread (on) or back to Windows' choice (off).
	// Failure (older Windows) is ignored: it's only a hint.
	inline void SetEcoQoS(bool on)
	{
		using Fn = BOOL(WINAPI*)(HANDLE, THREAD_INFORMATION_CLASS, LPVOID, DWORD);
		HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
		Fn set = kernel ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(kernel, "SetThreadInformation"))) : nullptr;
		if (!set)
			return;
		THREAD_POWER_THROTTLING_STATE state = {};
		state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
		state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
		state.StateMask = on ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0;
		set(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state));
	}

	namespace detail
	{
		struct Start
		{
			std::function<void()> fn;
			std::wstring name;
			Priority priority;
			HMODULE module;
		};

		inline void ApplyPriority(Priority p)
		{
			SetThreadPriority(GetCurrentThread(), p == Priority::Normal ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_BELOW_NORMAL);
			if (p == Priority::Background)
				SetEcoQoS(true);
		}

		inline DWORD WINAPI Entry(void* param)
		{
			Start* start = static_cast<Start*>(param);
			const HMODULE module = start->module;
			NameThisThread(start->name.c_str());
			ApplyPriority(start->priority);
			try
			{
				start->fn();
			}
			catch (...)
			{
			}
			delete start;   // the body's captures go here, while the code is still mapped
			FreeLibraryAndExitThread(module, 0);
		}
	}

	// Starts `fn` on a new thread (see the top of this file). The handle, or
	// nullptr if it couldn't start; the caller owns the handle (CloseHandle).
	// A template rather than a std::function parameter, so building the
	// std::function (which can allocate) happens in here, where it can't throw.
	template <class F>
	HANDLE SpawnHandle(const wchar_t* name, F&& fn, Priority p = Priority::Normal)
	{
		detail::Start* start = nullptr;
		try
		{
			start = new detail::Start{ std::function<void()>(std::forward<F>(fn)), name ? name : L"", p, nullptr };
		}
		catch (...)
		{
			return nullptr;
		}
		// No UNCHANGED_REFCOUNT: this is the thread's own reference, dropped by
		// FreeLibraryAndExitThread in Entry.
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&detail::Entry), &start->module))
		{
			delete start;
			return nullptr;
		}
		const HMODULE module = start->module;
		HANDLE thread = CreateThread(nullptr, 0, detail::Entry, start, 0, nullptr);
		if (!thread)
		{
			FreeLibrary(module);
			delete start;
			return nullptr;
		}
		return thread;
	}

	// SpawnHandle for a thread nobody waits for: whether it started.
	template <class F>
	bool Spawn(const wchar_t* name, F&& fn, Priority p = Priority::Normal)
	{
		HANDLE thread = SpawnHandle(name, std::forward<F>(fn), p);
		if (!thread)
			return false;
		CloseHandle(thread);
		return true;
	}
}

#endif
