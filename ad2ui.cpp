/* ad2ui.cpp -- in-game ImGui overlay for Astro Duel 2 (WREngine / cocos2d-x 3.17.X, SDL3)
 *
 * Current state (two-hook diagnostic build):
 *   IAT hooks: the SDL_GL_SwapWindow IAT hook (overlay) and the
 *   kernel32/shell32 file-API IAT log hooks (dir scans, mkdirs, opens).
 *   PLUS two LOG-ONLY inline entry hooks (trampolines, prologue-verified,
 *   zero behavioral change) for the level-pool pipeline:
 *     counthook  @ 0x1402ae190 WorldDataLoader::worldDataForType -- the
 *                 per-world level-count switch (which call site fired,
 *                 worldType, count).
 *     funnelhook @ 0x140030ee0 per-match versus level-load funnel -- the
 *                 caller's world/idx and the live pool int array.
 *   See re/LEVEL_POOL.md + re/CUSTOM_LEVELS.md. All diagnostics ->
 *   %LOCALAPPDATA%\AstroDuel2Mod\probe_ui.log.
 *
 *   UNPACK GATE: the DLL can now be loaded via a SUSPENDED start
 *   (ad2launch.exe: CreateProcess(CREATE_SUSPENDED) -> LoadLibrary ->
 *   ResumeThread). Because all 117 .tmx levels are preloaded in one ~450 ms
 *   block ~10 s after process start (Procmon capture, re/LEVEL_POOL.md),
 *   the worker polls until the VMP stub has unpacked the image in place
 *   (verified by the 20-byte prologue at RVA 0x26cce0) before patching
 *   anything. Manual inject (inject.exe into a running game) still works:
 *   the gate passes immediately.
 *
 * Render strategy (unchanged from milestone 1):
 *   - Worker thread (DllMain-spawned) waits for the game to settle, then
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
 *   - All diagnostics -> %LOCALAPPDATA%\AstroDuel2Mod\probe_ui.log
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <cctype>
#include <vector>
#include <string>

/* single WGL entry we need, declared by hand to avoid <GL/gl.h> clashing
   with imgui_impl_opengl3_loader.h's IMGL3W prototypes */
extern "C" HDC WINAPI wglGetCurrentDC(void);

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_opengl3_loader.h"

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */
static FILE* s_log = nullptr;
/* Log() is called from several threads (file-API hooks run on game threads,
   the watchdog on the worker thread) -- FILE* is not thread-safe, so every
   write is serialized. In practice the first call comes from the worker
   before any hook is installed, so the lazy init below is single-threaded. */
static CRITICAL_SECTION s_log_cs;
static volatile LONG s_log_cs_init = 0;

static void Log(const char* fmt, ...)
{
    if (InterlockedCompareExchange(&s_log_cs_init, 1, 0) == 0)
        InitializeCriticalSection(&s_log_cs);
    EnterCriticalSection(&s_log_cs);
    if (!s_log) {
        wchar_t dir[1024];
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", dir, 1024)) {
            char dirA[1024];
            WideCharToMultiByte(CP_UTF8, 0, dir, -1, dirA, 1024, nullptr, nullptr);
            char path[1200];
            char sub[1200];
            snprintf(sub, sizeof(sub), "%s\\AstroDuel2Mod", dirA);
            snprintf(path, sizeof(path), "%s\\probe_ui.log", sub);
            CreateDirectoryA(sub, nullptr);
            s_log = fopen(path, "a");
            if (s_log) setvbuf(s_log, nullptr, _IOFBF, 4096);
        }
    }
    FILE* f = s_log;
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap; va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fflush(f);
    }
    LeaveCriticalSection(&s_log_cs);
}

/* ------------------------------------------------------------------ */
/* globals                                                             */
/* ------------------------------------------------------------------ */
static uintptr_t s_module_base = 0;      /* main module (AstroDuel2.exe) */
static int (*s_orig_swap)(void*) = nullptr; /* original SDL_GL_SwapWindow */
/* SDL3 dropped per-platform SDL_GetWindowHWND; the HWND is a window
   property. Resolve the two exported functions we need to read it. */
typedef unsigned int (*FnSdlGetWindowProperties)(void* window);
typedef void* (*FnSdlGetPointerProperty)(unsigned int props, const char* name, void* def);
static FnSdlGetWindowProperties s_sdl_get_window_props = nullptr;
static FnSdlGetPointerProperty   s_sdl_get_pointer_prop = nullptr;
/* SDL_GetWindowSizeInPixels(SDL_Window*, int*, int*) -> SDL_bool */
typedef int (*FnSdlGetWindowSizeInPixels)(void* window, int* w, int* h);
static FnSdlGetWindowSizeInPixels s_sdl_get_size_px = nullptr;
typedef int (*FnSdlCursor)(int); /* SDL_ShowCursor/SDL_HideCursor (SDL3: int mode) */
static FnSdlCursor s_sdl_show_cursor = nullptr;
static FnSdlCursor s_sdl_hide_cursor = nullptr;
static void*       s_sdl_window = nullptr;
static const char kHwndPropKey[] = "SDL.window.win32.hwnd";
static volatile LONG s_hook_installed = 0;

static bool  s_imgui_ready = false;
static HWND  s_hwnd = nullptr;
static int   s_frame = 0;
static bool  s_gl_info_logged = false;
static bool  s_show_ui = true;           /* Ctrl+Alt+U toggles */

/* input edge-state */
static bool  s_mouse_btn[5] = {false,false,false,false,false};
#ifndef VK_MOUSEWHEEL
#define VK_MOUSEWHEEL 0x00E5
#endif

static bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

/* ------------------------------------------------------------------ */
/* versus level-pool editor (hook: re/LEVEL_POOL.md)                  */
/*                                                                     */
/* The per-match versus load (FUN_140030ee0) reads a caller-set int32  */
/* pool (mgr[+0x4a0..+0x4a8), count=len/4) and index (mgr[+0x380]) and */
/* fastfails if index >= count. An entry hook repoints the pool to our */
/* per-world int array and clamps the index -> shrink/replace/grow the */
/* live set with NO on-disk deletion and NO fastfail. Versus levels are*/
/* single .tmx files: res/levels/versus/<world>/<world><N>.tmx.        */
/* ------------------------------------------------------------------ */

#define POOL_WORLD_MAX  16      /* world ids 1..15 */
#define POOL_LEVEL_MAX  32      /* level numbers 1..31 */

struct PoolCfg { int count; int items[POOL_LEVEL_MAX]; };
static PoolCfg s_pools[POOL_WORLD_MAX];              /* live (committed) pools */
static int     s_draft_world = 0;                    /* world the draft checkboxes hold */
static bool    s_draft[POOL_LEVEL_MAX] = {false};    /* draft: level N included? */

