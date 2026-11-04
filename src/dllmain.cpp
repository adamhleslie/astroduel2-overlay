#include <thread>

#include "mod.h"

namespace
{
	// Simple intermediary worker used to launch a thread for the mod's main from DllMain
	unsigned WINAPI SimpleStartupWorker(LPVOID)
	{
		std::thread modThread(&mod::Main);
		if (modThread.joinable())
		{
			modThread.join();
		}

		return 0;
	}
}

// Thread creation in DllMain is risky, so we indirectly start our mod's worker thread via an intermediary, without blocking on the initial DLL_PROCESS_ATTACH thread
// https://devblogs.microsoft.com/oldnewthing/20070904-00/?p=25283/
// TODO: This doesn't seem strictly necessary, but for now leaving it in until the alternative is tested more thoroughly
int WINAPI DllMain(HINSTANCE hinstDll, DWORD fdwReason, LPVOID)
{
	if (fdwReason == DLL_PROCESS_ATTACH)
	{
		mod::dllModuleBase = hinstDll;
		HANDLE hThread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, &SimpleStartupWorker, nullptr, 0, nullptr));
		if (hThread)
		{
			CloseHandle(hThread);
		}
	}

	return 1;
}