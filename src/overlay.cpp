/* Render strategy:
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
#include "overlay.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_opengl3_loader.h>

#include "mod.h"
#include "level_pool.h"

namespace
{
	constexpr std::string_view category = "imgui";

	int (*origSwap)(void*) = nullptr; /* original SDL_GL_SwapWindow */
	/* SDL3 dropped per-platform SDL_GetWindowHWND; the HWND is a window
	   property. Resolve the two exported functions we need to read it. */
	typedef unsigned int (*FnSdlGetWindowProperties)(void* window);

	typedef void* (*FnSdlGetPointerProperty)(unsigned int props, const char* name, void* def);

	FnSdlGetWindowProperties sdlGetWindowProps = nullptr;
	FnSdlGetPointerProperty sdlGetPointerProp = nullptr;

	/* SDL_GetWindowSizeInPixels(SDL_Window*, int*, int*) -> SDL_bool */
	typedef int (*FnSdlGetWindowSizeInPixels)(void* window, int* w, int* h);

	FnSdlGetWindowSizeInPixels sdlGetSizePx = nullptr;

	typedef int (*FnSdlCursor)(int); /* SDL_ShowCursor/SDL_HideCursor (SDL3: int mode) */
	FnSdlCursor sdlShowCursor = nullptr;
	FnSdlCursor sdlHideCursor = nullptr;
	void* sdlWindow = nullptr;
	const char hwndPropKey[] = "SDL.window.win32.hwnd";
	volatile LONG hookInstalled = 0;

	bool imguiReady = false;
	HWND hwnd = nullptr;
	int frame = 0;
	bool glInfoLogged = false;
	bool showUi = false; /* Ctrl+Alt+U toggles (off by default) */

	/* input edge-state */
	bool mouseButton[5] = {false, false, false, false, false};
