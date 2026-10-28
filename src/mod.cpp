/* ad2mod.cpp -- in-game ImGui overlay for Astro Duel 2 injected as proxy steam dll
 * DllMain spawns one worker thread that:
 *   1. waits for VMP to unpack the image in place (UNPACK GATE below),
 *   2. IAT-hooks SDL_GL_SwapWindow to render an ImGui overlay on the
 *      game's own GL context (render strategy below),
 *   3. IAT-hooks the file APIs (kernel32/shell32/CRT fopen) for a
 *      runtime path log of what the game reads,
 *   4. installs MinHook LOG-ONLY hooks on the level-pool pipeline:
 *     GetWorldDataHook @ 0x1402ae190 WorldDataLoader::worldDataForType --
 *             the per-world level-count provider. Logs worldType + ra0
 *             (which call site fired). Defer-and-patch: the trampoline
 *             fills the out struct, we own only d3 (the count) and
 *             override it per world with the versus level cache's
 *             active-file count; worlds not in the cache run stock.
 *     GetLevelHook @ 0x14026C1C0 getLevelString -- the level-string
 *             composer every level-load path goes through (4 call sites
 *             binary-wide). Logs world/num/mode + the composed path + ra0
 *             (which composer fired) + the pick context globals.
 *     All MinHook hooks log the module-relative return address (ra0) via
 *     _ReturnAddress() -- /Oy- in CMakeLists keeps the frame pointer so
 *     it is valid (the entry patch is an absolute JMP; the game's CALL
 *     pushed the real rip before we arrived).
 *   All diagnostics -> %LOCALAPPDATA%\AstroDuel2Mod\probe_ui.log
 *
 *   UNPACK GATE: the game ships VMP-packed, and loaded at import
 *   resolution (steam_api64 proxy) the stub has NOT run yet -- sections
 *   are still ciphertext. Because all 117 .tmx levels are preloaded in
 *   one ~450 ms block ~10 s after process start, the worker polls until
 *   the VMP stub has unpacked the image in place (verified by the
 *   20-byte prologue at RVA 0x26cce0) before patching anything. If the
 *   image is already unpacked, the gate passes on the first check.
 *
 * Render strategy:
 *   - Worker thread (DllMain-spawned) waits for the game to settle, then
 *     IAT-hooks SDL_GL_SwapWindow: the game's IAT import directory is
 *     packer-forged, so we do NOT parse the PE. Instead:
 *         real_addr = GetProcAddress(GetModuleHandleA("SDL3.dll"), "SDL_GL_SwapWindow")
 *         slot      = scan known IAT region (RVA 0x47e028..0x47eba0) for == real_addr
 *     and patch that one slot to point at our hook.
 *   - The hook runs ON THE GAME'S RENDER THREAD, where the game's GL context
 *     is current -- exactly where we want to draw.
 *   - Lazy-inits ImGui (desktop GL backend, built-in gl3w-stripped loader,
 *     GLSL "#version 120" for the game's 2.1-compat context) on first swap.
 *   - Input: manual Win32 (GetCursorPos/ScreenToClient + GetAsyncKeyState),
 *     no WndProc subclassing. Reads are non-consuming, so the game still gets
 *     W/X/Y for its own level picker.
 *   - UI toggle: Ctrl+Alt+U (polled in worker thread, edge-triggered).
 */
#include "mod.h"

#include <cstdint>
#include <cstdlib>
#include <vector>
#include <winnt.h> // TODO: Investigate refactoring to use RtlCaptureStackBackTrace

#include "hooks_file.h"
#include "hooks_game.h"
#include "level_pool.h"
#include "overlay.h"

uintptr_t mod::moduleBase = 0;
HINSTANCE mod::dllInstance = nullptr;

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */
static FILE* logFile = nullptr;
/* log() is called from several threads (file-API hooks run on game threads,
   the watchdog on the worker thread) -- FILE* is not thread-safe, so every
   write is serialized. In practice the first call comes from the worker
   before any hook is installed, so the lazy init below is single-threaded. */
static CRITICAL_SECTION logCriticalSection;
static volatile LONG logCriticalSectionInit = 0;

