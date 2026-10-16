/* ad2ui.cpp -- in-game ImGui overlay for Astro Duel 2 (WREngine / cocos2d-x 3.17.X, SDL3)
 *
 * Strategy (milestone 1: UI only, no game calls yet):
 *   - Injected into the Steam-launched game at main menu (inject.exe).
 *   - Worker thread (DllMain-spawned) waits for the game to settle, then
 *     IAT-hooks SDL_GL_SwapWindow: the game's IAT import directory is
 *     packer-forged, so we do NOT parse the PE. Instead:
 *         real_addr = GetProcAddress(GetModuleHandleA("SDL3.dll"), "SDL_GL_SwapWindow")
 *         slot      = scan known IAT region (RVA 0x47e028..0x47eba0) for == real_addr
 *     and patch that one slot to point at our hook.
 *   - The hook runs ON THE GAME'S RENDER THREAD, which is exactly what we
 *     want for the later direct-call (Approach C) wiring: no cross-thread
 *     game-state races when UI buttons start calling game functions.
 *   - Lazy-inits ImGui (desktop GL backend, built-in gl3w-stripped loader,
 *     GLSL "#version 120" for the game's 2.1-compat context) on first swap.
 *   - Input: manual Win32 (GetCursorPos/ScreenToClient + GetAsyncKeyState),
 *     no WndProc subclassing in v1.
 *   - UI toggle: Ctrl+Alt+U (polled in worker thread, edge-triggered).
 *   - All diagnostics -> %LOCALAPPDATA%\AstroDuel2Mod\probe.log
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include <mutex>

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

static void Log(const char* fmt, ...)
{
    if (!s_log) {
        wchar_t dir[1024];
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, 1024)) return;
        char dirA[1024];
        WideCharToMultiByte(CP_UTF8, 0, dir, -1, dirA, 1024, nullptr, nullptr);
        char path[1200];
        char sub[1200];
        snprintf(sub, sizeof(sub), "%s\\AstroDuel2Mod", dirA);
        snprintf(path, sizeof(path), "%s\\probe_ui.log", sub);
        CreateDirectoryA(sub, nullptr);
        s_log = fopen(path, "a");
        if (!s_log) return;
        setvbuf(s_log, nullptr, _IOFBF, 4096);
    }
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(s_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(s_log, fmt, ap);
    va_end(ap);
    fputc('\n', s_log);
    fflush(s_log);
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
/* M2: game console wiring                                             */
/*                                                                     */
/* Output path (upstream v3 CCConsole, confirmed in decompilation):    */
/*   every command emits via mydprintf(fd,...) -> sendToConsole ->     */
/*   send/sendto. There is NO local output buffer -- the socket is the */
/*   only exit. So we IAT-hook sendto (RVA 0x47e608) and capture the   */
/*   bytes before they die into an invalid socket. We also hook        */
/*   recvfrom (0x47e610) for logging (and later: memory-fed args).     */
/* Command entry (decompiled chain): FUN_140375f30(console, char*line) */
/*   strips '#' comments, tail-calls the processor at console+0x120    */
/*   (bare stored fn ptr, must be non-NULL). Manager singleton:        */
/*   FUN_1403de148() no-args lazy getter; console at manager+0x1f0.    */
/* ------------------------------------------------------------------ */
typedef uintptr_t (*FnManagerGet)();
typedef int       (*FnConsoleLine)(void* console, const char* line);
static FnManagerGet  g_mgr_get  = nullptr;   /* base + 0x3de148 */
static FnConsoleLine g_line_in  = nullptr;   /* base + 0x375f30 */
static void* g_manager = nullptr;
static void* g_console = nullptr;
static void* g_proc120 = nullptr;   /* console+0x120, must be non-NULL */
static bool  g_probe_done = false;

/* captured console output ring */
static std::vector<char> g_out;
static std::mutex g_out_mu;
static void Capture(const char* buf, int len)
{
    if (len <= 0) return;
    std::lock_guard<std::mutex> lk(g_out_mu);
    if (g_out.size() + (size_t)len > 262144) g_out.clear();
    g_out.insert(g_out.end(), buf, buf + len);
}

