/* ad2ui.cpp -- in-game ImGui overlay for Astro Duel 2 (WREngine / cocos2d-x 3.17.X, SDL3)
 *
 * Built as both ad2ui.dll and the steam_api64.dll proxy (see CMakeLists);
 * in either case DllMain spawns one worker thread that:
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
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include <string>
#include <algorithm>   /* std::sort -- natural order of cached .tmx names */

/* single WGL entry we need, declared by hand to avoid <GL/gl.h> clashing
   with imgui_impl_opengl3_loader.h's IMGL3W prototypes */
extern "C" HDC WINAPI wglGetCurrentDC(void);

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_opengl3_loader.h"
#include <MinHook.h>
#include <intrin.h>    /* _ReturnAddress() -- hook caller forensics (x64 C++ intrinsic) */

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
static HINSTANCE s_dll_instance = nullptr; /* our module -- game dir base for pool.json */
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
/* world names (log labels)                                            */
/* ------------------------------------------------------------------ */

#define POOL_WORLD_MAX  16      /* world ids 1..15 */

static const char* PoolWorldName(int id)
{
    static const char* names[POOL_WORLD_MAX] = {
        "", "mines", "frontier", "flare", "darklab", "echocity", "highrise",
        "sunken", "graveyard", "gasrigs", "moonbase", "urspace", "void",
        "legacy", "up", "unsorted" };
    return (id >= 1 && id < POOL_WORLD_MAX) ? names[id] : "?";
}
static int PoolWorldId(const char* name)
{
    for (int i = 1; i < POOL_WORLD_MAX; i++)
        if (strcmp(PoolWorldName(i), name) == 0) return i;
    return 0;
}

/* ------------------------------------------------------------------ */
/* versus level cache -- per-world .tmx inventory + the count source  */
/*                                                                     */
/* Built once at startup: enumerate every subdirectory of             */
/* <gamedir>/res/levels/versus (the overlay DLL sits in the game dir  */
/* as steam_api64.dll, so its module path is the base) and, for each  */
/* world dir, every *.tmx in it. One cache object per world; each     */
/* file entry carries:                                                */
/*   name   -- the file's basename                                    */
/*   active -- in the live pool; default true EXCEPT the "cg_"       */
/*             prefix (the Cash Grab variants)                        */
/*   slot   -- the preload slot the file will be pre-loaded to;       */
/*             active files take slots 1..K in sort order, inactive   */
/*             files hold no slot (0)                                 */
/* Sort is natural: numbered files by ascending trailing number,      */
/* then non-numbered files alphabetically (cg_mines.tmx etc.).        */
/* The world's count is K = number of active files; the game          */
/* preloads slots 1..K contiguously, so this table is also the future*/
/* glv remap (slot -> file) is enforced in GetLevelRemap: a world      */
/* with no cache entry (or no active files) runs stock.               */
/* for active-flag persistence (a future UI step) -- not read here.   */
/* ------------------------------------------------------------------ */
struct PoolFile { std::string name; std::string path; bool active; int slot; };
struct PoolWorld { int id; std::string dir; std::vector<PoolFile> files; };
static std::vector<PoolWorld> s_pool_worlds;
static int s_pool_world_by_id[POOL_WORLD_MAX] = {-1};   /* index into s_pool_worlds, -1 = none */

/* trailing number of a tmx name ("mines12.tmx" -> 12); -1 when none */
static int PoolTmxNumber(const std::string& n)
{
    std::string b = n;
    size_t dot = b.rfind('.');
    if (dot != std::string::npos) b.erase(dot);
    size_t i = b.size();
    while (i > 0 && b[i-1] >= '0' && b[i-1] <= '9') i--;
    if (i == b.size()) return -1;
    int num = 0;
    for (size_t k = i; k < b.size(); k++) num = num * 10 + (b[k] - '0');
    return num;
}