#ifndef VK_MOUSEWHEEL
#define VK_MOUSEWHEEL 0x00E5
#endif

	bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

	/* ------------------------------------------------------------------ */
	/* find the game window                                                */
	/* ------------------------------------------------------------------ */
	struct WinCand
	{
		HWND h;
		int area;
	};

	BOOL CALLBACK EnumWinProc(HWND h, LPARAM lp)
	{
		WinCand* best = (WinCand*) lp;
		DWORD pid = 0;
		GetWindowThreadProcessId(h, &pid);
		if (pid != GetCurrentProcessId()) return TRUE;
		if (!IsWindowVisible(h)) return TRUE;
		RECT r;
		GetClientRect(h, &r);
		int area = (r.right - r.left) * (r.bottom - r.top);
		if (area > best->area)
		{
			best->area = area;
			best->h = h;
		}
		return TRUE;
	}

	HWND FindGameWindow()
	{
		WinCand best = {nullptr, -1};
		EnumWindows(EnumWinProc, (LPARAM) &best);
		return best.h;
	}

	/* ------------------------------------------------------------------ */
	/* hook                                                                */
	/* ------------------------------------------------------------------ */
	void ImGuiInit()
	{
		mod::Log(category, "hwnd={} dc={}", (void*)hwnd, (void*)wglGetCurrentDC());
		if (!hwnd) return;
		if (!wglGetCurrentDC())
		{
			mod::Log(category, "no GL context yet, deferring");
			return;
		}

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

		/* io.DisplaySize defaults to (-1,-1) in 1.90.9 -- seed a valid value so
		   even a NewFrame before FeedInput could not assert. */
		{
			RECT rc = {0, 0, 0, 0};
			if (GetClientRect(hwnd, &rc) && rc.right > 0 && rc.bottom > 0)
				io.DisplaySize = ImVec2((float) rc.right, (float) rc.bottom);
			else
				io.DisplaySize = ImVec2(1920.0f, 1080.0f);
		}

		/* HiDPI: we lay out in PHYSICAL pixels (DisplaySize = GL viewport, e.g.
		   3840x2160) with FramebufferScale (1,1). The default 13px font is then
		   physically tiny at 4K. Rasterize the font at the monitor's DPI so the
		   UI is physically sized right (13px * 2.0 = 26px at 200%). GetClientRect
		   is unreliable on the SDL fullscreen window, so read DPI from the hwnd. */
		{
			HWND w = hwnd ? hwnd : NULL;
			HDC hdc = GetDC(w);
			int dpi = (hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96);
			if (hdc) ReleaseDC(w, hdc);
			if (dpi <= 0) dpi = 96;
			float scale = dpi / 96.0f;
			if (scale < 1.0f) scale = 1.0f;
			ImFontConfig fc;
			fc.SizePixels = 13.0f * scale;
			io.Fonts->AddFontDefault(&fc);
			mod::Log(category, "dpi={} scale={:.2f} font={:.0f}px", dpi, scale, fc.SizePixels);
		}

		ImGuiStyle& st = ImGui::GetStyle();
		st.WindowRounding = 4.0f;

		/* GLSL 120: the game gates on "Ready for OpenGL 2.1"; #version 120
		   compiles on 2.1 compat and desktop 3.x alike. */
		if (!ImGui_ImplOpenGL3_Init("#version 120"))
		{
			mod::Log(category, "ImGui_ImplOpenGL3_Init FAILED (shader compile?)");
			return;
		}
		imguiReady = true;
		mod::Log(category, "OK (GLSL #version 120)");
	}

	void FeedInput()
	{
		ImGuiIO& io = ImGui::GetIO();

		/* delta time via system timer */
		UINT64 last = 0;
		UINT64 now = GetTickCount64();
		if (last) io.DeltaTime = (float) ((now - last) / 1000.0);
		last = now;
		if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0f / 60.0f;

		/* display size: the game sets glViewport every frame before SwapWindow,
		   so the GL viewport IS the true drawable size (physical pixels, DPI-
		   agnostic). Fallbacks: SDL pixel size, then logical client rect. */
		int vw[4] = {0, 0, 0, 0};
		glGetIntegerv(GL_VIEWPORT, vw);
		RECT rc = {0, 0, 0, 0};
		GetClientRect(hwnd, &rc);
		int sw = 0, sh = 0;
		if (sdlWindow && sdlGetSizePx) sdlGetSizePx(sdlWindow, &sw, &sh);
		// bool s_size_logged = false;
		// if (!s_size_logged)
		// {
		// 	s_size_logged = true;
		// 	mod::Log("sizes: gl_vp=%dx%d sdl_px=%dx%d client=%dx%d", vw[2], vw[3], sw, sh, rc.right, rc.bottom);
		// }
		if (vw[2] > 0 && vw[3] > 0)
			io.DisplaySize = ImVec2((float) vw[2], (float) vw[3]);
		else if (sw > 0 && sh > 0)
			io.DisplaySize = ImVec2((float) sw, (float) sh);
		else if (rc.right > 0 && rc.bottom > 0)
			io.DisplaySize = ImVec2((float) rc.right, (float) rc.bottom);
		else if (io.DisplaySize.x < 1.0f)
			io.DisplaySize = ImVec2(1920.0f, 1080.0f);

		/* mouse pos (client coords, scaled logical->physical) */
		POINT p;
		GetCursorPos(&p);
		ScreenToClient(hwnd, &p);
		{
			float sx = (rc.right > 0) ? io.DisplaySize.x / (float) rc.right : 1.0f;
			float sy = (rc.bottom > 0) ? io.DisplaySize.y / (float) rc.bottom : 1.0f;
			io.AddMousePosEvent((float) p.x * sx, (float) p.y * sy);
		}

		/* mouse buttons (edge via AddMouseButtonEvent: it takes a bool state
		   per event and imgui dedupes; simplest is to send current state only
		   on change) */
		const int vkmap[5] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
		for (int i = 0; i < 5; i++)
		{
			bool d = KeyDown(vkmap[i]);
			if (d != mouseButton[i])
			{
				io.AddMouseButtonEvent(i, d);
				mouseButton[i] = d;
			}
		}

		/* keyboard: map common VKs to ImGuiKey; send state every frame via
		   AddKeyEvent (imgui handles repeated same-state events) */
		struct
		{
			int vk;
			ImGuiKey k;
		} kmap[] = {
			{VK_ESCAPE, ImGuiKey_Escape}, {VK_TAB, ImGuiKey_Tab},
			{VK_RETURN, ImGuiKey_Enter}, {VK_BACK, ImGuiKey_Backspace},
			{VK_DELETE, ImGuiKey_Delete}, {VK_SPACE, ImGuiKey_Space},
			{VK_LEFT, ImGuiKey_LeftArrow}, {VK_RIGHT, ImGuiKey_RightArrow},
			{VK_UP, ImGuiKey_UpArrow}, {VK_DOWN, ImGuiKey_DownArrow},
			{VK_SHIFT, ImGuiKey_LeftShift}, {VK_CONTROL, ImGuiKey_LeftCtrl},
			{VK_MENU, ImGuiKey_LeftAlt}, {VK_LWIN, ImGuiKey_LeftSuper},
			{0x41, ImGuiKey_A}, {0x42, ImGuiKey_B}, {0x43, ImGuiKey_C},
			{0x44, ImGuiKey_D}, {0x45, ImGuiKey_E}, {0x46, ImGuiKey_F},
			{0x47, ImGuiKey_G}, {0x48, ImGuiKey_H}, {0x49, ImGuiKey_I},
			{0x4A, ImGuiKey_J}, {0x4B, ImGuiKey_K}, {0x4C, ImGuiKey_L},
			{0x4D, ImGuiKey_M}, {0x4E, ImGuiKey_N}, {0x4F, ImGuiKey_O},
			{0x50, ImGuiKey_P}, {0x51, ImGuiKey_Q}, {0x52, ImGuiKey_R},
			{0x53, ImGuiKey_S}, {0x54, ImGuiKey_T}, {0x55, ImGuiKey_U},
			{0x56, ImGuiKey_V}, {0x57, ImGuiKey_W}, {0x58, ImGuiKey_X},
			{0x59, ImGuiKey_Y}, {0x5A, ImGuiKey_Z},
			{0x30, ImGuiKey_0}, {0x31, ImGuiKey_1}, {0x32, ImGuiKey_2},
			{0x33, ImGuiKey_3}, {0x34, ImGuiKey_4}, {0x35, ImGuiKey_5},
			{0x36, ImGuiKey_6}, {0x37, ImGuiKey_7}, {0x38, ImGuiKey_8},
			{0x39, ImGuiKey_9},
		};
		for (auto& e: kmap)
			io.AddKeyEvent(e.k, KeyDown(e.vk));

		io.AddMouseWheelEvent(0.0f, (float) ((SHORT) (GetAsyncKeyState(VK_MOUSEWHEEL) >> 8)) / 120.0f);
	}

	void DrawUi()
	{
		ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_FirstUseEver);
		ImGui::Begin("ad2mod overlay", nullptr, ImGuiWindowFlags_NoCollapse);
		ImGui::Separator();

		/* ---------------- versus level-pool editor ---------------- */

		level_pool::DrawUi();
		ImGui::End();
	}

	/* the hook: same ABI as SDL_GL_SwapWindow(SDL_Window*) -> int */
	int Ad2HookedSwap(void* window)
	{
		if (hookInstalled)
		{
			/* the hook parameter IS the game's SDL_Window -- authoritative HWND
			   source (better than window enumeration). Read it via SDL3's
			   property API: GetWindowProperties -> GetPointerProperty("...hwnd") */
			if (window) sdlWindow = window;
			if (!hwnd && window && sdlGetWindowProps && sdlGetPointerProp)
			{
				unsigned int props = sdlGetWindowProps(window);
				if (props) hwnd = (HWND) sdlGetPointerProp(props, hwndPropKey, nullptr);
			}
			if (!hwnd) hwnd = FindGameWindow(); /* last-resort fallback */
			if (!imguiReady) ImGuiInit();
			if (imguiReady)
			{
				frame++;
				ImGui_ImplOpenGL3_NewFrame();
				FeedInput(); /* update io.DisplaySize + queue input BEFORE NewFrame */
				if (!glInfoLogged)
				{
					glInfoLogged = true;
					mod::Log(category, "window={} display={}x{}", (void*) window,
					         (int) ImGui::GetIO().DisplaySize.x, (int) ImGui::GetIO().DisplaySize.y);
				}
				ImGui::NewFrame();
				if (showUi) DrawUi();
				ImGui::Render();
				ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
			}
		}
		return origSwap ? origSwap(window) : 0;
	}
}

