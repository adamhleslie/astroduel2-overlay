/* ad2ui.cpp -- in-game ImGui overlay for Astro Duel 2 (WREngine / cocos2d-x 3.17.X, SDL3)
 *
 * Purpose (versus level-pool editor, see re/LEVEL_POOL.md):
 *   An inline hook on the per-match versus level-load funnel
 *   FUN_140030ee0 (entry 0x140030ee0 + 24-byte trampoline). It reads the
 *   game's per-world level pools (singleton 0x1405df4f0 + 0x4a0/0x4a8) and,
 *   when a custom pool (level file exists in res/) is committed for that
 *   world from the overlay, remaps the chosen index into it and re-runs the
 *   original code; otherwise it passes through untouched. No game memory is
 *   written outside the chosen index field (+0x380). A watchdog re-verifies
 *   the 12-byte entry patch and logs corruption (e.g. VMP rewrites).
 *
 *   A second log-only inline hook on the engine's fopen wrapper
 *   FUN_140445388 (0x140445388, 24-byte trampoline, rdx/r8 saved across
 *   the call) logs every file the game opens -- the .tmx / *_data.xml
 *   loads go through CRT fopen, which bypasses the kernel32 IAT hooks, so
 *   this is the only ground truth for the real level-load path. Plus the
 *   kernel32/shell32 file-API IAT hooks (dir scans, mkdirs, interesting
 *   opens). All diagnostics -> %LOCALAPPDATA%\AstroDuel2Mod\probe_ui.log.
 *
 * Injection / render strategy (unchanged from milestone 1):
 *   - Injected into the Steam-launched game at main menu (inject.exe).
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
static char    s_cwd[1024] = {0};                    /* game CWD (path check base) */

/* hook-side diagnostics */
struct PoolStats { int world; int gate; int stockCount; int customCount;
                   uintptr_t stockStart; uintptr_t stockEnd; int hits; int clamped; };
static PoolStats s_stats[POOL_WORLD_MAX];
static volatile LONG s_pool_hook_on = 0;

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

/* ---- inline hook on FUN_140030ee0 (versus per-match level load) ----

   Layout (no inline asm -- MSVC x64 has none; we emit the trampoline bytes):
     entry  : 48 B8 <imm64 tp> FF E0       ; absolute jmp trampoline
     tp +0x00: <24 B stolen prologue: mov rax,rsp / 3 movs / 5 pushes>
     tp +0x18: E8 <rel32>  call PoolHook   ; rcx = mgr (ABI param 1)
     tp +0x1d: 48 89 c8    mov rcx, rax    ; PoolHook returns mgr in rax
     tp +0x22: 48 B8 <imm64 entry+24> FF E0 ;
   Alignment: at tp entry rsp = pre-prologue rsp (orig+8 mod16); the 5
   prologue pushes leave rsp = 0 mod 16, so `call` gives PoolHook the
   required 8 mod 16. After ret rsp is back to 0 mod 16 = exactly where
   the original prologue left it, and rax still holds the pre-prologue
   rsp, so the movaps block at entry+0x26 keeps its alignment. The
   original saves rcx at entry+0x76 (mov r15,rcx / [rbp-48],rcx) -- hence
   the restore; volatile regs (rdx, r8-r11, xmm0-5) are not depended on
   at entry. */

static BYTE* s_tp_buf = nullptr;
static char  s_tp_note[128] = {0};
static uintptr_t PoolHook(long long* mgr);  /* fwd (defined after InstallPoolHook) */