static const char* PoolWorldName(int id)
{
    static const char* names[POOL_WORLD_MAX] = {
        "", "mines", "frontier", "flare", "darklab", "echocity", "highrise",
        "sunken", "graveyard", "gasrigs", "moonbase", "urspace", "void",
        "legacy", "up", "unsorted" };
    return (id >= 1 && id < POOL_WORLD_MAX) ? names[id] : "?";
}

static void PoolDraftLoad(int world)
{
    if (world < 1 || world >= POOL_WORLD_MAX) return;
    s_draft_world = world;
    PoolCfg* p = &s_pools[world];
    for (int n = 1; n < POOL_LEVEL_MAX; n++)
        s_draft[n] = false;
    for (int i = 0; i < p->count; i++)
        if (p->items[i] >= 1 && p->items[i] < POOL_LEVEL_MAX)
            s_draft[p->items[i]] = true;
}

/* level file check: game CWD first (getLevelString composes relative
   "levels/versus/..."), exe-dir\\res fallback */
static bool PoolFileExists(int world, int level, char* out, size_t outsz)
{
    const char* w = PoolWorldName(world);
    char stem[64];
    snprintf(stem, sizeof stem, "%s%d.tmx", w, level);
    if (GetFileAttributesA(stem) != INVALID_FILE_ATTRIBUTES) {
        snprintf(out, outsz, "%s", stem); return true;
    }
    char exedir[MAX_PATH];
    GetModuleFileNameA(nullptr, exedir, MAX_PATH);
    char* sl = strrchr(exedir, '\\'); if (sl) *sl = 0;
    char alt[1200];
    snprintf(alt, sizeof alt, "%s\\res\\levels\\versus\\%s\\%s", exedir, w, stem);
    if (GetFileAttributesA(alt) != INVALID_FILE_ATTRIBUTES) {
        snprintf(out, outsz, "%s", alt); return true;
    }
    snprintf(out, outsz, "%s", stem); return false;
}

/* commit: rebuild the live int array in ascending order; refuse if any
   checked level's file is missing or the set is empty */
static bool PoolCommit(int world, char* err, size_t errsz)
{
    if (world < 1 || world >= POOL_WORLD_MAX) { snprintf(err, errsz, "bad world"); return false; }
    int cnt = 0; int items[POOL_LEVEL_MAX];
    for (int n = 1; n < POOL_LEVEL_MAX; n++) {
        if (!s_draft[n]) continue;
        char path[1200];
        if (!PoolFileExists(world, n, path, sizeof path)) {
            snprintf(err, errsz, "file missing: %s  (levels/versus/%s/%s%d.tmx)",
                     path, PoolWorldName(world), PoolWorldName(world), n);
            return false;
        }
        items[cnt++] = n;
    }
    if (cnt == 0) { snprintf(err, errsz, "empty pool"); return false; }
    PoolCfg* p = &s_pools[world];
    memcpy(p->items, items, (size_t)cnt * sizeof(int));
    p->count = cnt;
    char list[256] = {0}; int o = 0;
    for (int i = 0; i < cnt && o < (int)sizeof(list) - 4; i++)
        o += snprintf(list + o, sizeof(list) - o, "%s%d", i ? "," : "", items[i]);
    Log("pool: COMMIT world=%d(%s) count=%d items=[%s]", world, PoolWorldName(world), cnt, list);
    return true;
}

/* file-API IAT hooks: runtime path log (match-start diagnostics)      */
/*                                                                     */
/* IAT-hooks the game's own file imports (kernel32/shell32, not VMP   */
/* code) and logs every dir scan, mkdir, and open on "interesting"    */
/* paths (levels/*.tmx, *_data.xml, res/...). Ground truth for WHICH   */
/* level files a match actually loads -- used to verify the versus     */
/* pool funnel (FUN_140030ee0) is on the real load path.               */
/* ------------------------------------------------------------------ */
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
    if (pattern && s_hook_active) Log("GAME FindFirstFileW: %ls", pattern);
    return s_fff_orig(pattern, fd);
}
static BOOL WINAPI Ad2CreateDirectoryW(const wchar_t* path, void* sa)
{
    BOOL r = s_cdw_orig(path, sa);
    if (path && s_hook_active) Log("GAME CreateDirectoryW: %ls  (ret=%d, err=%lu)", path, r, GetLastError());
    return r;
}

/* CreateFileW: catches level-file opens AND failures (the "lists but won't
   load" case). Filtered to interesting paths so res-loading noise stays out. */
static HANDLE WINAPI Ad2CreateFileW(const wchar_t* path, DWORD acc, DWORD share,
                                    void* sa, DWORD disp, DWORD flags, void* tmpl)
{
    HANDLE h = s_cfw_orig(path, acc, share, sa, disp, flags, tmpl);
    if (s_hook_active && PathInteresting(path))
        Log("GAME CreateFileW: %ls  (ret=%p%s)", path, (void*)h,
            h == INVALID_HANDLE_VALUE ? " FAILED" : "");
    return h;
}

static BOOL WINAPI Ad2CreateFile2(const wchar_t* path, void* ph, DWORD acc,
                                  DWORD opts, void* info)
{
    BOOL r = s_cf2_orig(path, ph, acc, opts, info);
    if (s_hook_active && PathInteresting(path))
        Log("GAME CreateFile2: %ls  (ret=%d%s)", path, r, r ? "" : " FAILED");
    return r;
}

/* GetFileAttributesW: the dir-exists pre-check (fn_cfg_dir_exists, vtable
   0x158) almost certainly probes through this -- if the scan never fires,
   the NOT-FOUND probe of the true base dir will show up here. */
static DWORD WINAPI Ad2GetFileAttributesW(const wchar_t* path)
{
    DWORD r = s_gfa_orig(path);
    if (s_hook_active && PathInteresting(path))
        Log("GAME GetFileAttributesW: %ls  (ret=0x%lx%s)", path, r,
            r == INVALID_FILE_ATTRIBUTES ? " NOT-FOUND" : "");
    return r;
}

/* fopen: THE real level-load path. cocos FileUtils::fillFileData
   (FUN_1403c686c: _stat64i32 + fopen(path,"rb")) loads every .tmx and
   *_data.xml -- it goes through the CRT import, invisible to the kernel32
   IAT hooks. Hook the game's own fopen import (ucrtbase or msvcrt, resolved
   at runtime; only the one actually in the game IAT will match a slot).
   char* paths here make a future rewrite trivial: change the pointer we pass
   to the original. */
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
        Log("GAME fopen: %s  [%s]  (ret=%p%s)", path, mode ? mode : "?",
            (void*)f, f ? "" : " FAILED");
    return f;
}

/* SHGetKnownFolderPath: the base-path getter (fn_cfg_get_base_path) calls
   this with FOLDERID_UserGameLocations. Logging every request + result is
   direct ground truth for the true base path (open question #3). */
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
            Log("GAME SHGetKnownFolderPath %s -> %ls", g, *out);
        else
            Log("GAME SHGetKnownFolderPath %s FAILED hr=0x%08lX", g, (unsigned long)hr);
    }
    return hr;
}

