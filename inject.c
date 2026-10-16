/* inject.c -- load ad2mod.dll into a running x64 process.
 *
 *   inject.exe <PID> <path-to-ad2mod.dll>
 *
 * Classic CreateRemoteThread + LoadLibraryW. The game has no code integrity
 * or anti-cheat, and we only inject when the user says so. We do NOT kill or
 * detach the game's thread ownership; we wait for LoadLibrary to return so
 * DllMain has spawned the worker before we exit.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <PID> <path-to-ad2mod.dll>\n", argv[0]);
        return 2;
    }
    DWORD pid = (DWORD)strtoul(argv[1], NULL, 10);
    const char* dll = argv[2];

    char wide[4096];
    int w = MultiByteToWideChar(CP_ACP, 0, dll, -1, wide, 4096);
    if (w <= 0) { fprintf(stderr, "path too long\n"); return 2; }

    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc) {
        fprintf(stderr, "OpenProcess(%lu) failed: %lu\n", pid, GetLastError());
        return 1;
    }
    LPVOID remote = VirtualAllocEx(hProc, NULL, w * sizeof(WCHAR),
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { fprintf(stderr, "VirtualAllocEx failed: %lu\n", GetLastError()); return 1; }
    if (!WriteProcessMemory(hProc, remote, wide, w * sizeof(WCHAR), NULL)) {
        fprintf(stderr, "WriteProcessMemory failed: %lu\n", GetLastError());
        return 1;
    }
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    LPTHREAD_START_ROUTINE loadlib = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW");
    HANDLE th = CreateRemoteThread(hProc, NULL, 0, loadlib, remote, 0, NULL);
    if (!th) { fprintf(stderr, "CreateRemoteThread failed: %lu\n", GetLastError()); return 1; }

    DWORD code = WAIT_TIMEOUT;
    WaitForSingleObject(th, 8000);
    GetExitCodeThread(th, &code);
    CloseHandle(th);
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hProc);

    if (code == (DWORD)-1 || code == STILL_ACTIVE) {
        fprintf(stderr, "LoadLibrary did not finish in time\n");
        return 1;
    }
    if (code == 0) {
        fprintf(stderr, "LoadLibraryW returned NULL -- DLL failed to load\n");
        return 1;
    }
    printf("Injected %s (module handle %p) into PID %lu.\n", dll, (void*)code, pid);
    printf("Keys in-game: Ctrl+Alt+V = version smoke test, Ctrl+Alt+K = upload from cmd.txt\n");
    printf("Log: %%LOCALAPPDATA%%\\AstroDuel2Mod\\probe.log\n");
    return 0;
}