static bool InstallPoolHook()
{
    if (!s_module_base) return false;
    const uintptr_t kEntry = 0x30ee0, kCont = 0x30ef8;   /* RVAs (image base 0x140000000) */
    const BYTE want[24] = { 0x48,0x8b,0xc4,0x48,0x89,0x58,0x10, 0x48,0x89,0x70,0x18,
                            0x48,0x89,0x78,0x20, 0x55, 0x41,0x54, 0x41,0x55,
                            0x41,0x56, 0x41,0x57 };
    uint8_t* entry = (uint8_t*)(s_module_base + kEntry);
    if (memcmp(entry, want, 24) != 0) {
        snprintf(s_tp_note, sizeof s_tp_note, "prologue mismatch (bytes %02x %02x %02x..)",
                 entry[0], entry[1], entry[2]);
        Log("poolhook: %s -- REFUSED", s_tp_note); return false;
    }
    BYTE* tp = (BYTE*)VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!tp) { snprintf(s_tp_note, sizeof s_tp_note, "VirtualAlloc failed"); return false; }

    memcpy(tp, entry, 24);                       /* +0x00 prologue copy  */
    /* E8 rel32 is relative to the NEXT instruction: tp+24+5 = tp+29 */
    int32_t rel = (int32_t)((intptr_t)PoolHook - (intptr_t)(tp + 29));
    tp[24] = 0xE8; memcpy(tp + 25, &rel, 4);     /* +0x18 call PoolHook  */
    tp[29] = 0x48; tp[30] = 0x89; tp[31] = 0xC8; /* +0x1d mov rcx, rax   */
    uintptr_t cont = s_module_base + kCont;
    tp[32] = 0x48; tp[33] = 0xB8; memcpy(tp + 34, &cont, 8);
    tp[42] = 0xFF; tp[43] = 0xE0;                /* +0x22 jmp entry+24   */
    s_tp_buf = tp;

    DWORD oldProt = 0;
    uint8_t* page = (uint8_t*)((uintptr_t)entry & ~0xFFFULL);
    if (!VirtualProtect(page, 4096, PAGE_READWRITE, &oldProt)) {
        snprintf(s_tp_note, sizeof s_tp_note, "VirtualProtect failed: %lu", GetLastError());
        return false;
    }
    entry[0] = 0x48; entry[1] = 0xB8;
    memcpy(entry + 2, &tp, 8);
    entry[10] = 0xFF; entry[11] = 0xE0;
    DWORD dummy = 0;
    VirtualProtect(page, 4096, oldProt, &dummy);
    InterlockedExchange(&s_pool_hook_on, 1);
    s_tp_note[0] = 0;
    Log("poolhook: INSTALLED entry=%p tp=%p (call PoolHook, jmp back -> %p)",
        (void*)entry, (void*)tp, (void*)cont);
    return true;
}

/* runs on the game thread via the emitted `call` (normal ABI, rcx=mgr).
   Returns mgr in rax -- the trampoline does `mov rcx, rax`, so the
   original's `mov r15, rcx` at entry+0x76 still sees the real pointer.
   Reads/writes manager fields + globals only -- no locks (UI commits and
   this run on the same render thread in practice). */
static uintptr_t PoolHook(long long* mgr)
{
    uintptr_t ret = (uintptr_t)mgr;
    /* never touch a wild pointer -- a crash here would take the game down */
    if (!mgr || (uintptr_t)mgr < 0x10000 || ((uintptr_t)mgr & 0x7)) return ret;
    uintptr_t b = s_module_base;
    uintptr_t m = (uintptr_t)mgr;
    int world = (int)*(volatile int32_t*)(b + 0x5DE484);   /* DAT_1405de484 */
    int idx   = (int)*(volatile int32_t*)(m + 0x380);      /* caller's pick  */
    uintptr_t stockS = *(uintptr_t*)(m + 0x4A0);           /* read BEFORE repoint */
    uintptr_t stockE = *(uintptr_t*)(m + 0x4A8);
    int stockN = (int)((stockE - stockS) / 4);
    PoolCfg*  p   = (world >= 1 && world < POOL_WORLD_MAX) ? &s_pools[world] : nullptr;
    if (p && p->count > 0) {
        uintptr_t items = (uintptr_t)p->items;
        *(int32_t**)(m + 0x4A0) = (int32_t*)items;
        *(int32_t**)(m + 0x4A8) = (int32_t*)(items + (size_t)p->count * 4);
        int clamped = 0;
        if (idx < 0 || idx >= p->count) {
            idx = (int)(rand() % p->count);
            clamped = 1;
        }
        *(int32_t*)(m + 0x380) = (int32_t)idx;
        PoolStats& s = s_stats[world];
        s.world = world;
        s.gate  = (int)*(volatile uint8_t*)(b + 0x5DE4CF);
        s.stockStart = stockS; s.stockEnd = stockE; s.stockCount = stockN;
        s.customCount = p->count;
        s.hits++;
        s.clamped += clamped;
        Log("poolhook: world=%d(%s) gate=%d stock=%d custom=%d idx=%d -> level %d%s",
            world, PoolWorldName(world), s.gate, stockN, p->count, idx, p->items[idx],
            clamped ? "  [CLAMPED]" : "");
    } else if (s_stats[world].hits == 0 && world >= 1 && world < POOL_WORLD_MAX) {
        s_stats[world].world = world;
        s_stats[world].gate  = (int)*(volatile uint8_t*)(b + 0x5DE4CF);
        s_stats[world].stockStart = stockS; s_stats[world].stockEnd = stockE;
        s_stats[world].stockCount = stockN;
        s_stats[world].hits = -1;   /* "saw stock, no custom pool yet" */
        Log("poolhook: stock-only hit world=%d(%s) gate=%d stockCount=%d pool=%p..%p",
            world, PoolWorldName(world), s_stats[world].gate, stockN,
            (void*)stockS, (void*)stockE);
    }
    return ret;
}

static void InstallOpenLogHook();   /* fwd (defined below) */