/* IAT-slot hook core (same mechanism as the SDL_GL_SwapWindow hook:
   overwrite the game's import-table slot, no trampolines, no patching of
   system-DLL code -- the earlier inline-hook attempt crashed the game).
   `expected` is the runtime-resolved address the slot should hold. */
static uintptr_t HookIatSlot(uintptr_t expected, void* hook, const char* tag)
{
    if (!expected) { Log("iat-hook %s: expected address NOT resolvable", tag); return 0; }
    Log("iat-hook %s: expected=%p", tag, (void*)expected);
    /* scan the whole IAT region (SDL slot at +0x47e3c0) */
    for (uintptr_t va = 0x47e000; va < 0x495a00; va += 8) {
        uintptr_t* slot = (uintptr_t*)(s_module_base + va);
        if (*slot == expected) {
            uintptr_t orig = *slot;
            DWORD old = 0;
            if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) {
                Log("iat-hook %s: VirtualProtect FAILED %lu", tag, GetLastError());
                return 0;
            }
            *slot = (uintptr_t)hook;
            DWORD d = 0; VirtualProtect(slot, 8, old, &d);
            Log("iat-hook %s: PATCHED slot @base+%x (orig=%p)", tag, (unsigned)va, (void*)orig);
            return orig;
        }
    }
    Log("iat-hook %s: slot NOT FOUND in IAT 0x47e000..0x495a00", tag);
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

static void InstallFileApiHooks()
{
    uintptr_t o1 = HookIatImport("FindFirstFileW", Ad2FindFirstFileW);
    uintptr_t o2 = HookIatImport("CreateDirectoryW", Ad2CreateDirectoryW);
    if (o1) s_fff_orig = (FnFindFirstFileW)o1;
    if (o2) s_cdw_orig = (FnCreateDirectoryW)o2;
    /* activate only once the scan hooks are set, to avoid a torn state */
    if (o1 && o2) InterlockedExchange(&s_hook_active, 1);
    else Log("fileapi: hooks incomplete (fff=%p cdw=%p) -- logging disabled", (void*)o1, (void*)o2);

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
    Log("fileapi: summary fff=%p cdw=%p cfw=%p cf2=%p gfa=%p shgkf=%p fopen=%p",
        (void*)o1, (void*)o2, (void*)o3, (void*)o4, (void*)o5, (void*)o6, (void*)o7);
}

/* ------------------------------------------------------------------ */
/* log-only entry hooks: level-pool pipeline diagnostics               */
/* (re/LEVEL_POOL.md, re/CUSTOM_LEVELS.md)                             */
/*                                                                     */
/* Two inline entry hooks on one RWX trampoline page. Per hook:        */
/*   entry : 48 B8 <imm64 tp> FF E0   (absolute jmp, 13 B)             */
/*   tp    : <stolen prologue> <arg save> <call Hook> <arg restore>    */
/*           <48 B8 <imm64 entry+N> FF E0>                            */
/* Prologue bytes are verified at runtime (REFUSE -> no patch, game    */
/* untouched) and re-verified in the worker loop. Zero behavioral      */
/* change: every register the body depends on at entry+N is restored;  */
/* the hooks only read and log.                                        */
/*                                                                     */
/* counthook  @ 0x1402ae190 WorldDataLoader::worldDataForType(out,wt) */
/*   Prologue is 9 B (40 53 push rbx / sub rsp,0x20 / 48 8B D9        */
/*   mov rbx,rcx) but the 13 B entry patch overruns it (cmp edx,0x10  */
/*   at +9, ja at +0xc), so the trampoline re-emits that cmp+ja       */
/*   (ja in absolute form, target 0x1402ae2bf) and lands at           */
/*   entry+0x12 (movsxd rax,edx),                                     */
/*   where rax is redefined before first use. At the call point        */
/*   rsp = S-0x28 (0 mod 16, S = post-retaddr-push rsp); the original  */
/*   retaddr sits at [rsp+0x28] (read into r8, the 3rd ABI arg); args  */
/*   are (rcx=out, rdx=wt, r8=retaddr). The hook may clobber rcx/rdx   */
/*   (ABI caller-saved), so `push rdx; push rcx` before the call and   */
/*   `pop rcx; pop rdx` after restore both -- the body from +0x12 uses */
/*   rdx (movsxd rax,rdx) and stores the result through rcx.           */
/*                                                                     */
/* funnelhook @ 0x140030ee0 (mgr)                                     */
/*   The 24 B prologue (mov rax,rsp / 3 shadow stores / 5 pushes) ends */
/*   exactly at entry+24, where `lea rbp,[rax-0x198]` + the movaps     */
/*   spill block need rax = pre-prologue rsp R, and the body first     */
/*   reads param_1 from rcx at +0xf56 (the prologue does not save      */
/*   it). `push rax; push rcx` before the call, popped back after,     */
/*   protects both. (The old build's epilogue `mov rcx,rax` left rax   */
/*   = hook return = mgr -- wild movaps writes to [mgr-0x38..] and a   */
/*   frame base of mgr-0x198. Never fired in a recorded session; this  */
/*   push/pop design removes the hazard entirely.)                     */
/* ------------------------------------------------------------------ */

static BYTE* s_hook_page = nullptr;     /* one RWX page, both trampolines */

struct EntryPatch {
    const char* tag; uintptr_t rva; bool patched; int bad;
};
static EntryPatch s_ep[2] = {
    { "counthook",  0x2ae190, false, 0 },
    { "funnelhook", 0x30ee0,  false, 0 },
};

/* append `jmp target` (absolute, 13 B) at tp+off; returns new off */
static size_t EmitAbsJmp(BYTE* tp, size_t off, uintptr_t target)
{
    tp[off] = 0x48; tp[off+1] = 0xB8;
    memcpy(tp + off + 2, &target, 8);
    tp[off+10] = 0xFF; tp[off+11] = 0xE0;
    return off + 13;
}

/* append `call fn` at tp+off: E8 rel32 when in range, else absolute
   `jmp rax; ret` is NOT used (ret would go to the wrong place) -- fall
   back to `mov rax,fn; call rax` (12 B, clobbers rax only). */
static size_t EmitCall(BYTE* tp, size_t off, void* fn)
{
    int64_t d = (int64_t)fn - (int64_t)(tp + off + 5);
    if (d >= INT32_MIN && d <= INT32_MAX) {
        int32_t r = (int32_t)d;
        tp[off] = 0xE8; memcpy(tp + off + 1, &r, 4);
        return off + 5;
    }
    tp[off] = 0x48; tp[off+1] = 0xB8; memcpy(tp + off + 2, &fn, 8);
    tp[off+10] = 0xFF; tp[off+11] = 0xD0;       /* call rax */
    return off + 12;
}