void mod::Log(const char* fmt, ...)
{
	if (InterlockedCompareExchange(&logCriticalSectionInit, 1, 0) == 0)
		InitializeCriticalSection(&logCriticalSection);
	EnterCriticalSection(&logCriticalSection);
	if (!logFile)
	{
		wchar_t dir[1024];
		if (GetEnvironmentVariableW(L"LOCALAPPDATA", dir, 1024))
		{
			char dirA[1024];
			WideCharToMultiByte(CP_UTF8, 0, dir, -1, dirA, 1024, nullptr, nullptr);
			char path[1200];
			char sub[1200];
			snprintf(sub, sizeof(sub), "%s\\AstroDuel2Mod", dirA);
			snprintf(path, sizeof(path), "%s\\probe_ui.log", sub);
			CreateDirectoryA(sub, nullptr);
			logFile = fopen(path, "a");
			if (logFile) setvbuf(logFile, nullptr, _IOFBF, 4096);
		}
	}
	FILE* f = logFile;
	if (f)
	{
		SYSTEMTIME st;
		GetLocalTime(&st);
		fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
		va_list ap;
		va_start(ap, fmt);
		vfprintf(f, fmt, ap);
		va_end(ap);
		fputc('\n', f);
		fflush(f);
	}
	LeaveCriticalSection(&logCriticalSection);
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
			mod::Log("unpackgate: UNPACKED after %.2fs -- runtime code bytes verified", dt);
			return true;
		}
		if (i % 25 == 0 && logged++ < 10)
			mod::Log("unpackgate: still packed (attempt %d) ...", i);
		Sleep(200);
	}
	mod::Log("unpackgate: TIMEOUT -- bytes at +0x26cce0 = %02x %02x %02x .. %02x %02x %02x",
	         p[0], p[1], p[2], p[17], p[18], p[19]);
	return false;
}

static DWORD WINAPI Worker(LPVOID)
{
	mod::moduleBase = (uintptr_t) GetModuleHandleA(nullptr);

	mod::Log("session: ad2ui build %s %s | pid=%lu | module_base=%p",
	         __DATE__, __TIME__, (unsigned long) GetCurrentProcessId(), (void*) mod::moduleBase);

	/* versus level cache: the per-world .tmx inventory that drives the
	   count override (active files per world). Plain file read -- needs
	   no unpacking; built before the hooks install so the summary is in
	   the log before the first getWorldData call. */
	level_pool::Init();
	{
		/* the game's own command line */
		typedef long (WINAPI *FnReadProcCmdLine)(HANDLE, DWORD, PWSTR, DWORD, PDWORD);
		FnReadProcCmdLine rpc = nullptr;
		HMODULE hk = GetModuleHandleA("kernel32.dll");
		if (hk) rpc = (FnReadProcCmdLine) GetProcAddress(hk, "ReadProcessCommandLine");
		if (!rpc)
		{
			hk = GetModuleHandleA("kernelbase.dll");
			if (hk) rpc = (FnReadProcCmdLine) GetProcAddress(hk, "ReadProcessCommandLine");
		}
		if (rpc)
		{
			wchar_t cl[2048] = {0};
			DWORD len = 0;
			if (rpc(GetCurrentProcess(), 0, cl, 2048, &len) == 0)
				mod::Log("session: cmdline=%ls", cl);
			else
				mod::Log("session: ReadProcessCommandLine FAILED (err=%lu)", GetLastError());
		}
		else mod::Log("session: ReadProcessCommandLine not resolvable");
	}

	/* wait until VMP has unpacked the image in place before trusting ANY
	   runtime bytes (IAT slot scan, patches). */
	if (!WaitUnpacked())
	{
		mod::Log("worker: ABORTING -- game not unpacked, refusing to patch packed bytes");
		return 1;
	}

	if (!overlay::Init()) { return 1; }

	/* file-API IAT hooks: runtime path log (match-start diagnostics) */
	hooks::file::InstallHooks();

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

	return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		mod::dllInstance = h;
		DisableThreadLibraryCalls(h);
		HANDLE th = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
		if (th) CloseHandle(th);
	}
	return TRUE;
}