static void PoolInstallWorker()
{
    if (GetCurrentDirectoryA((DWORD)sizeof s_cwd, s_cwd))
        Log("poolhook: game CWD = %s", s_cwd);
    if (!InstallPoolHook())
        Log("poolhook: not installed (%s)", s_tp_note[0] ? s_tp_note : "unknown");
    InstallOpenLogHook();
}

/* ---- inline log hook on FUN_140445388 (the engine's fopen wrapper) ----

   Ground truth for WHICH files the game actually opens: the versus .tmx
   and *_data.xml loads go through this wrapper (CRT fopen), which bypasses
   every kernel32 IAT hook, so the file-API IAT log alone is blind to them.

   Exact entry bytes (capstone-verified against the unpacked PE):
     0x140445388  40 53          push rbx
     0x14044538a  48 83 EC 20    sub  rsp, 0x20
     0x14044538e  41 8B C0       mov  eax, r8d        ; flags
     0x140445391  33 C9          xor  ecx, ecx
     0x140445393  24 03          and  al, 3           ; al = flags & 3
     0x140445395  48 8B DA       mov  rbx, rdx        ; path (callee-saved)
     0x140445398  3C 01          cmp  al, 1           ; <-- resume here
   Steal the first 16 B (through mov rbx,rdx); resume at 0x140445398.
   The code after resume reads al (flags&3), r8b (mode), and the path
   (rdx and/or rbx) -- and OpenHook's Log() clobbers all three (volatile
   x64 ABI), so rdx/r8/rax are saved in the prologue's own 0x20 scratch
   ([rsp+8],[rsp+0x10],[rsp+0x18]) around the call and restored before
   the absolute jmp back. rbx is ABI-callee-saved so Log keeps it.
   Trampoline: <16 B copy> save x3 / call OpenHook / restore x3 /
   jmp 0x140445398. Stack at the call: rsp = 0 mod16 (push + sub 0x20),
   so the callee gets the required 8 mod16. OpenHook is log-only; its
   rax return is discarded by the restore. */
static volatile LONG s_open_hook_on = 0;
static BYTE* s_otp_buf = nullptr;
static void OpenHook(const char* path, int flags);

static void InstallOpenLogHook()
{
    if (!s_module_base) return;
    const uintptr_t kEntry = 0x445388, kCont = 0x445398;
    const BYTE want[16] = { 0x40,0x53, 0x48,0x83,0xEC,0x20, 0x41,0x8B,0xC0,
                            0x33,0xC9, 0x24,0x03, 0x48,0x8B,0xDA };
    uint8_t* entry = (uint8_t*)(s_module_base + kEntry);
    if (memcmp(entry, want, 16) != 0) {
        Log("openlog: prologue mismatch (%02x %02x %02x .. %02x %02x %02x) -- REFUSED",
            entry[0], entry[1], entry[2], entry[13], entry[14], entry[15]);
        return;
    }
    BYTE* tp = (BYTE*)VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!tp) { Log("openlog: VirtualAlloc failed"); return; }

    memcpy(tp, entry, 16);                          /* +0x00 16 B prologue copy */
    int32_t rel = (int32_t)((intptr_t)OpenHook - (intptr_t)(tp + 0x1f + 5));
    tp[0x10] = 0x48; tp[0x11] = 0x89; tp[0x12] = 0x54; tp[0x13] = 0x08; /* mov [rsp+8], rdx  */
    tp[0x15] = 0x41; tp[0x16] = 0x89; tp[0x17] = 0x44; tp[0x18] = 0x10; /* mov [rsp+0x10], r8 */
    tp[0x1a] = 0x48; tp[0x1b] = 0x89; tp[0x1c] = 0x44; tp[0x1d] = 0x18; /* mov [rsp+0x18], rax */
    tp[0x1f] = 0xE8; memcpy(tp + 0x20, &rel, 4);    /* call OpenHook         */
    tp[0x24] = 0x48; tp[0x25] = 0x8B; tp[0x26] = 0x54; tp[0x27] = 0x08; /* mov rdx, [rsp+8]   */
    tp[0x29] = 0x41; tp[0x2a] = 0x8B; tp[0x2b] = 0x44; tp[0x2c] = 0x10; /* mov r8,  [rsp+0x10] */
    tp[0x2e] = 0x48; tp[0x2f] = 0x8B; tp[0x30] = 0x44; tp[0x31] = 0x18; /* mov rax, [rsp+0x18] */
    uintptr_t cont = s_module_base + kCont;
    tp[0x33] = 0x48; tp[0x34] = 0xB8; memcpy(tp + 0x35, &cont, 8);
    tp[0x3d] = 0xFF; tp[0x3e] = 0xE0;               /* jmp 0x140445398       */
    s_otp_buf = tp;

    DWORD oldProt = 0;
    uint8_t* page = (uint8_t*)((uintptr_t)entry & ~0xFFFULL);
    if (!VirtualProtect(page, 4096, PAGE_READWRITE, &oldProt)) {
        Log("openlog: VirtualProtect failed: %lu", GetLastError());
        return;
    }
    entry[0] = 0x48; entry[1] = 0xB8;
    memcpy(entry + 2, &tp, 8);
    entry[10] = 0xFF; entry[11] = 0xE0;
    DWORD dummy = 0;
    VirtualProtect(page, 4096, oldProt, &dummy);
    InterlockedExchange(&s_open_hook_on, 1);
    Log("openlog: INSTALLED entry=%p tp=%p (call OpenHook, jmp back -> %p)",
        (void*)entry, (void*)tp, (void*)cont);
}

