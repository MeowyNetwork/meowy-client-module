#include "multicore.h"

#include <atomic>

#ifdef _WIN32
#include "crash_reports.h"
#include "file_util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace multicore
{
	namespace
	{
		std::atomic<bool> g_managed{ false };

#ifdef _WIN32
		// What AtProcessExit writes. Allocated once and never freed: it's read
		// while the process exits, after this DLL's statics may be gone. A
		// critical section, not a slim lock: a thread killed holding it at exit
		// doesn't hang the exit (and AtProcessExit only tries it).
		struct Exit
		{
			CRITICAL_SECTION lock;
			bool armed = false;
			std::wstring path;
			std::string text;
			std::wstring crashRoot;
			long long since = 0;
			Exit() { InitializeCriticalSectionAndSpinCount(&lock, 4000); }
		};
		Exit& E()
		{
			static Exit* e = new Exit();
			return *e;
		}
#endif
	}

	void SetManaged(bool on) { g_managed.store(on); }
	bool Managed() { return g_managed.load(); }

#ifdef _WIN32
	void ArmExitWrite(const std::wstring& path, const std::string& text, const std::wstring& crashRoot, long long since)
	{
		Exit& e = E();
		EnterCriticalSection(&e.lock);
		e.armed = !path.empty();
		e.path = path;
		e.text = text;
		e.crashRoot = crashRoot;
		e.since = since;
		LeaveCriticalSection(&e.lock);
	}

	void Disarm()
	{
		Exit& e = E();
		EnterCriticalSection(&e.lock);
		e.armed = false;
		LeaveCriticalSection(&e.lock);
	}

	bool AtProcessExit()
	{
		Exit& e = E();
		if (!TryEnterCriticalSection(&e.lock))
			return false;
		bool wrote = false;
		if (e.armed)
		{
			e.armed = false;
			// A crash report from this run: the game didn't close cleanly, even
			// if its crash handler then exited the normal way.
			const bool crashed = !e.crashRoot.empty() && !crashes::Candidates(e.crashRoot, e.since - 1).empty();
			if (!crashed)
				wrote = file_util::WriteAtomic(e.path, e.text, file_util::Durability::Relaxed);
		}
		LeaveCriticalSection(&e.lock);
		return wrote;
	}
#else
	void ArmExitWrite(const std::wstring&, const std::string&, const std::wstring&, long long) {}
	void Disarm() {}
	bool AtProcessExit() { return false; }
#endif
}
