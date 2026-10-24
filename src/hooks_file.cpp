/* file-API IAT hooks: runtime path log (match-start diagnostics)      */
/*                                                                     */
/* IAT-hooks the game's own file imports (kernel32/shell32, not VMP   */
/* code) and logs every dir scan, mkdir, and open on "interesting"    */
/* paths (levels/*.tmx, *_data.xml, res/...). Ground truth for WHICH   */
/* level files a match actually loads.                                  */
/* ------------------------------------------------------------------ */

#include "hooks_file.h"

#include <cstdio>

#include "ad2mod.h"

static volatile LONG s_hook_active = 0;
typedef HANDLE (WINAPI *FnFindFirstFileW)(const wchar_t*, WIN32_FIND_DATAW*);
typedef BOOL   (WINAPI *FnCreateDirectoryW)(const wchar_t*, void*);
typedef HANDLE (WINAPI *FnCreateFileW)(const wchar_t*, DWORD, DWORD, void*, DWORD, DWORD, void*);
typedef BOOL   (WINAPI *FnCreateFile2)(const wchar_t*, void*, DWORD, DWORD, void*);
typedef DWORD  (WINAPI *FnGetFileAttributesW)(const wchar_t*);
typedef HRESULT (WINAPI *FnSHGKF)(const GUID*, DWORD, HANDLE, PWSTR*);
static FnFindFirstFileW s_fff_orig = nullptr;
static FnCreateDirectoryW s_cdw_orig = nullptr;
static FnCreateFileW s_cfw_orig = nullptr;
static FnCreateFile2 s_cf2_orig = nullptr;
static FnGetFileAttributesW s_gfa_orig = nullptr;
static FnSHGKF s_shgkf_orig = nullptr;

/* case-insensitive substring test (hooks run at game call rates; keep it
   cheap -- a handful of short needles, early-out) */
static bool IContains(const wchar_t* hay, const wchar_t* needle)
{
    for (const wchar_t* h = hay; *h; ++h) {
        const wchar_t* a = h, *b = needle;
        while (*a && *b) {
            wchar_t la = (*a >= L'A' && *a <= L'Z') ? (wchar_t)(*a + 32) : *a;
            wchar_t lb = (*b >= L'A' && *b <= L'Z') ? (wchar_t)(*b + 32) : *b;
            if (la != lb) break;
            ++a; ++b;
        }
        if (*b == 0) return true;
    }
    return false;
}

/* Only interesting paths reach the log for the high-volume file APIs:
   custom-levels names, the candidate base dirs, and anything level-ish. */
static bool PathInteresting(const wchar_t* p)
{
    if (!p) return false;
    static const wchar_t* const kFilters[] = {
        L"tmx", L"_data.xml", L"bounty", L"ironcactus", L"dropbox",
        L"\\games\\", L"astro duel",
    };
    for (size_t i = 0; i < sizeof(kFilters) / sizeof(kFilters[0]); i++)
        if (IContains(p, kFilters[i])) return true;
    return false;
}

static HANDLE WINAPI Ad2FindFirstFileW(const wchar_t* pattern, WIN32_FIND_DATAW* fd)
{
    if (pattern && s_hook_active) ad2mod::Log("GAME FindFirstFileW: %ls", pattern);
    return s_fff_orig(pattern, fd);
}
static BOOL WINAPI Ad2CreateDirectoryW(const wchar_t* path, void* sa)
{
    BOOL r = s_cdw_orig(path, sa);
    if (path && s_hook_active) ad2mod::Log("GAME CreateDirectoryW: %ls  (ret=%d, err=%lu)", path, r, GetLastError());
    return r;
}

/* CreateFileW: catches level-file opens AND failures (the "lists but won't
   load" case). Filtered to interesting paths so res-loading noise stays out. */
static HANDLE WINAPI Ad2CreateFileW(const wchar_t* path, DWORD acc, DWORD share,
                                    void* sa, DWORD disp, DWORD flags, void* tmpl)
{
    HANDLE h = s_cfw_orig(path, acc, share, sa, disp, flags, tmpl);
    if (s_hook_active && PathInteresting(path))
        ad2mod::Log("GAME CreateFileW: %ls  (ret=%p%s)", path, (void*)h,
            h == INVALID_HANDLE_VALUE ? " FAILED" : "");
    return h;
}