/* winsock hook pair (signatures per winsock2.h) */
typedef int (WINAPI *FnSendto)(unsigned int s, const char* buf, int len, int flags, const void* to, int tolen);
typedef int (WINAPI *FnRecvfrom)(unsigned int s, char* buf, int len, int flags, void* from, int* fromlen);
static FnSendto   g_orig_sendto   = nullptr;
static FnRecvfrom g_orig_recvfrom = nullptr;
static int s_sendto_calls = 0;

static int WINAPI Ad2HookedSendto(unsigned int s, const char* buf, int len, int flags, const void* to, int tolen)
{
    Capture(buf, len);
    s_sendto_calls++;
    if (s_sendto_calls <= 8) Log("sendto#%d: socket=%u len=%d flags=%d", s_sendto_calls, s, len, flags);
    return g_orig_sendto(s, buf, len, flags, to, tolen);
}
static int WINAPI Ad2HookedRecvfrom(unsigned int s, char* buf, int len, int flags, void* from, int* fromlen)
{
    int r = g_orig_recvfrom(s, buf, len, flags, from, fromlen);
    Log("recvfrom: socket=%u len=%d -> ret=%d", s, len, r);
    return r;
}

/* read manager -> console -> processor. Safe: only pointer reads plus a
   lazy-init getter (no side effects if already initialized). */
static bool ProbeConsole()
{
    if (g_probe_done) return g_console != nullptr && g_proc120 != nullptr;
    g_probe_done = true;
    g_manager = (void*)g_mgr_get();
    if (!g_manager) { Log("probe: manager=NULL"); return false; }
    g_console = *(void**)((BYTE*)g_manager + 0x1f0);
    if (!g_console) { Log("probe: manager=%p console=NULL (not created yet)", (void*)g_manager); return false; }
    g_proc120 = *(void**)((BYTE*)g_console + 0x120);
    Log("probe: manager=%p console=%p proc120=%p", (void*)g_manager, (void*)g_console, (void*)g_proc120);
    return g_proc120 != nullptr;
}

static void RunLine(const char* line)
{
    if (!ProbeConsole()) { Log("run: refused, console not available"); return; }
    Log("RUN line: '%s'", line);
    int r = g_line_in(g_console, line);
    Log("RUN line ret=%d", r);
}