bool overlay::Init()
{
	/* SDL3.dll loads early in the game's own startup; it may not exist yet at import resolution, so poll. */
	HMODULE s3 = nullptr;
	for (int i = 0; i < 450 && !s3; i++)
	{
		/* up to 90 s */
		s3 = GetModuleHandleA("SDL3.dll");
		if (!s3) Sleep(200);
	}
	mod::Log(category, "module_base={} SDL3.dll={}", (void*) mod::ad2ModuleBase, (void*) s3);
	if (!s3)
	{
		mod::Log(category, "SDL3.dll NOT loaded after 90s -- giving up");
		return false;
	}

	sdlGetWindowProps = (FnSdlGetWindowProperties) GetProcAddress(s3, "SDL_GetWindowProperties");
	sdlGetPointerProp = (FnSdlGetPointerProperty) GetProcAddress(s3, "SDL_GetPointerProperty");
	sdlGetSizePx = (FnSdlGetWindowSizeInPixels) GetProcAddress(s3, "SDL_GetWindowSizeInPixels");
	sdlShowCursor = (FnSdlCursor) GetProcAddress(s3, "SDL_ShowCursor");
	sdlHideCursor = (FnSdlCursor) GetProcAddress(s3, "SDL_HideCursor");
	mod::Log(category, "worker: GetWindowProperties={} GetPointerProperty={} GetWindowSizeInPixels={} ShowCursor={} HideCursor={}",
		(void*) sdlGetWindowProps, (void*) sdlGetPointerProp,
		(void*) sdlGetSizePx, (void*) sdlShowCursor, (void*) sdlHideCursor);
	uintptr_t real = (uintptr_t) GetProcAddress(s3, "SDL_GL_SwapWindow");
	mod::Log(category, "SDL_GL_SwapWindow = {}", (void*) real);
	if (!real)
	{
		mod::Log(category, "GetProcAddress failed ({})", GetLastError());
		return false;
	}

	/* scan the static-IAT region (RVA 0x47e028..0x47eba0) for the slot.
	   The game may fill slots at load or resolve later; retry until found. */
	uintptr_t* slot = nullptr;
	for (int attempt = 0; attempt < 100 && !slot; attempt++)
	{
		uintptr_t* begin = (uintptr_t*) (mod::ad2ModuleBase + 0x47e028);
		uintptr_t* end = (uintptr_t*) (mod::ad2ModuleBase + 0x47eba0);
		for (uintptr_t* it = begin; it < end; it++)
		{
			if (*it == real)
			{
				slot = it;
				break;
			}
		}
		if (!slot) Sleep(200);
	}
	if (!slot)
	{
		mod::Log(category, "IAT slot for SwapWindow NOT FOUND in 0x47e028..0x47eba0");
		return false;
	}
	mod::Log(category, "IAT slot at {} (content={})", (void*) slot, (void*) *slot);

	/* IAT lives in .rdata (read-only) -- flip the page, patch, restore. */
	origSwap = (int(*)(void*)) *slot;
	uintptr_t page = (uintptr_t) slot & ~(uintptr_t) 0xFFF;
	DWORD oldProt = 0;
	if (!VirtualProtect((LPVOID) page, 0x1000, PAGE_READWRITE, &oldProt))
	{
		mod::Log(category, "VirtualProtect({}) FAILED: {}", (void*) page, GetLastError());
		return false;
	}
	*slot = (uintptr_t) Ad2HookedSwap;
	DWORD dummy = 0;
	VirtualProtect((LPVOID) page, 0x1000, oldProt, &dummy);
	InterlockedExchange(&hookInstalled, 1);
	mod::Log(category, "HOOK INSTALLED (orig={}, page {} prot {:#x}->restored)",
	         (void*) origSwap, (void*) page, oldProt);
	mod::Log(category, "level-pool overlay ready (Ctrl+Alt+U toggles UI)\n");
	return true;
}

void overlay::Loop()
{
	/* hotkey poll: Ctrl+Alt+U toggles UI */
	bool prev = false;
	// int tick = 0;
	for (;;)
	{
		Sleep(80);
		// if ((++tick % 62) == 0) VerifyEntryHooks();    /* ~5s: entries still ours? */
		bool d = KeyDown(VK_CONTROL) && KeyDown(VK_MENU) && KeyDown('U');
		if (d && !prev)
		{
			showUi = !showUi;
			if (showUi)
			{
				if (sdlShowCursor) sdlShowCursor(1);
				/* put the cursor somewhere visible: screen center */
				POINT c;
				GetCursorPos(&c);
				SetCursorPos(GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2);
			}
			else
			{
				if (sdlHideCursor) sdlHideCursor(0);
			}
			mod::Log(category, "UI {}", showUi ? "shown (cursor on, centered)" : "hidden (cursor off)");
		}
		prev = d;
	}
}
