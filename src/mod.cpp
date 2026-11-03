/* mod.cpp -- in-game ImGui overlay for Astro Duel 2 injected as proxy steam dll running on a thread spun up on DllMain:
 *   1. Waits for VMP to unpack the image in place
 *   2. Initializes the imgui overlay
 *   3. Installs hooks under, via hooks:: namespaces
 *   4. Runs the overlay loop
 *   All diagnostics -> %LOCALAPPDATA%\AstroDuel2Mod\probe_ui.log
 *
 *  VMP Unpack: the game ships VMP-packed, and loaded at import
 *   resolution (steam_api64 proxy) the stub has NOT run yet -- sections
 *   are still ciphertext. Because all 117 .tmx levels are preloaded in
 *   one ~450 ms block ~10 s after process start, the worker polls until
 *   the VMP stub has unpacked the image in place (verified by the
 *   20-byte prologue at RVA 0x26cce0) before patching anything. If the
 *   image is already unpacked, the gate passes on the first check.
 */
#include "mod.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shlobj_core.h>
#include <winnt.h> // TODO: Investigate refactoring to use RtlCaptureStackBackTrace
#include <print>

#include "hooks_iat.h"
#include "hooks_game.h"
#include "level_pool.h"
#include "overlay.h"

uintptr_t mod::moduleBase = 0;
HINSTANCE mod::dllInstance = nullptr;
std::mutex mod::internal::logMutex;
std::ofstream mod::internal::logFileStream;

namespace
{
	bool InitLog()
	{
		std::filesystem::path logFilePath;
		{
			// Get the local app data path on windows, which our log file is relative to
			PWSTR appDataPath;
			if (SHGetKnownFolderPath(mod::logFileFolderId, 0, nullptr, &appDataPath) != S_OK)
			{
				// Could not retrieve the local app data path
				CoTaskMemFree(appDataPath);
				return false;
			}
			logFilePath = std::filesystem::path{appDataPath};
			CoTaskMemFree(appDataPath);
		}
		logFilePath.append(mod::logFile);

		if (!logFilePath.has_filename())
		{
			// No file provided
			return false;
		}

		if (logFilePath.has_parent_path())
		{
			std::error_code error;
			std::filesystem::create_directories(logFilePath.parent_path(), error);
			if (error.value())
			{
				// Could not create directories
				return false;
			}
		}

		mod::internal::logFileStream.open(logFilePath, std::ofstream::out | std::ofstream::app);
		return mod::internal::logFileStream.is_open();
	}
}

/* ------------------------------------------------------------------ */
/* worker: locate IAT slot, patch, poll hotkey                         */
/* ------------------------------------------------------------------ */

/* UNPACK GATE ------------------------------------------------------- */
/* Loaded at import resolution (steam_api64 proxy), the VMP stub has
   NOT run yet: the game's sections are still ciphertext, so every
   runtime-byte check (prologues, IAT slot contents) sees the packed
   image. Poll until the unpacked image is verifiable -- we use the
   20-byte shadow-register-store prologue at RVA 0x26cce0
   (capstone-verified against the unpacked PE). If the image is
   already unpacked, the gate passes on the first check. */
static bool WaitUnpacked()
{
	const BYTE want[20] = {
		0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
		0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20
	};
	const uint8_t* p = (const uint8_t*) (mod::moduleBase + 0x26cce0);
	LARGE_INTEGER t0;
	QueryPerformanceCounter(&t0);
	LARGE_INTEGER frq;
	QueryPerformanceFrequency(&frq);
	int logged = 0;
	for (int i = 0; i < 450; i++)
	{
		/* 450 * 200 ms = 90 s */
		if (memcmp(p, want, 20) == 0)
		{
			LARGE_INTEGER t1;
			QueryPerformanceCounter(&t1);
			double dt = (double) (t1.QuadPart - t0.QuadPart) / frq.QuadPart;
			mod::Log("unpackgate: UNPACKED after {:.2f}s -- runtime code bytes verified", dt);
			return true;
		}
		if (i % 25 == 0 && logged++ < 10)
			mod::Log("unpackgate: still packed (attempt {}) ...", i);
		Sleep(200);
	}
	mod::Log("unpackgate: TIMEOUT -- bytes at +0x26cce0 = {:02X} {:02X} {:02X} .. {:02X} {:02X} {:02X}",
	         p[0], p[1], p[2], p[17], p[18], p[19]);
	return false;
}

void mod::Main()
{
	if (!InitLog())
	{
		// TODO: Alert user to log file failing
		return;
	}
	moduleBase = (uintptr_t)GetModuleHandleA(nullptr);
	Log("session: ad2ui build {} {} | pid={} | module_base={}", __DATE__, __TIME__, GetCurrentProcessId(), reinterpret_cast<void*>(moduleBase));

	/* versus level cache: the per-world .tmx inventory that drives the
	   count override (active files per world). Plain file read -- needs
	   no unpacking; built before the hooks install so the summary is in
	   the log before the first getWorldData call. */
	level_pool::Init();
	{
		// TODO: Refactor to use NtQueryInformationProcess ?
		
		char* cl = GetCommandLineA();
		Log("session: cmdline={}", cl);
	}

	/* wait until VMP has unpacked the image in place before trusting ANY
	   runtime bytes (IAT slot scan, patches). */
	if (!WaitUnpacked())
	{
		Log("worker: ABORTING -- game not unpacked, refusing to patch packed bytes");
		return;
	}

	if (!overlay::Init()) { return; }

	/* file-API IAT hooks: runtime path log (match-start diagnostics) */
	hooks::iat::InstallHooks();

	/* hook on worldDataForType (defer-and-patch count). Installed after
	   the IAT hooks (render path stable) and before the ~t+9s preload
	   block that first fires it. */
	hooks::game::InstallGetWorldDataHook();

	/* level-string composer hook: identifies which call site fired
	   (ra0), applies the slot -> file remap (stage 2), logs the final
	   path and the pick context (pick/spec/cnt). Installed after
	   PoolCacheBuild/PoolDraftInit so the cache is ready on first fire. */
	hooks::game::InstallGetLevelHook();

	overlay::Loop();
}