static void PatchIatSlot(uintptr_t rva, const char* what, uintptr_t hook)
{
    uintptr_t* slot = (uintptr_t*)(s_module_base + rva);
    Log("worker: %s slot %p content=%p", what, (void*)slot, (void*)*slot);
    if (!*slot) { Log("worker: %s slot EMPTY, skipping", what); return; }
    uintptr_t page = (uintptr_t)slot & ~(uintptr_t)0xFFF;
    DWORD oldProt = 0;
    if (!VirtualProtect((LPVOID)page, 0x1000, PAGE_READWRITE, &oldProt)) {
        Log("worker: VirtualProtect(%p) FAILED: %lu", (void*)page, GetLastError()); return;
    }
    *slot = hook;
    DWORD dummy = 0;
    VirtualProtect((LPVOID)page, 0x1000, oldProt, &dummy);
    Log("worker: %s hooked (old prot 0x%lx restored)", what, oldProt);
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
    ImGui::Begin("ad2mod (imgui overlay)", nullptr, ImGuiWindowFlags_NoCollapse);
    ImGui::Text("frame: %d", s_frame);
    ImGui::Separator();
    ImGui::Text("manager=%p console=%p proc120=%p",
                (void*)g_manager, (void*)g_console, (void*)g_proc120);
    ImGui::Text("sendto captures: %d", s_sendto_calls);
    ImGui::Separator();
    ImGui::TextDisabled("console commands (executed on render thread):");
    struct { const char* label; const char* line; } cmds[] = {
        { "version",          "version" },
        { "help",             "help" },
        { "scenegraph print", "scenegraph print" },
        { "config print",     "config print" },
        { "director pause",   "director pause" },
        { "director resume",  "director resume" },
        { "debugmsg on",      "debugmsg on" },
        { "debugmsg off",     "debugmsg off" },
    };
    for (int i = 0; i < 8; i++) {
        if (i % 2 == 1) ImGui::SameLine();
        if (ImGui::Button(cmds[i].label, ImVec2(160, 0)))
            RunLine(cmds[i].line);
    }
    ImGui::Separator();
    static char cmd[256] = "version";
    ImGui::InputText("custom line", cmd, sizeof(cmd));
    if (ImGui::Button("RUN", ImVec2(80, 0)))
        RunLine(cmd);
    ImGui::End();

    /* second window: captured console output (from the sendto hook) */
    ImGui::SetNextWindowPos(ImVec2(30, 300), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(900, 500), ImGuiCond_FirstUseEver);
    ImGui::Begin("ad2mod console output");
    char text[8192];
    {
        std::lock_guard<std::mutex> lk(g_out_mu);
        size_t total = g_out.size();
        if (total == 0) {
            ImGui::TextDisabled("(no output captured yet -- run a command)");
        } else {
            size_t n = total > 8000 ? 8000 : total;   /* most recent bytes */
            memcpy(text, g_out.data() + (total - n), n);
            text[n] = 0;
            ImGui::TextUnformatted(text);
        }
    }
    if (ImGui::Button("clear output")) {
        std::lock_guard<std::mutex> lk(g_out_mu);
        g_out.clear();
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
static DWORD WINAPI Worker(LPVOID)
{
    Sleep(1500); /* let the game settle past injection */
    s_module_base = (uintptr_t)GetModuleHandleA(nullptr);

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

    /* M2: game function pointers (static RVAs, no ASLR math needed) */
    g_mgr_get   = (FnManagerGet)(s_module_base + 0x3de148);
    g_line_in   = (FnConsoleLine)(s_module_base + 0x375f30);

    /* M2: hook ws2_32 sendto/recvfrom in the game's IAT.
       Slots: connect=0x47e600, sendto=0x47e608, recvfrom=0x47e610
       (ordinal imports of ws2_32; ws2 is a load-time dependency, so the
        slots are populated by the loader before we run). */
    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    Log("worker: ws2_32.dll=%p", (void*)ws2);
    uintptr_t real_sendto   = ws2 ? (uintptr_t)GetProcAddress(ws2, "sendto")   : 0;
    uintptr_t real_recvfrom = ws2 ? (uintptr_t)GetProcAddress(ws2, "recvfrom") : 0;
    Log("worker: ws2 real sendto=%p recvfrom=%p", (void*)real_sendto, (void*)real_recvfrom);
    {
        uintptr_t* st = (uintptr_t*)(s_module_base + 0x47e608);
        uintptr_t* rf = (uintptr_t*)(s_module_base + 0x47e610);
        if (*st == real_sendto && real_sendto) {
            g_orig_sendto = (FnSendto)*st;
            PatchIatSlot(0x47e608, "sendto", (uintptr_t)Ad2HookedSendto);
        } else {
            Log("worker: sendto slot %p != ws2 sendto %p -- NOT patched (VMP rewrote?)", (void*)*st, (void*)real_sendto);
        }
        if (*rf == real_recvfrom && real_recvfrom) {
            g_orig_recvfrom = (FnRecvfrom)*rf;
            PatchIatSlot(0x47e610, "recvfrom", (uintptr_t)Ad2HookedRecvfrom);
        } else {
            Log("worker: recvfrom slot %p != ws2 recvfrom %p -- NOT patched", (void*)*rf, (void*)real_recvfrom);
        }
    }

    /* hotkey poll: Ctrl+Alt+U toggles UI */
    bool prev = false;
    for (;;) {
        Sleep(80);
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