/* natural order: numbered by ascending number, non-numbered last */
static bool PoolTmxBefore(const std::string& a, const std::string& b)
{
    int na = PoolTmxNumber(a), nb = PoolTmxNumber(b);
    if ((na < 0) != (nb < 0)) return na >= 0;
    if (na != nb) return na < nb;
    return a < b;
}

/* compact slots 1..K over the active files (already sort-ordered); returns K */
static int PoolWorldAssignSlots(PoolWorld& w)
{
    int slot = 0;
    for (PoolFile& pf : w.files)
        pf.slot = pf.active ? ++slot : 0;   /* inactive must lose any stale slot */
    return slot;
}

/* count of active files in a worldType's cache entry; 0 = no entry (stock) */
static int PoolActiveCount(int worldType)
{
    if (worldType < 1 || worldType >= POOL_WORLD_MAX) return 0;
    int idx = s_pool_world_by_id[worldType];
    if (idx < 0) return 0;
    int n = 0;
    for (const PoolFile& f : s_pool_worlds[idx].files)
        if (f.active) n++;
    return n;
}

/* scan one world dir (<base>\\res\\levels\\versus\\<dir>) into the cache */
static void PoolWorldScan(const char* dirA, const char* dirName)
{
    wchar_t pat[MAX_PATH + 16];
    if (!MultiByteToWideChar(CP_ACP, 0, dirA, -1, pat, MAX_PATH + 16)) return;
    wcscat(pat, L"\\*.tmx");
    std::vector<std::string> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                char an[260];
                if (WideCharToMultiByte(CP_ACP, 0, fd.cFileName, -1, an, 260, nullptr, nullptr))
                    names.push_back(an);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    } else {
        Log("poolcache: %s: scan FAILED (err %lu)", dirA, GetLastError());
        return;
    }
    std::sort(names.begin(), names.end(), PoolTmxBefore);

    PoolWorld w;
    w.id = PoolWorldId(dirName);                    /* 0 = not in the worldType enum */
    w.dir = dirName;
    int cg = 0;
    for (const std::string& nm : names) {
        PoolFile pf;
        pf.name = nm;
        pf.path = std::string(dirA) + "\\" + nm;
        pf.active = (nm.rfind("cg_", 0) != 0);
        if (!pf.active) cg++;
        pf.slot = 0;                                /* assigned below */
        w.files.push_back(pf);
    }
    int act = PoolWorldAssignSlots(w);
    int idx = (int)s_pool_worlds.size();
    s_pool_worlds.push_back(w);
    if (w.id >= 1 && w.id < POOL_WORLD_MAX) s_pool_world_by_id[w.id] = idx;
    Log("poolcache: %s: %d tmx, %d active (cg_ excluded: %d)%s -> count=%d",
        dirName, (int)w.files.size(), act, cg,
        (w.id < 1) ? " [worldType unknown, not applied]" : "", act);
}