/* append `ja target` at tp+off as `76 0C; 48 B8 imm64; FF E0` (14 B).
   rel32 can't span the DLL<->game-module distance (tens of TB), so the
   branch is inverted (jbe +12 = NOT ja) and the taken path uses an
   absolute jmp. Semantics after `cmp edx,0x10`: CF=0,ZF=0 (edx>0x10)
   -> target; else fallthrough. */
static size_t EmitJaAbs(BYTE* tp, size_t off, uintptr_t target)
{
    tp[off]   = 0x76; tp[off+1] = 0x0C;              /* jbe +12 (skip) */
    tp[off+2] = 0x48; tp[off+3] = 0xB8;
    memcpy(tp + off + 4, &target, 8);
    tp[off+12] = 0xFF; tp[off+13] = 0xE0;
    return off + 14;
}

/* verify prologue + emit the 13 B absolute jmp at entry */
static bool PatchEntry(EntryPatch& ep, const BYTE* want, size_t wantLen, BYTE* tp)
{
    uint8_t* entry = (uint8_t*)(s_module_base + ep.rva);
    if (memcmp(entry, want, wantLen) != 0) {
        Log("%s: REFUSED -- prologue mismatch at +0x%lx (%02x %02x %02x %02x ..)",
            ep.tag, (unsigned long)ep.rva, entry[0], entry[1], entry[2], entry[3]);
        return false;
    }
    DWORD oldProt = 0;
    uint8_t* page = (uint8_t*)((uintptr_t)entry & ~0xFFFULL);
    if (!VirtualProtect(page, 4096, PAGE_READWRITE, &oldProt)) {
        Log("%s: VirtualProtect(%p) FAILED %lu", ep.tag, (void*)entry, GetLastError());
        return false;
    }
    entry[0] = 0x48; entry[1] = 0xB8;
    memcpy(entry + 2, &tp, 8);
    entry[10] = 0xFF; entry[11] = 0xE0;
    DWORD dummy = 0;
    VirtualProtect(page, 4096, oldProt, &dummy);
    ep.patched = true;
    Log("%s: INSTALLED entry=%p tp=%p", ep.tag, (void*)entry, (void*)tp);
    return true;
}

/* ---- counthook ---- */
/* WorldDataLoader::worldDataForType writes {d0,d1,d2,d3} into the
   caller's 16 B buffer (rcx); d3 (offset 0xc) is the per-world level
   COUNT -- both key call sites loop on *(int*)(out+0xc): the preload
   loop (call at 0x140013d5a) and the picker (call at 0x1400327ee).
   The hook fires at entry, before the buffer is filled, so it logs
   worldType + the original return address (which call site) and the
   count computed from a copy of the switch below. */
static int WorldDataCount(int t)
{
    switch (t) {
    case 1:  case 5:  case 7:              return 9;
    case 2:  case 3:  case 4:  case 6:     
    case 8:  case 9:                       return 8;
    case 0:  case 0xc: case 0xe:           return 1;
    case 0xa: case 0xb: case 0xd:          
    case 0xf: case 0x10:                   return 0xd;
    default:                               return -1; /* game aborts */
    }
}

static const char* CountRetSite(uintptr_t ret)
{
    if (!s_module_base) return "";
    uintptr_t rva = ret - s_module_base;   /* ret = call site + 5 */
    switch (rva) {
    case 0x13d5f: return "preload loop";        /* call 0x140013d5a, FUN_140013c00 */
    case 0x327f3: return "picker/match";        /* call 0x1400327ee */
    case 0x8d3a4: return "coop pathbuilder";    /* call 0x14008d39f */
    case 0x19745c: return "init";               /* call 0x140197457 */
    case 0x26bab8: return "getLevelString";     /* call 0x14026bab3 */
    default: return "other";
    }
}

static void CountHook(void* out, int worldType, uintptr_t retAddr)
{
    uintptr_t ret = retAddr;
    Log("counthook: world=%d(%s) count=%d site=%s(+0x%lx) out=%p",
        worldType, PoolWorldName(worldType), WorldDataCount(worldType),
        CountRetSite(ret), (unsigned long)(ret ? ret - s_module_base : 0), out);
}

static bool InstallCountHook()
{
    const uintptr_t kRva = s_ep[0].rva;
    const uintptr_t kCont = kRva + 0x12;                 /* movsxd rax,edx  */
    const uintptr_t kJaTarget = kRva + 0x12f;            /* 0x1402ae2bf     */
    const BYTE want[9] = { 0x40,0x53, 0x48,0x83,0xEC,0x20, 0x48,0x8B,0xD9 };
    uint8_t* entry = (uint8_t*)(s_module_base + kRva);

    BYTE* tp = s_hook_page;
    memcpy(tp, entry, 9);                                 /* +0x00 stolen prologue */
    tp[9] = 0x83; tp[10] = 0xFA; tp[11] = 0x10;           /* +0x09 cmp edx,0x10 */
    size_t o = EmitJaAbs(tp, 12, s_module_base + kJaTarget); /* +0x0c ja (abs form) */
    /* rsp = S-0x28 (0 mod 16) here. r8 = retaddr for the hook.          */
    tp[o]=0x4C; tp[o+1]=0x8B; tp[o+2]=0x44; tp[o+3]=0x24; tp[o+4]=0x28; /* mov r8,[rsp+0x28] */
    o += 5;
    tp[o] = 0x52; o += 1;                                 /* push rdx (protect worldType) */
    tp[o] = 0x51; o += 1;                                 /* push rcx (protect out)     */
    o = EmitCall(tp, o, (void*)CountHook);                /* call; rcx=out rdx=wt r8=ret  */
    tp[o] = 0x59; o += 1;                                 /* pop rcx (out)          */
    tp[o] = 0x52; o += 1;                                 /* pop rdx -> rsp = S-0x28 again */
    EmitAbsJmp(tp, o, s_module_base + kCont);
    return PatchEntry(s_ep[0], want, 9, tp);
}

/* ---- funnelhook ---- */
static void FunnelHook(void* mgr)
{
    uintptr_t m = (uintptr_t)mgr;
    if (!mgr || m < 0x10000 || (m & 7)) {
        Log("funnelhook: bad mgr %p -- skipped", (void*)m); return;
    }
    uintptr_t b = s_module_base;
    int world = (int)*(volatile int32_t*)(b + 0x5DE484);   /* DAT_1405de484 */
    int gate  = (int)*(volatile uint8_t*)(b + 0x5DE4CF);   /* DAT_1405de4cf */
    int idx   = (int)*(volatile int32_t*)(m + 0x380);      /* caller's pick */
    uintptr_t ps = *(volatile uintptr_t*)(m + 0x4A0);      /* poolStart     */
    uintptr_t pe = *(volatile uintptr_t*)(m + 0x4A8);      /* poolEnd       */
    Log("funnelhook: world=%d(%s) gate=%d idx=%d pool=%p..%p",
        world, PoolWorldName(world), gate, idx, (void*)ps, (void*)pe);
    if (ps >= 0x10000 && pe >= ps && (pe - ps) <= 256) {
        int n = (int)((pe - ps) / 4);
        if (n > 24) n = 24;
        char buf[256]; int out = 0;
        const int32_t* arr = (const int32_t*)ps;
        for (int i = 0; i < n && out < 240; i++)
            out += snprintf(buf + out, sizeof(buf) - out, "%s%d", i ? "," : "", arr[i]);
        Log("funnelhook:   poolN=%d pool=[%s]", n, buf);
    } else {
        Log("funnelhook:   pool pointers out of range -- not dumping");
    }
}

