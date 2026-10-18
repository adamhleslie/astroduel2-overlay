/* ad2launch.c -- suspended-start launcher for Astro Duel 2 (simplistic).
 *
 *   ad2launch.exe [path-to-ad2ui.dll] [extra game args...]
 *
 * Why suspended:
 *   All 117 .tmx levels are preloaded in ONE ~450 ms block ~10 s after
 *   process start (Procmon capture). Any runtime intervention that needs
 *   to be in place before that window can't rely on manual inject-after-
 *   launch. So we:
 *     1. CreateProcessW(AstroDuel2.exe, CREATE_SUSPENDED)  (cwd = game dir)
 *     2. CreateRemoteThread(LoadLibraryW, ad2ui.dll)       (same as inject.c)
 *     3. wait for LoadLibrary to return (DllMain has spawned the worker)
 *     4. ResumeThread -- the game's VMP stub unpacks, the worker's unpack
 *        gate passes, hooks install, all long before the preload block.
 *   The loader lock held by the remote thread during LoadLibrary does not
 *   block the (suspended) main thread; the worker thread in ad2ui.dll
 *   polls for the unpacked image independently.
 *
 * Steam: appid 655350 has no launch args / steam:// requirement -- Steam's
 * own Play button is effectively what step 1 does. The game talks to the
 * Steam client over the client pipe regardless of its parent process, and
 * self-launches Steam if the client is closed (slower boot, more margin).
 * We warn (but do not refuse) if the Steam client is not running.
 *
 * Exit codes: 0 = launched+injected+resumed. Non-zero = failure (the game
 * process, if created, is terminated on any post-CreateProcess failure).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GAME_EXE_DEFAULT  L"D:\\SteamLibrary\\steamapps\\common\\Astro Duel 2\\AstroDuel2.exe"

static void fail(const char* msg, DWORD code)
{
    fprintf(stderr, "ad2launch: %s (err=%lu)\n", msg, code ? code : GetLastError());
}

static void w2a(const wchar_t* w, char* out, int n)
{
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
    out[n - 1] = 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s [path-to-ad2ui.dll] [game args...]\n", argv[0]);
        return 2;
    }

    /* ---- locate ad2ui.dll: arg 2, else next to this exe ---- */
    wchar_t dllw[MAX_PATH];
    if (MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, dllw, MAX_PATH) <= 0) {
        fprintf(stderr, "ad2launch: DLL path too long\n");
        return 2;
    }
    if (GetFileAttributesW(dllw) == INVALID_FILE_ATTRIBUTES) {
        wchar_t self[MAX_PATH];
        if (GetModuleFileNameW(NULL, self, MAX_PATH) > 0) {
            wchar_t* sl = wcsrchr(self, L'\\');
            if (sl) *sl = 0;
            if (GetFileAttributesW(dllw) == INVALID_FILE_ATTRIBUTES) {
                wsprintfW(dllw, L"%s\\ad2ui.dll", self);
            }
        }
    }
    if (GetFileAttributesW(dllw) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "ad2launch: ad2ui.dll not found (pass its path as arg 1)\n");
        return 2;
    }

    /* ---- locate the game exe ---- */
    wchar_t game[MAX_PATH];
    wcscpy_s(game, MAX_PATH, GAME_EXE_DEFAULT);
    if (GetFileAttributesW(game) == INVALID_FILE_ATTRIBUTES) {
        {
            char ga[MAX_PATH]; w2a(game, ga, MAX_PATH);
            fprintf(stderr, "ad2launch: game exe not found at %s\n", ga);
        }
        return 2;
    }
    wchar_t gamedir[MAX_PATH];
    wcscpy_s(gamedir, MAX_PATH, game);
    wchar_t* sl = wcsrchr(gamedir, L'\\');
    if (sl) *sl = 0;

    /* ---- Steam client check (warn only) ---- */
    if (!FindWindowW(L"Steam_UIWindow", NULL))
        printf("ad2launch: NOTE: Steam client not running -- the game will launch it; "
               "expect a slower first boot.\n");

    /* ---- 1. suspended start ---- */
    wchar_t cmd[2 * MAX_PATH];
    wsprintfW(cmd, L"\"%s\"", game);
    for (int i = 2; i < argc; i++) {
        wchar_t extra[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, extra, MAX_PATH);
        wcscat_s(cmd, 2 * MAX_PATH, L" ");
        wcscat_s(cmd, 2 * MAX_PATH, extra);
    }
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(game, cmd, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, gamedir, &si, &pi)) {
        fail("CreateProcessW failed", 0);
        return 1;
    }
    {
        char ga[MAX_PATH]; w2a(game, ga, MAX_PATH);
        printf("ad2launch: started %s (pid=%lu, suspended)\n", ga, pi.dwProcessId);
    }
    CloseHandle(pi.hThread);

    /* ---- 2. inject ---- */
    HANDLE hProc = pi.hProcess;
    int w = MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, NULL, 0);
    LPVOID remote = VirtualAllocEx(hProc, NULL, w * sizeof(WCHAR),
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    int ok = 0;
    if (!remote) { fail("VirtualAllocEx failed", 0); goto done; }
    if (!WriteProcessMemory(hProc, remote, dllw, w * sizeof(WCHAR), NULL)) {
        fail("WriteProcessMemory failed", 0); goto done;
    }
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    LPTHREAD_START_ROUTINE loadlib =
        (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW");
    HANDLE th = CreateRemoteThread(hProc, NULL, 0, loadlib, remote, 0, NULL);
    if (!th) { fail("CreateRemoteThread failed", 0); goto done; }

    DWORD code = STILL_ACTIVE;
    WaitForSingleObject(th, 8000);
    GetExitCodeThread(th, &code);
    if (code == STILL_ACTIVE) {
        fail("LoadLibrary did not finish in 8 s", 0); goto done;
    }
    if (code == 0) {
        fail("LoadLibraryW returned NULL -- DLL failed to load", 0); goto done;
    }
    {
        char da[MAX_PATH]; w2a(dllw, da, MAX_PATH);
        printf("ad2launch: injected %s (module handle %p)\n", da, (void*)code);
    }
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);

    /* ---- 3. resume ---- */
    if (!ResumeThread(pi.hThread)) { fail("ResumeThread failed", 0); goto done; }
    {
        char la[260] = "";
        GetEnvironmentVariableA("LOCALAPPDATA", la, sizeof la);
        printf("ad2launch: RESUMED -- game is running; the ad2ui worker will wait "
               "for the VMP unpack and install hooks (see %s\\AstroDuel2Mod\\probe_ui.log)\n", la);
    }
    ok = 1;

done:
    if (!ok) {
        printf("ad2launch: terminating game pid=%lu\n", pi.dwProcessId);
        TerminateProcess(hProc, 1);
    }
    CloseHandle(hProc);
    return ok ? 0 : 1;
}