static void PoolCacheBuild()
{
    char base[MAX_PATH] = {0};
    if (!GetModuleFileNameA(s_dll_instance, base, MAX_PATH)) { Log("poolcache: GetModuleFileName FAILED"); return; }
    char* slash = strrchr(base, '\\');
    if (!slash) { Log("poolcache: no dir in %s", base); return; }
    *slash = 0;
    char dir[MAX_PATH + 32];
    snprintf(dir, sizeof dir, "%s\\res\\levels\\versus", base);

    wchar_t pat[MAX_PATH + 32];
    if (!MultiByteToWideChar(CP_ACP, 0, dir, -1, pat, MAX_PATH + 32)) return;
    wcscat(pat, L"\\*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { Log("poolcache: %s not found (err %lu) -- all worlds stock", dir, GetLastError()); return; }
    int worlds = 0;
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char an[MAX_PATH];
            if (WideCharToMultiByte(CP_ACP, 0, fd.cFileName, -1, an, MAX_PATH, nullptr, nullptr)) {
                char wdir[MAX_PATH + 64];
                snprintf(wdir, sizeof wdir, "%s\\%s", dir, an);
                PoolWorldScan(wdir, an);
                worlds++;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    Log("poolcache: %d world dir(s) cached under %s", worlds, dir);
}

/* ------------------------------------------------------------------ */
/* pool editor draft + apply (UI)                                      */
/*                                                                     */
/* The UI edits a DRAFT copy of the active flags (one vector per world,*/
/* parallel to s_pool_worlds). Apply copies the selected world's draft*/
/* into the live cache and recompacts its slots. PoolActiveCount (and */
/* and the glv remap) read the LIVE cache, so the count only moves    */
/* on Apply. Threading: the draft + live cache are touched by the      */
/* render thread (UI) only after s_pool_ready; the game thread reads   */
/* the live cache via PoolActiveCount (minor race, single-player local*/
/* mod -- the UI is used at menus, not during level loads).            */
/* ------------------------------------------------------------------ */
static int    s_pool_sel_world = 0;                    /* index into s_pool_worlds */
static std::vector<std::vector<char>> s_pool_draft;    /* draft active flags (0/1) */
static volatile LONG s_pool_ready = 0;                 /* cache + draft built */

/* draft = current live flags; once, after PoolCacheBuild */
static void PoolDraftInit()
{
    s_pool_draft.resize(s_pool_worlds.size());
    for (size_t i = 0; i < s_pool_worlds.size(); i++) {
        s_pool_draft[i].resize(s_pool_worlds[i].files.size());
        for (size_t j = 0; j < s_pool_worlds[i].files.size(); j++)
            s_pool_draft[i][j] = s_pool_worlds[i].files[j].active ? 1 : 0;
    }
    InterlockedExchange(&s_pool_ready, 1);
}

/* copy the selected world's draft into the live cache + recompact slots */
static void PoolApplyWorld(int i)
{
    if (i < 0 || i >= (int)s_pool_worlds.size()) return;
    PoolWorld& w = s_pool_worlds[i];
    int changed = 0;
    for (size_t j = 0; j < w.files.size() && j < s_pool_draft[i].size(); j++) {
        bool a = s_pool_draft[i][j] != 0;
        if (a != w.files[j].active) { w.files[j].active = a; changed++; }
    }
    int count = PoolWorldAssignSlots(w);
    Log("poolui: APPLY %s -- count=%d (%d flag change(s))", w.dir.c_str(), count, changed);
}

/* file-API IAT hooks: runtime path log (match-start diagnostics)      */
/*                                                                     */
/* IAT-hooks the game's own file imports (kernel32/shell32, not VMP   */
/* code) and logs every dir scan, mkdir, and open on "interesting"    */
/* paths (levels/*.tmx, *_data.xml, res/...). Ground truth for WHICH   */
/* level files a match actually loads.                                  */
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

/* GetFileAttributesW: the dir-exists pre-checks probe through this --
   a NOT-FOUND result on a base dir shows up here. */
static DWORD WINAPI Ad2GetFileAttributesW(const wchar_t* path)
{
    DWORD r = s_gfa_orig(path);
    if (s_hook_active && PathInteresting(path))
        Log("GAME GetFileAttributesW: %ls  (ret=0x%lx%s)", path, r,
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
        Log("GAME fopen: %s  [%s]  (ret=%p%s)", path, mode ? mode : "?",
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
            Log("GAME SHGetKnownFolderPath %s -> %ls", g, *out);
        else
            Log("GAME SHGetKnownFolderPath %s FAILED hr=0x%08lX", g, (unsigned long)hr);
    }
    return hr;
}

/* IAT-slot hook core (same mechanism as the SDL_GL_SwapWindow hook:
   overwrite the game's import-table slot, no trampolines, no patching of
   system-DLL code). `expected` is the runtime-resolved address the slot
   should hold. */
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
/* MinHook hook on WorldDataLoader::worldDataForType (0x1402ae190).   */
/* Defer-and-patch: GetWorldDataHook calls the trampoline (== the      */
/* original function: stolen prologue re-executes and jmps into the   */
/* continuation), which fills out[0..3] = {d0,d1,d2,d3}; we own ONLY  */
/* d3 (the per-world level count, +0xc) and leave d0/d1/d2 untouched   */
/* -- they always come from the real code. The count override comes   */
/* from the versus level cache (active .tmx files per world); worlds  */
/* not in the cache run 100% stock.                                    */
/*                                                                      */
/* Entry: 5-byte E9 rel32 to a +2^20 relay (jmp GetWorldDataHook).     */
/* The 14-byte stolen prologue lands in the 64-byte slot's trampoline */
/* half. Failure mode: if VMP ever re-writes the entry, the hook      */
/* silently disengages (stock behavior, no crash) -- the verifier     */
/* logs it.                                                          */
/* ------------------------------------------------------------------ */
static void*        s_orig_getWorldData = nullptr; /* MinHook trampoline = original function */
static volatile LONG s_getWorldData_state = 0;     /* 0=off/failed, 1=installed+enabled */
static int s_getWorldData_revert_logged = 0;
/* getLevel state -- declared here; the hook is further below. */
static void*         s_orig_getLevel = nullptr;
static volatile LONG s_getLevel_state = 0;
static int s_getLevel_revert_logged = 0;

typedef void* (*FnGetWorldData)(void* out, int worldType);

/* RA forensics: MinHook patches the target entry with an absolute JMP to
   the detour, so the game's CALL has already pushed the real return
   address before we arrive -- _ReturnAddress() (frame-pointer based,
   valid with /Oy- from CMakeLists) == the game caller's rip, i.e. the
   instruction right after their call. Log it module-relative; it names
   the exact call site. (Depth-2 walk unavailable: MSVC's C
   _returnaddress(N) is x86-only; a MASM stub could add it if ever
   needed -- ra0 already names the call site.) */
static void* GetWorldDataHook(void* out, int worldType)
{
    if (!s_orig_getWorldData) {
        Log("getWorldData: FATAL -- no trampoline; out buffer left zeroed");
        return out;
    }
    void* r = ((FnGetWorldData)s_orig_getWorldData)(out, worldType);
    int* f = (int*)r;                       /* f[0..3] = d0,d1,d2,d3 */
    const int stock = f[3];
    const int ov = PoolActiveCount(worldType);
    if (ov) f[3] = ov;
    Log("getWorldData: world=%d(%s) out=%p cnt=%d (stock %d)%s ra0=%08X",
        worldType, PoolWorldName(worldType), r, f[3], stock,
        ov ? " OVERRIDDEN" : "",
        (uint32_t)((uintptr_t)_ReturnAddress() - (uintptr_t)s_module_base));
    return r;
}

static bool InstallGetWorldDataHook()
{
    if (MH_Initialize() != MH_OK) { Log("getWorldData: MH_Initialize FAILED"); return false; }
    void* target = (void*)(s_module_base + 0x2AE190);
    if (MH_CreateHook(target, (LPVOID)GetWorldDataHook, &s_orig_getWorldData) != MH_OK) {
        Log("getWorldData: MH_CreateHook FAILED -- REFUSED, stock behavior");
        return false;
    }
    /* EnableHookLL freezes all threads once while it writes the entry; do
       it after the IAT hooks (render path stable) and well before the
       ~t+9s preload block that first fires worldDataForType. */
    if (MH_EnableHook(target) != MH_OK) {
        Log("getWorldData: MH_EnableHook FAILED -- MH_RemoveHook, stock behavior");
        MH_RemoveHook(target);
        return false;
    }
    s_getWorldData_state = 1;
    Log("getWorldData: INSTALLED target=%p trampoline=%p", target, s_orig_getWorldData);
    return true;
}

/* ~5s worker-loop check (both MinHook entries): the entry must still be
   our E9 rel32. If VMP re-writes it the hook has silently disengaged
   (stock behavior). Log only, no re-arm. */
static void VerifyEntryHooks()
{
    if (InterlockedCompareExchange(&s_getWorldData_state, 0, 0) == 1) {
        uint8_t* p = (uint8_t*)(s_module_base + 0x2AE190);
        if (p[0] != 0xE9 && s_getWorldData_revert_logged++ < 3)
            Log("getWorldData: entry REVERTED (bytes %02x %02x %02x %02x) -- VMP? hook disengaged, stock behavior",
                p[0], p[1], p[2], p[3]);
    }
    if (InterlockedCompareExchange(&s_getLevel_state, 0, 0) == 1) {
        uint8_t* p = (uint8_t*)(s_module_base + 0x26C1C0);
        if (p[0] != 0xE9 && s_getLevel_revert_logged++ < 3)
            Log("getLevel: entry REVERTED (bytes %02x %02x %02x %02x) -- VMP? hook disengaged, stock behavior",
                p[0], p[1], p[2], p[3]);
    }
}

/* ------------------------------------------------------------------ */
/* MinHook: LOG-ONLY hook on the level-string composer getLevelString */
/* (0x14026C1C0: rcx=out std::string, rdx=world, r8=level num,        */
/* r9=mode). EVERY level-load path composes its path here -- the whole*/
/* binary has exactly 4 call sites:                                   */
/*   0x140013d9e/0x140013e30   preload, in FUN_140013c00              */
/*   0x140031456               versus pick path 0x140030ee0          */
/*   0x14026cda2               alt cluster 0x14026cce0 (world global  */
/*                             DAT_1405de550, num from obj+0x114),    */
/*                             called from 0x14026b9c0/0x14026bf20    */
/* ra0 tells which composer fired. Per call the hook also logs the    */
/* pick context the picker refreshes before the chain -- pick         */
/* (DAT_1405b9ff0: the versus game-mode enum), spec (the player-      */
/* specified level: DAT_1405de480 when DAT_1405de607 is set, else     */
/* singleton DAT_1405de9c8 + 0x45c) and cnt (DAT_1405de56c = the d3   */
/* level count). Zero behavioral change.                              */
/* ------------------------------------------------------------------ */
typedef void* (*FnGetLevel)(void* out, int world, int num, char mode);

/* Game's 32-byte std::string layout (proven from the copy-ctor FUN_140008700
   and dtor FUN_140003000, not guessed):
     bytes  0-7  word0: heap data ptr (heap mode) or SSO chars[0..7]
     bytes  8-15 word1: SSO chars[8..15] (stale/garbage in heap mode)
     bytes 16-23 word2: length
     bytes 24-31 word3: capacity -- >15 => heap, ==15 => SSO (dtor frees word0 iff word3>15)
   Read-only, clamped. */
static void PeekStdString(void* s, char* buf, size_t bufsize)
{
    if (!s) { snprintf(buf, bufsize, "(nil)"); return; }
    const uint64_t* w = (const uint64_t*)s;
    uint64_t len = w[2], cap = w[3];
    char* data = (cap > 15) ? (char*)w[0] : (char*)s;   /* SSO chars in bytes 0-15 */
    if ((!data && cap > 15) || len > 127) { snprintf(buf, bufsize, "(bad ptr=%p len=%llu cap=%llu)", (void*)w[0], (unsigned long long)len, (unsigned long long)cap); return; }
    size_t n = len < bufsize - 1 ? (size_t)len : bufsize - 1;
    memcpy(buf, data, n); buf[n] = 0;
}

/* glv remap (stage 2): the composer built levels/versus/<world>/<world><num>.tmx
   with num = pool slot (1..K, via the WdHook count). Rewrite the composed
   string to the slot's actual .tmx per the live cache.
   Guards, in order (each = silent no-op unless noted):
     pick == 3            cash grab -- composer ignores num (cg_<world>.tmx);
                          the 4th param is 0 in BOTH branches (proven in the
                          caller FUN_140013c00: it sets DAT_1405b9ff0 but passes
                          0); pick is the real discriminator
     world out of range   void/default/unknown are not pooled
     no slot == num       outside the active pool (count grew) -> ERROR log
     name == <w><num>.tmx identity -- the file IS its slot; no rewrite
   Rewrites via assign() on the game's 32-byte std::string (layout proven from
   its own copy-ctor FUN_140008700 / dtor FUN_140003000; shared UCRT heap,
   so assign() may realloc safely). */
static void GetLevelRemap(void* out, int world, int num, int pick)
{
    if (!s_pool_ready || !out) return;
    if (pick == 3) return;                            /* cash grab */
    if (world < 1 || world >= POOL_WORLD_MAX) return; /* not pooled */
    int idx = s_pool_world_by_id[world];
    if (idx < 0) return;
    const PoolWorld& w = s_pool_worlds[idx];
    const PoolFile* pf = nullptr;
    for (size_t j = 0; j < w.files.size(); j++)
        if (w.files[j].slot == num) { pf = &w.files[j]; break; }
    if (!pf) {
        Log("getLevel REMAP MISS: world=%d(%s) num=%d -- no slot (outside active pool)",
            world, PoolWorldName(world), num);
        return;
    }
    char ident[48];
    snprintf(ident, sizeof ident, "%s%d.tmx", PoolWorldName(world), num);
    if (strcmp(pf->name.c_str(), ident) == 0) return; /* identity: file IS slot num */
    char np[192];
    snprintf(np, sizeof np, "levels/versus/%s/%s", w.dir.c_str(), pf->name.c_str());
    reinterpret_cast<std::string*>(out)->assign(np, strlen(np));
    Log("getLevel REMAP: world=%d(%s) slot=%d %s -> %s",
        world, PoolWorldName(world), num, ident, pf->name.c_str());
}

static void* GetLevelHook(void* out, int world, int num, char mode)
{
    void* ret = s_orig_getLevel ? ((FnGetLevel)s_orig_getLevel)(out, world, num, mode) : nullptr;
    if (!s_orig_getLevel) { Log("getLevel: FATAL -- no trampoline; composer NOT run"); return ret; }
    /* probe: pick context FUN_14026B9C0 refreshes before the chain. */
    uintptr_t b = s_module_base;
    int pick = (int)*(volatile int32_t*)(b + 0x5B9FF0);      /* DAT_1405b9ff0 */
    int cnt  = (int)*(volatile int32_t*)(b + 0x5DE56C);      /* DAT_1405de56c (d3) */
    int spec = -1;
    if (*(volatile int8_t*)(b + 0x5DE607))                   /* DAT_1405de607 */
        spec = (int)*(volatile int32_t*)(b + 0x5DE480);      /* DAT_1405de480 */
    else {
        uintptr_t s = *(volatile uintptr_t*)(b + 0x5DE9C8);  /* singleton */
        if (s >= 0x10000) spec = (int)*(volatile int32_t*)(s + 0x45C);
    }
    GetLevelRemap(out, world, num, pick);   /* slot -> file (stage 2); logs on rewrite/miss */
    char path[160];
    PeekStdString(out, path, sizeof(path));   /* post-rewrite: the final path */
    Log("getLevel: world=%d(%s) num=%d mode=%d pick=%d spec=%d cnt=%d path=\"%s\" ret=%p ra0=%08X",
        world, PoolWorldName(world), num, (int)mode, pick, spec, cnt, path, ret,
        (uint32_t)((uintptr_t)_ReturnAddress() - (uintptr_t)s_module_base));
    return ret;
}

static bool InstallGetLevelHook()
{
    void* target = (void*)(s_module_base + 0x26C1C0);
    if (MH_CreateHook(target, (LPVOID)GetLevelHook, &s_orig_getLevel) != MH_OK) {
        Log("getLevel: MH_CreateHook FAILED -- REFUSED, stock behavior");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        Log("getLevel: MH_EnableHook FAILED -- MH_RemoveHook, stock behavior");
        MH_RemoveHook(target);
        return false;
    }
    s_getLevel_state = 1;
    Log("getLevel: INSTALLED target=%p trampoline=%p", target, s_orig_getLevel);
    return true;
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
    ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("ad2mod overlay", nullptr, ImGuiWindowFlags_NoCollapse);
    ImGui::Text("ad2mod overlay -- hooks active   (Ctrl+Alt+U toggles)");
    ImGui::Separator();

    /* ---------------- versus level-pool editor ---------------- */
    ImGui::Text("VERSUS LEVEL POOL   (local play only)");
    if (!s_pool_ready || s_pool_worlds.empty()) {
        ImGui::TextDisabled("no worlds cached (res/levels/versus missing?)");
    } else {
        if (s_pool_sel_world < 0 || s_pool_sel_world >= (int)s_pool_worlds.size())
            s_pool_sel_world = 0;
        const PoolWorld& sel = s_pool_worlds[s_pool_sel_world];
        if (ImGui::BeginCombo("##world", sel.dir.c_str())) {
            for (int i = 0; i < (int)s_pool_worlds.size(); i++) {
                bool s = ImGui::Selectable(s_pool_worlds[i].dir.c_str(), s_pool_sel_world == i);
                if (s) s_pool_sel_world = i;
            }
            ImGui::EndCombo();
        }
        ImGui::Spacing();
        ImGui::BeginChild("##files", ImVec2(0, 300), true);
        std::vector<char>& draft = s_pool_draft[s_pool_sel_world];
        for (size_t j = 0; j < sel.files.size() && j < draft.size(); j++) {
            bool b = draft[j] != 0;
            if (ImGui::Checkbox(sel.files[j].name.c_str(), &b))
                draft[j] = b ? 1 : 0;
            ImGui::SameLine();
            ImGui::TextDisabled("%s", sel.files[j].path.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled(sel.files[j].active ? "[slot %d]" : "[off]", sel.files[j].slot);
        }
        ImGui::EndChild();
        int act = 0; for (char c : draft) if (c) act++;
        ImGui::Text("draft count: %d    live count: %d", act, PoolActiveCount(sel.id));
        if (ImGui::Button(("Apply " + sel.dir).c_str()))
            PoolApplyWorld(s_pool_sel_world);
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

    /* versus level cache: the per-world .tmx inventory that drives the
       count override (active files per world). Plain file read -- needs
       no unpacking; built before the hooks install so the summary is in
       the log before the first getWorldData call. */
    PoolCacheBuild();
    PoolDraftInit();
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

    /* wait until VMP has unpacked the image in place before trusting ANY
       runtime bytes (IAT slot scan, patches). */
    if (!WaitUnpacked()) {
        Log("worker: ABORTING -- game not unpacked, refusing to patch packed bytes");
        return 1;
    }

    /* SDL3.dll loads early in the game's own startup; it may not exist
       yet at import resolution, so poll. */
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

    /* hook on worldDataForType (defer-and-patch count). Installed after
       the IAT hooks (render path stable) and before the ~t+9s preload
       block that first fires it. */
    InstallGetWorldDataHook();

    /* level-string composer hook: identifies which call site fired
       (ra0), applies the slot -> file remap (stage 2), logs the final
       path and the pick context (pick/spec/cnt). Installed after
       PoolCacheBuild/PoolDraftInit so the cache is ready on first fire. */
    InstallGetLevelHook();

    /* hotkey poll: Ctrl+Alt+U toggles UI */
    bool prev = false;
    int tick = 0;
    for (;;) {
        Sleep(80);
        if ((++tick % 62) == 0) VerifyEntryHooks();    /* ~5s: entries still ours? */
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
        s_dll_instance = h;
        DisableThreadLibraryCalls(h);
        HANDLE th = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (th) CloseHandle(th);
    }
    return TRUE;
}