static BOOL WINAPI Ad2CreateFile2(const wchar_t* path, void* ph, DWORD acc,
                                  DWORD opts, void* info)
{
    BOOL r = s_cf2_orig(path, ph, acc, opts, info);
    if (s_hook_active && PathInteresting(path))
        ad2mod::Log("GAME CreateFile2: %ls  (ret=%d%s)", path, r, r ? "" : " FAILED");
    return r;
}

/* GetFileAttributesW: the dir-exists pre-checks probe through this --
   a NOT-FOUND result on a base dir shows up here. */
static DWORD WINAPI Ad2GetFileAttributesW(const wchar_t* path)
{
    DWORD r = s_gfa_orig(path);
    if (s_hook_active && PathInteresting(path))
        ad2mod::Log("GAME GetFileAttributesW: %ls  (ret=0x%lx%s)", path, r,
            r == INVALID_FILE_ATTRIBUTES ? " NOT-FOUND" : "");
    return r;
}

/* fopen: the real level-load path. cocos FileUtils::fillFileData
   (_stat64i32 + fopen(path,"rb")) loads every .tmx and *_data.xml through
   the CRT import, invisible to the kernel32 IAT hooks. Hook the game's own
   fopen import (ucrtbase or msvcrt, resolved at runtime; only the one
   actually in the game IAT will match a slot). */
typedef FILE* (__cdecl *FnFopen)(const char*, const char*);
static FnFopen s_fopen_orig = nullptr;

static bool PathInterestingA(const char* p)
{
    if (!p) return false;
    struct { const char* s; size_t n; } f[] = {
        { "tmx", 3 }, { "_data.xml", 9 }, { "res/levels", 10 },
        { "bounty", 6 }, { "dropbox", 7 },
    };
    /* case-insensitive substring scan */
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        for (const char* h = p; *h; ++h) {
            const char* a = h; size_t j = 0;
            for (; *a; ++a, ++j) {
                char la = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
                char lb = (f[i].s[j] >= 'A' && f[i].s[j] <= 'Z') ? (char)(f[i].s[j] + 32) : f[i].s[j];
                if (la != lb) break;
                if (j + 1 == f[i].n) return true;
            }
        }
    }
    return false;
}

static FILE* __cdecl Ad2Fopen(const char* path, const char* mode)
{
    FILE* f = s_fopen_orig(path, mode);
    if (s_hook_active && PathInterestingA(path))
        ad2mod::Log("GAME fopen: %s  [%s]  (ret=%p%s)", path, mode ? mode : "?",
            (void*)f, f ? "" : " FAILED");
    return f;
}

/* SHGetKnownFolderPath: the base-path getter calls this with
   FOLDERID_UserGameLocations. Logging every request + result is direct
   ground truth for the true base path. */
static void LogGuid(const GUID* g, char* out, size_t n)
{
    snprintf(out, n, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1],
             g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

static HRESULT WINAPI Ad2SHGKF(const GUID* rfid, DWORD flags, HANDLE tok, PWSTR* out)
{
    HRESULT hr = s_shgkf_orig(rfid, flags, tok, out);
    if (s_hook_active) {
        char g[64] = "(null)";
        if (rfid) LogGuid(rfid, g, sizeof g);
        if (hr == S_OK && out && *out)
            ad2mod::Log("GAME SHGetKnownFolderPath %s -> %ls", g, *out);
        else
            ad2mod::Log("GAME SHGetKnownFolderPath %s FAILED hr=0x%08lX", g, (unsigned long)hr);
    }
    return hr;
}

/* IAT-slot hook core (same mechanism as the SDL_GL_SwapWindow hook:
   overwrite the game's import-table slot, no trampolines, no patching of
   system-DLL code). `expected` is the runtime-resolved address the slot
   should hold. */
static uintptr_t HookIatSlot(uintptr_t expected, void* hook, const char* tag)
{
    if (!expected) { ad2mod::Log("iat-hook %s: expected address NOT resolvable", tag); return 0; }
    ad2mod::Log("iat-hook %s: expected=%p", tag, (void*)expected);
    /* scan the whole IAT region (SDL slot at +0x47e3c0) */
    for (uintptr_t va = 0x47e000; va < 0x495a00; va += 8) {
        uintptr_t* slot = (uintptr_t*)(ad2mod::s_module_base + va);
        if (*slot == expected) {
            uintptr_t orig = *slot;
            DWORD old = 0;
            if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) {
                ad2mod::Log("iat-hook %s: VirtualProtect FAILED %lu", tag, GetLastError());
                return 0;
            }
            *slot = (uintptr_t)hook;
            DWORD d = 0; VirtualProtect(slot, 8, old, &d);
            ad2mod::Log("iat-hook %s: PATCHED slot @base+%x (orig=%p)", tag, (unsigned)va, (void*)orig);
            return orig;
        }
    }
    ad2mod::Log("iat-hook %s: slot NOT FOUND in IAT 0x47e000..0x495a00", tag);
    return 0;
}