static bool InstallFunnelHook()
{
    const uintptr_t kRva = s_ep[1].rva;
    const uintptr_t kCont = kRva + 0x18;                   /* lea rbp,[rax-0x198] */
    const BYTE want[24] = { 0x48,0x8b,0xc4, 0x48,0x89,0x58,0x10, 0x48,0x89,0x70,0x18,
                            0x48,0x89,0x78,0x20, 0x55, 0x41,0x54, 0x41,0x55,
                            0x41,0x56, 0x41,0x57 };
    uint8_t* entry = (uint8_t*)(s_module_base + kRva);

    BYTE* tp = s_hook_page + 0x100;                        /* second slot on the page */
    memcpy(tp, entry, 24);                                 /* +0x00 stolen prologue   */
    /* rsp = R-0x28 (0 mod 16) here; rax = R, rcx = mgr. Two pushes     */
    /* protect both across the call and keep the call point 0 mod 16,  */
    /* so the hook gets the required 8 mod 16. Popped back before the  */
    /* jmp (that stack region is scratch -- the body's own spills      */
    /* overwrite it after `sub rsp,0x270`).                            */
    tp[24] = 0x50;                                         /* +0x18 push rax          */
    tp[25] = 0x51;                                         /* +0x19 push rcx          */
    size_t o = EmitCall(tp, 26, (void*)FunnelHook);        /* +0x1a call FunnelHook   */
    tp[o] = 0x59; o += 1;                                  /* +0x1f pop rcx (mgr)     */
    tp[o] = 0x58; o += 1;                                  /* +0x20 pop rax (R)       */
    EmitAbsJmp(tp, o, s_module_base + kCont);
    return PatchEntry(s_ep[1], want, 24, tp);
}

static bool InstallEntryHooks()
{
    s_hook_page = (BYTE*)VirtualAlloc(nullptr, 0x1000,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!s_hook_page) { Log("entryhooks: VirtualAlloc FAILED %lu", GetLastError()); return false; }
    bool a = InstallCountHook();
    bool c = InstallFunnelHook();
    Log("entryhooks: counthook=%s funnelhook=%s", a ? "ok" : "REFUSED", c ? "ok" : "REFUSED");
    return a || c;
}