/* runs on the game thread via the emitted `call` (rcx=this, rdx=path,
   r8d=flags at call time). Log-only: the trampoline restores rdx/r8/rax
   before jumping back to 0x140445398. */
static void OpenHook(const char* path, int flags)
{
    if (!path) return;
    Log("open: [f%d] %s", flags & 0xF, path);
}

/* ------------------------------------------------------------------ */
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
    Log("fileapi: summary fff=%p cdw=%p cfw=%p cf2=%p gfa=%p shgkf=%p",
        (void*)o1, (void*)o2, (void*)o3, (void*)o4, (void*)o5, (void*)o6);
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
    PoolStats& ps = s_stats[sel_world];
    char pl_sum[160];
    if (s_pools[sel_world].count > 0) {
        snprintf(pl_sum, sizeof pl_sum, "LIVE  %d/%d  (stock=%d, hits=%d, clamped=%d)",
                 s_pools[sel_world].count, sel_world == 1 ? 9 : 8,
                 ps.stockCount, ps.hits, ps.clamped);
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), pl_sum);
    } else if (ps.hits < 0) {
        snprintf(pl_sum, sizeof pl_sum, "stock pool seen (count=%d) -- commit a set to override", ps.stockCount);
        ImGui::TextDisabled(pl_sum);
    } else {
        ImGui::TextDisabled("no pool committed -- stock behaviour; no versus loads observed yet");
    }
    if (!s_tp_note[0] && s_pool_hook_on) {
        ImGui::TextDisabled("hook: installed (FUN_140030ee0)     ");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "hook: %s", s_tp_note[0] ? s_tp_note : "not installed");
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
/* watchdog: re-verify the 12-byte entry patch every ~600ms so a lazy
   VMP re-write (or anything else) shows up in the log immediately */
static void PoolHookVerifyTick()
{
    if (s_pool_hook_on && s_module_base) {
        uint8_t* entry = (uint8_t*)(s_module_base + 0x30ee0);
        if (entry[0] != 0x48 || entry[1] != 0xB8 ||
            memcmp(entry + 2, &s_tp_buf, 8) != 0 || entry[10] != 0xFF || entry[11] != 0xE0) {
            Log("poolhook: !!! ENTRY BYTES CORRUPTED (%02x %02x .. %02x %02x) -- hook dead",
                entry[0], entry[1], entry[10], entry[11]);
            InterlockedExchange(&s_pool_hook_on, 0);
        }
    }
    if (s_open_hook_on && s_module_base) {
        uint8_t* entry = (uint8_t*)(s_module_base + 0x445388);
        if (entry[0] != 0x48 || entry[1] != 0xB8 ||
            memcmp(entry + 2, &s_otp_buf, 8) != 0 || entry[10] != 0xFF || entry[11] != 0xE0) {
            Log("openlog: !!! ENTRY BYTES CORRUPTED (%02x %02x .. %02x %02x) -- hook dead",
                entry[0], entry[1], entry[10], entry[11]);
            InterlockedExchange(&s_open_hook_on, 0);
        }
    }
}

static DWORD WINAPI Worker(LPVOID)
{
    Sleep(1500); /* let the game settle past injection */
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

    HMODULE s3 = GetModuleHandleA("SDL3.dll");
    Log("worker: module_base=%p SDL3.dll=%p", (void*)s_module_base, (void*)s3);
    if (!s3) { Log("worker: SDL3.dll NOT loaded?? giving up"); return 1; }

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

    /* ground truth: log every dir scan / mkdir the game performs */

    /* versus level-pool inline hook (re/LEVEL_POOL.md) */
    PoolInstallWorker();
    InstallFileApiHooks();

    /* hotkey poll: Ctrl+Alt+U toggles UI; pool-hook re-verify every ~600ms */
    bool prev = false;
    int wd_ms = 0;
    for (;;) {
        Sleep(80);
        if ((wd_ms += 80) >= 600) { wd_ms = 0; PoolHookVerifyTick(); }
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