/* k32/kb variant: the game's slots may hold the kernel32 forwarder or the
   kernelbase target (match must use the runtime-resolved address of either). */
static uintptr_t HookIatImport(const char* name, void* hook)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    HMODULE kb  = GetModuleHandleA("kernelbase.dll");
    uintptr_t exp[2] = {
        k32 ? (uintptr_t)GetProcAddress(k32, name) : 0,
        kb  ? (uintptr_t)GetProcAddress(kb,  name) : 0,
    };
    for (int i = 0; i < 2; i++) {
        if (!exp[i]) continue;
        uintptr_t r = HookIatSlot(exp[i], hook, name);
        if (r) return r;
    }
    return 0;
}

static uintptr_t HookIatImportDll(const char* dll, const char* name, void* hook)
{
    HMODULE h = GetModuleHandleA(dll);
    uintptr_t exp = h ? (uintptr_t)GetProcAddress(h, name) : 0;
    return HookIatSlot(exp, hook, name);
}

void hooks_file::InstallFileApiHooks()
{
    uintptr_t o1 = HookIatImport("FindFirstFileW", Ad2FindFirstFileW);
    uintptr_t o2 = HookIatImport("CreateDirectoryW", Ad2CreateDirectoryW);
    if (o1) s_fff_orig = (FnFindFirstFileW)o1;
    if (o2) s_cdw_orig = (FnCreateDirectoryW)o2;
    /* activate only once the scan hooks are set, to avoid a torn state */
    if (o1 && o2) InterlockedExchange(&s_hook_active, 1);
    else ad2mod::Log("fileapi: hooks incomplete (fff=%p cdw=%p) -- logging disabled", (void*)o1, (void*)o2);

    /* supplementary capture (each independent; a missing slot just logs) */
    uintptr_t o3 = HookIatImport("CreateFileW", Ad2CreateFileW);
    if (o3) s_cfw_orig = (FnCreateFileW)o3;
    uintptr_t o4 = HookIatImport("CreateFile2", Ad2CreateFile2);
    if (o4) s_cf2_orig = (FnCreateFile2)o4;
    uintptr_t o5 = HookIatImport("GetFileAttributesW", Ad2GetFileAttributesW);
    if (o5) s_gfa_orig = (FnGetFileAttributesW)o5;
    uintptr_t o6 = HookIatImportDll("shell32.dll", "SHGetKnownFolderPath", Ad2SHGKF);
    if (o6) s_shgkf_orig = (FnSHGKF)o6;
    /* CRT fopen: try ucrtbase first, then msvcrt -- whichever address the
       loader actually bound into the game's IAT is the one that matches. */
    uintptr_t o7 = HookIatImportDll("ucrtbase.dll", "fopen", Ad2Fopen);
    if (!o7) o7 = HookIatImportDll("msvcrt.dll", "fopen", Ad2Fopen);
    if (o7) s_fopen_orig = (FnFopen)o7;
    ad2mod::Log("fileapi: summary fff=%p cdw=%p cfw=%p cf2=%p gfa=%p shgkf=%p fopen=%p",
        (void*)o1, (void*)o2, (void*)o3, (void*)o4, (void*)o5, (void*)o6, (void*)o7);
}