/* worker-loop re-verify: the patch must still be our 13 B absolute jmp */
static void VerifyEntryPatches()
{
    for (int i = 0; i < 2; i++) {
        EntryPatch& ep = s_ep[i];
        if (!ep.patched) continue;
        uint8_t* p = (uint8_t*)(s_module_base + ep.rva);
        if (p[0] != 0x48 || p[1] != 0xB8 ||
            *(uint64_t*)(p + 2) != (uint64_t)(i == 0 ? s_hook_page : s_hook_page + 0x100)) {
            if (ep.bad++ < 3)
                Log("%s: entry patch OVERWRITTEN (bytes %02x %02x %02x ..) -- VMP? "
                    "hooks will misfire until reinstalled", ep.tag, p[0], p[1], p[2]);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Basic .text patch test: mines count 9 -> 2 (1 byte, no trampoline)  */
/* FUN_1402ae190 worldDataForType, case worldType==1 (mines):          */
/*   0x1402ae1c4: B8 09 00 00 00  mov eax, 9   (count -> out[3])       */
/*   0x1402ae1c5 holds the `9`. Flip to 0x02 (2). The preload loop     */
/*   (FUN_140013c00) and the picker both iterate i = 1..*(int*)(out+0xC) */
/*   so both see 2: pool = [1,2]. Tests the match-start pick counter   */
/*   in 2P vs 3P: does pickIndex advance+wrap across matches, or is it  */
/*   random? Level = pool[pickIndex] = pickIndex + 1. count=1 froze on  */
/*   the 2nd match (pickIndex advanced past the 1-element pool ->       */
/*   "invalid vector subscript" halt); count=2 should survive the      */
/*   2-player counter and expose the 3-player behavior.                */
/* ------------------------------------------------------------------ */
static uint8_t s_mc_orig = 0;
static int s_mc_bad = 0;

static uint8_t* MinesCountByte()
{
    return s_module_base ? (uint8_t*)(s_module_base + 0x2ae1c5) : nullptr;
}

static bool PatchMinesCount()
{
    uint8_t* p = MinesCountByte();
    if (!p) { Log("minescount: no module base"); return false; }
    uint8_t* inst = p - 1;                        /* B8 xx 00 00 00 */
    if (inst[0] == 0xB8 && p[0] == 0x02) {
        Log("minescount: already patched (0x02) -- nothing to do");
        s_mc_orig = 0x09;
        return true;
    }
    if (!(inst[0] == 0xB8 && p[0] == 0x09 && p[1] == 0 && p[2] == 0 && p[3] == 0)) {
        Log("minescount: REFUSED -- bytes at +0x2ae1c4: %02x %02x %02x %02x %02x (want B8 09 00 00 00)",
            inst[0], p[0], p[1], p[2], p[3]);
        return false;
    }
    uint8_t* page = (uint8_t*)((uintptr_t)inst & ~0xFFFULL);
    DWORD old = 0;
    if (!VirtualProtect(page, 4096, PAGE_READWRITE, &old)) {
        Log("minescount: VirtualProtect FAILED %lu", GetLastError());
        return false;
    }
    s_mc_orig = p[0];
    p[0] = 0x02;
    DWORD dummy = 0;
    VirtualProtect(page, 4096, old, &dummy);
    Log("minescount: PATCHED +0x2ae1c5 0x%02x -> 0x02 (mines pool 9 -> 2)", s_mc_orig);
    return true;
}

static void VerifyMinesCount()
{
    uint8_t* p = MinesCountByte();
    if (p && p[0] != 0x02 && s_mc_bad++ < 3)
        Log("minescount: byte REVERTED to 0x%02x (want 0x02) -- VMP re-wrote .text?", p[0]);
}

/* ------------------------------------------------------------------ */
/* find the game window                                                */
/* ------------------------------------------------------------------ */
struct WinCand { HWND h; int area; };
static BOOL CALLBACK EnumWinProc(HWND h, LPARAM lp)
{
    WinCand* best = (WinCand*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    RECT r; GetClientRect(h, &r);
    int area = (r.right - r.left) * (r.bottom - r.top);
    if (area > best->area) { best->area = area; best->h = h; }
    return TRUE;
}

static HWND FindGameWindow()
{
    WinCand best = { nullptr, -1 };
    EnumWindows(EnumWinProc, (LPARAM)&best);
    return best.h;
}

/* ------------------------------------------------------------------ */
/* hook                                                                */
/* ------------------------------------------------------------------ */
static void ImGuiInit()
{
    Log("ImGuiInit: hwnd=%p dc=%p", (void*)s_hwnd, wglGetCurrentDC());
    if (!s_hwnd) return;
    if (!wglGetCurrentDC()) { Log("ImGuiInit: no GL context yet, deferring"); return; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    /* io.DisplaySize defaults to (-1,-1) in 1.90.9 -- seed a valid value so
       even a NewFrame before FeedInput could not assert. */
    {
        RECT rc = { 0, 0, 0, 0 };
        if (GetClientRect(s_hwnd, &rc) && rc.right > 0 && rc.bottom > 0)
            io.DisplaySize = ImVec2((float)rc.right, (float)rc.bottom);
        else
            io.DisplaySize = ImVec2(1920.0f, 1080.0f);
    }

    /* HiDPI: we lay out in PHYSICAL pixels (DisplaySize = GL viewport, e.g.
       3840x2160) with FramebufferScale (1,1). The default 13px font is then
       physically tiny at 4K. Rasterize the font at the monitor's DPI so the
       UI is physically sized right (13px * 2.0 = 26px at 200%). GetClientRect
       is unreliable on the SDL fullscreen window, so read DPI from the hwnd. */
    {
        HWND w = s_hwnd ? s_hwnd : NULL;
        HDC  hdc = GetDC(w);
        int dpi = (hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96);
        if (hdc) ReleaseDC(w, hdc);
        if (dpi <= 0) dpi = 96;
        float scale = dpi / 96.0f;
        if (scale < 1.0f) scale = 1.0f;
        ImFontConfig fc;
        fc.SizePixels = 13.0f * scale;
        io.Fonts->AddFontDefault(&fc);
        Log("ImGuiInit: dpi=%d scale=%.2f font=%.0fpx", dpi, scale, fc.SizePixels);
    }

    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 4.0f;

    /* GLSL 120: the game gates on "Ready for OpenGL 2.1"; #version 120
       compiles on 2.1 compat and desktop 3.x alike. */
    if (!ImGui_ImplOpenGL3_Init("#version 120")) {
        Log("ImGuiInit: ImGui_ImplOpenGL3_Init FAILED (shader compile?)");
        return;
    }
    s_imgui_ready = true;
    Log("ImGuiInit: OK (GLSL #version 120)");
}

static void FeedInput()
{
    ImGuiIO& io = ImGui::GetIO();

    /* delta time via system timer */
    static UINT64 last = 0;
    UINT64 now = GetTickCount64();
    if (last) io.DeltaTime = (float)((now - last) / 1000.0);
    last = now;
    if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0f / 60.0f;

    /* display size: the game sets glViewport every frame before SwapWindow,
       so the GL viewport IS the true drawable size (physical pixels, DPI-
       agnostic). Fallbacks: SDL pixel size, then logical client rect. */
    int vw[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_VIEWPORT, vw);
    RECT rc = { 0, 0, 0, 0 };
    GetClientRect(s_hwnd, &rc);
    int sw = 0, sh = 0;
    if (s_sdl_window && s_sdl_get_size_px) s_sdl_get_size_px(s_sdl_window, &sw, &sh);
    static bool s_size_logged = false;
    if (!s_size_logged) {
        s_size_logged = true;
        Log("sizes: gl_vp=%dx%d sdl_px=%dx%d client=%dx%d", vw[2], vw[3], sw, sh, rc.right, rc.bottom);
    }
    if (vw[2] > 0 && vw[3] > 0)
        io.DisplaySize = ImVec2((float)vw[2], (float)vw[3]);
    else if (sw > 0 && sh > 0)
        io.DisplaySize = ImVec2((float)sw, (float)sh);
    else if (rc.right > 0 && rc.bottom > 0)
        io.DisplaySize = ImVec2((float)rc.right, (float)rc.bottom);
    else if (io.DisplaySize.x < 1.0f)
        io.DisplaySize = ImVec2(1920.0f, 1080.0f);

    /* mouse pos (client coords, scaled logical->physical) */
    POINT p; GetCursorPos(&p);
    ScreenToClient(s_hwnd, &p);
    {
        float sx = (rc.right > 0) ? io.DisplaySize.x / (float)rc.right : 1.0f;
        float sy = (rc.bottom > 0) ? io.DisplaySize.y / (float)rc.bottom : 1.0f;
        io.AddMousePosEvent((float)p.x * sx, (float)p.y * sy);
    }

    /* mouse buttons (edge via AddMouseButtonEvent: it takes a bool state
       per event and imgui dedupes; simplest is to send current state only
       on change) */
    static const int vkmap[5] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
    for (int i = 0; i < 5; i++) {
        bool d = KeyDown(vkmap[i]);
        if (d != s_mouse_btn[i]) { io.AddMouseButtonEvent(i, d); s_mouse_btn[i] = d; }
    }

    /* keyboard: map common VKs to ImGuiKey; send state every frame via
       AddKeyEvent (imgui handles repeated same-state events) */
    struct { int vk; ImGuiKey k; } kmap[] = {
        { VK_ESCAPE, ImGuiKey_Escape }, { VK_TAB, ImGuiKey_Tab },
        { VK_RETURN, ImGuiKey_Enter },   { VK_BACK, ImGuiKey_Backspace },
        { VK_DELETE, ImGuiKey_Delete },  { VK_SPACE, ImGuiKey_Space },
        { VK_LEFT, ImGuiKey_LeftArrow }, { VK_RIGHT, ImGuiKey_RightArrow },
        { VK_UP, ImGuiKey_UpArrow },     { VK_DOWN, ImGuiKey_DownArrow },
        { VK_SHIFT, ImGuiKey_LeftShift }, { VK_CONTROL, ImGuiKey_LeftCtrl },
        { VK_MENU, ImGuiKey_LeftAlt },   { VK_LWIN, ImGuiKey_LeftSuper },
        { 0x41, ImGuiKey_A }, { 0x42, ImGuiKey_B }, { 0x43, ImGuiKey_C },
        { 0x44, ImGuiKey_D }, { 0x45, ImGuiKey_E }, { 0x46, ImGuiKey_F },
        { 0x47, ImGuiKey_G }, { 0x48, ImGuiKey_H }, { 0x49, ImGuiKey_I },
        { 0x4A, ImGuiKey_J }, { 0x4B, ImGuiKey_K }, { 0x4C, ImGuiKey_L },
        { 0x4D, ImGuiKey_M }, { 0x4E, ImGuiKey_N }, { 0x4F, ImGuiKey_O },
        { 0x50, ImGuiKey_P }, { 0x51, ImGuiKey_Q }, { 0x52, ImGuiKey_R },
        { 0x53, ImGuiKey_S }, { 0x54, ImGuiKey_T }, { 0x55, ImGuiKey_U },
        { 0x56, ImGuiKey_V }, { 0x57, ImGuiKey_W }, { 0x58, ImGuiKey_X },
        { 0x59, ImGuiKey_Y }, { 0x5A, ImGuiKey_Z },
        { 0x30, ImGuiKey_0 }, { 0x31, ImGuiKey_1 }, { 0x32, ImGuiKey_2 },
        { 0x33, ImGuiKey_3 }, { 0x34, ImGuiKey_4 }, { 0x35, ImGuiKey_5 },
        { 0x36, ImGuiKey_6 }, { 0x37, ImGuiKey_7 }, { 0x38, ImGuiKey_8 },
        { 0x39, ImGuiKey_9 },
    };
    for (auto& e : kmap)
        io.AddKeyEvent(e.k, KeyDown(e.vk));

    io.AddMouseWheelEvent(0.0f, (float)((SHORT)(GetAsyncKeyState(VK_MOUSEWHEEL) >> 8)) / 120.0f);
}

static void DrawUI()
{
    ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("ad2mod overlay", nullptr, ImGuiWindowFlags_NoCollapse);

    ImGui::Text("frame: %d     (Ctrl+Alt+U toggles this overlay)", s_frame);
    ImGui::Separator();

    /* ---------------- versus level-pool editor ---------------- */
    ImGui::Text("VERSUS LEVEL POOL   (local play only)");
    ImGui::TextDisabled("shrink/replace/grow the live map set; commit validates files exist");
    static int sel_world = 1;
    if (ImGui::BeginCombo("##poolworld", PoolWorldName(sel_world))) {
        for (int w = 1; w < POOL_WORLD_MAX; w++) {
            bool sel = (ImGui::Selectable(PoolWorldName(w), sel_world == w) && sel_world != w);
            if (sel) { sel_world = w; PoolDraftLoad(w); }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (s_draft_world != sel_world) PoolDraftLoad(sel_world);
    if (ImGui::Button("load current##pl")) PoolDraftLoad(sel_world);

    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            int n = row * 4 + col + 1;
            if (n >= POOL_LEVEL_MAX) break;
            char label[64];
            bool exists = false;
            if (n <= 12) {
                char path[1200];
                exists = PoolFileExists(sel_world, n, path, sizeof path);
                snprintf(label, sizeof label, "%s%d%s", PoolWorldName(sel_world), n,
                         exists ? "" : " \u2717");
            } else {
                snprintf(label, sizeof label, "(slot %d)", n);
            }
            if (col) ImGui::SameLine();
            if (n <= 12)
                ImGui::Checkbox(label, &s_draft[n]);
            else
                ImGui::TextDisabled("--");
        }
    }
    static char pl_err[256] = {0};
    ImGui::SameLine();
    if (ImGui::Button("commit pool##pl")) {
        pl_err[0] = 0;
        PoolCommit(sel_world, pl_err, sizeof pl_err);
    }
    if (pl_err[0])
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "commit refused: %s", pl_err);
    if (s_pools[sel_world].count > 0) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "committed: %d levels", s_pools[sel_world].count);
    } else {
        ImGui::TextDisabled("no pool committed -- stock behaviour");
    }
    ImGui::End();
}

/* the hook: same ABI as SDL_GL_SwapWindow(SDL_Window*) -> int */
static int Ad2HookedSwap(void* window)
{
    if (s_hook_installed) {
        /* the hook parameter IS the game's SDL_Window -- authoritative HWND
           source (better than window enumeration). Read it via SDL3's
           property API: GetWindowProperties -> GetPointerProperty("...hwnd") */
        if (window) s_sdl_window = window;
        if (!s_hwnd && window && s_sdl_get_window_props && s_sdl_get_pointer_prop) {
            unsigned int props = s_sdl_get_window_props(window);
            if (props) s_hwnd = (HWND)s_sdl_get_pointer_prop(props, kHwndPropKey, nullptr);
        }
        if (!s_hwnd) s_hwnd = FindGameWindow(); /* last-resort fallback */
        if (!s_imgui_ready) ImGuiInit();
        if (s_imgui_ready) {
            s_frame++;
            ImGui_ImplOpenGL3_NewFrame();
            FeedInput();      /* update io.DisplaySize + queue input BEFORE NewFrame */
            if (!s_gl_info_logged) {
                s_gl_info_logged = true;
                Log("first frame: window=%p display=%dx%d", (void*)window,
                    (int)ImGui::GetIO().DisplaySize.x, (int)ImGui::GetIO().DisplaySize.y);
            }
            ImGui::NewFrame();
            if (s_show_ui) DrawUI();
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }
    }
    return s_orig_swap ? s_orig_swap(window) : 0;
}

/* ------------------------------------------------------------------ */
/* worker: locate IAT slot, patch, poll hotkey                         */
/* ------------------------------------------------------------------ */

/* UNPACK GATE -- suspended-inject support -------------------------- */
/* When ad2ui.dll is loaded via a suspended start (ad2launch.exe), the
   VMP stub has NOT run yet: the game's sections are still ciphertext,
   so every runtime-byte check (prologues, IAT slot contents) sees the
   packed image. Poll until the unpacked image is verifiable -- we use
   the 20-byte shadow-register-store prologue at RVA 0x26cce0
   (capstone-verified against the unpacked PE; its presence at runtime
   is confirmed by the live log history). In the manual-inject flow
   (game already running) the gate passes on the first check. */
static bool WaitUnpacked()
{
    const BYTE want[20] = { 0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x6C,0x24,0x10,
                            0x48,0x89,0x74,0x24,0x18, 0x48,0x89,0x7C,0x24,0x20 };
    const uint8_t* p = (const uint8_t*)(s_module_base + 0x26cce0);
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    LARGE_INTEGER frq; QueryPerformanceFrequency(&frq);
    int logged = 0;
    for (int i = 0; i < 450; i++) {                 /* 450 * 200 ms = 90 s */
        if (memcmp(p, want, 20) == 0) {
            LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
            double dt = (double)(t1.QuadPart - t0.QuadPart) / frq.QuadPart;
            Log("unpackgate: UNPACKED after %.2fs -- runtime code bytes verified", dt);
            return true;
        }
        if (i % 25 == 0 && logged++ < 10)
            Log("unpackgate: still packed (attempt %d) ...", i);
        Sleep(200);
    }
    Log("unpackgate: TIMEOUT -- bytes at +0x26cce0 = %02x %02x %02x .. %02x %02x %02x",
        p[0], p[1], p[2], p[17], p[18], p[19]);
    return false;
}

static DWORD WINAPI Worker(LPVOID)
{
    s_module_base = (uintptr_t)GetModuleHandleA(nullptr);

    Log("session: ad2ui build %s %s | pid=%lu | module_base=%p",
        __DATE__, __TIME__, (unsigned long)GetCurrentProcessId(), (void*)s_module_base);
    {
        /* the game's own command line -- ground truth for whether
           -custom/-dropbox were actually passed to this session */
        typedef long (WINAPI *FnReadProcCmdLine)(HANDLE, DWORD, PWSTR, DWORD, PDWORD);
        FnReadProcCmdLine rpc = nullptr;
        HMODULE hk = GetModuleHandleA("kernel32.dll");
        if (hk) rpc = (FnReadProcCmdLine)GetProcAddress(hk, "ReadProcessCommandLine");
        if (!rpc) { hk = GetModuleHandleA("kernelbase.dll"); if (hk) rpc = (FnReadProcCmdLine)GetProcAddress(hk, "ReadProcessCommandLine"); }
        if (rpc) {
            wchar_t cl[2048] = {0};
            DWORD len = 0;
            if (rpc(GetCurrentProcess(), 0, cl, 2048, &len) == 0)
                Log("session: cmdline=%ls", cl);
            else
                Log("session: ReadProcessCommandLine FAILED (err=%lu)", GetLastError());
        } else Log("session: ReadProcessCommandLine not resolvable");
    }

    /* suspended-inject: wait until VMP has unpacked the image in place
       before trusting ANY runtime bytes (IAT slot scan, patches). */
    if (!WaitUnpacked()) {
        Log("worker: ABORTING -- game not unpacked, refusing to patch packed bytes");
        return 1;
    }

    /* SDL3.dll loads early in the game's own startup; under suspended
       inject it may not exist yet, so poll. */
    HMODULE s3 = nullptr;
    for (int i = 0; i < 450 && !s3; i++) {          /* up to 90 s */
        s3 = GetModuleHandleA("SDL3.dll");
        if (!s3) Sleep(200);
    }
    Log("worker: module_base=%p SDL3.dll=%p", (void*)s_module_base, (void*)s3);
    if (!s3) { Log("worker: SDL3.dll NOT loaded after 90s -- giving up"); return 1; }

    s_sdl_get_window_props = (FnSdlGetWindowProperties)GetProcAddress(s3, "SDL_GetWindowProperties");
    s_sdl_get_pointer_prop = (FnSdlGetPointerProperty)GetProcAddress(s3, "SDL_GetPointerProperty");
    s_sdl_get_size_px = (FnSdlGetWindowSizeInPixels)GetProcAddress(s3, "SDL_GetWindowSizeInPixels");
    s_sdl_show_cursor = (FnSdlCursor)GetProcAddress(s3, "SDL_ShowCursor");
    s_sdl_hide_cursor = (FnSdlCursor)GetProcAddress(s3, "SDL_HideCursor");
    Log("worker: GetWindowProperties=%p GetPointerProperty=%p GetWindowSizeInPixels=%p ShowCursor=%p HideCursor=%p",
        (void*)s_sdl_get_window_props, (void*)s_sdl_get_pointer_prop,
        (void*)s_sdl_get_size_px, (void*)s_sdl_show_cursor, (void*)s_sdl_hide_cursor);
    uintptr_t real = (uintptr_t)GetProcAddress(s3, "SDL_GL_SwapWindow");
    Log("worker: SDL_GL_SwapWindow = %p", (void*)real);
    if (!real) { Log("worker: GetProcAddress failed (%lu)", GetLastError()); return 1; }

    /* scan the static-IAT region (RVA 0x47e028..0x47eba0) for the slot.
       The game may fill slots at load or resolve later; retry until found. */
    uintptr_t* slot = nullptr;
    for (int attempt = 0; attempt < 100 && !slot; attempt++) {
        uintptr_t* begin = (uintptr_t*)(s_module_base + 0x47e028);
        uintptr_t* end   = (uintptr_t*)(s_module_base + 0x47eba0);
        for (uintptr_t* it = begin; it < end; it++) {
            if (*it == real) { slot = it; break; }
        }
        if (!slot) Sleep(200);
    }
    if (!slot) {
        Log("worker: IAT slot for SwapWindow NOT FOUND in 0x47e028..0x47eba0");
        return 1;
    }
    Log("worker: IAT slot at %p (content=%p)", (void*)slot, (void*)*slot);

    /* IAT lives in .rdata (read-only) -- flip the page, patch, restore. */
    s_orig_swap = (int(*)(void*))*slot;
    uintptr_t page = (uintptr_t)slot & ~(uintptr_t)0xFFF;
    DWORD oldProt = 0;
    if (!VirtualProtect((LPVOID)page, 0x1000, PAGE_READWRITE, &oldProt)) {
        Log("worker: VirtualProtect(%p) FAILED: %lu", (void*)page, GetLastError());
        return 1;
    }
    *slot = (uintptr_t)Ad2HookedSwap;
    DWORD dummy = 0;
    VirtualProtect((LPVOID)page, 0x1000, oldProt, &dummy);
    InterlockedExchange(&s_hook_installed, 1);
    Log("worker: HOOK INSTALLED (orig=%p, page %p prot 0x%lx->restored)",
        (void*)s_orig_swap, (void*)page, oldProt);
    Log("worker: level-pool overlay ready (Ctrl+Alt+U toggles UI)\n");

    /* file-API IAT hooks: runtime path log (match-start diagnostics) */
    InstallFileApiHooks();

    /* log-only entry hooks (counthook + funnelhook) -- must be in before
       the ~9 s preload block, and they are: this runs ~1 s after start.
       DISABLED 2026-10-17: startup crash in the first live two-hook run
       (counthook fired once at the init site, then AD2_Crash dump at
       23:20:54). Control run: IAT hooks only, no .text patching. */
    /* InstallEntryHooks(); */

    /* basic .text patch test (1-byte immediate, no trampoline): mines 9->12 */
    PatchMinesCount();

    /* hotkey poll: Ctrl+Alt+U toggles UI */
    bool prev = false;
    int tick = 0;
    for (;;) {
        Sleep(80);
        if ((++tick % 62) == 0) VerifyMinesCount();
        /* if ((++tick % 62) == 0) VerifyEntryPatches(); */
        bool d = KeyDown(VK_CONTROL) && KeyDown(VK_MENU) && KeyDown('U');
        if (d && !prev) {
            s_show_ui = !s_show_ui;
            if (s_show_ui) {
                if (s_sdl_show_cursor) s_sdl_show_cursor(1);
                /* put the cursor somewhere visible: screen center */
                POINT c; GetCursorPos(&c);
                SetCursorPos(GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2);
            } else {
                if (s_sdl_hide_cursor) s_sdl_hide_cursor(0);
            }
            Log("hotkey: UI %s", s_show_ui ? "shown (cursor on, centered)" : "hidden (cursor off)");
        }
        prev = d;
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        HANDLE th = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (th) CloseHandle(th);
    }
    return TRUE;
}
