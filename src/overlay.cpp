/* Render strategy:
*   - IAT-hooks SDL_GL_SwapWindow on the game's render thread
*   - Lazy-inits ImGui (GLSL "#version 120" for the game's 2.1-compat context) on first swap
*   - Input: manual Win32 (GetCursorPos/ScreenToClient + GetAsyncKeyState)
*   - UI toggle: Ctrl+Alt+U (polled in worker thread, edge-triggered)
*/
#include "overlay.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_opengl3_loader.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_version.h>

#include "mod.h"
#include "hooks_iat.h"
#include "level_pool.h"

namespace
{
	constexpr std::string_view category = "overlay";
	constexpr std::string_view windowHandlePropertyKey = "SDL.window.win32.hwnd";
	constexpr std::string_view glslVersion = "#version 120";
	constexpr int imguiWindowRounding = 4;

	decltype(&SDL_GL_SwapWindow) sdl_gl_swapWindowOrig = nullptr;
	decltype(&SDL_GL_CreateContext) sdl_gl_createContextOrig = nullptr;
	SDL_Window* sdlWindow = nullptr;
	bool isInitialized = false;

	bool isImguiInitialized = false;
	HWND windowHandle = nullptr;
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
		mod::Log("DEBUG", "FALLBACK GAME WINDOW SEARCH");
		WinCand best = {nullptr, -1};
		EnumWindows(EnumWinProc, (LPARAM) &best);
		return best.h;
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
		GetClientRect(windowHandle, &rc);
		int sw = 0, sh = 0;
		if (sdlWindow) SDL_GetWindowSizeInPixels(sdlWindow, &sw, &sh);
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
		ScreenToClient(windowHandle, &p);
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

		// Level Pool Editor
		level_pool::DrawUi();

		ImGui::End();
	}

	SDL_GLContext SDL_GL_CreateContextHook(SDL_Window* window)
	{
		assert(sdl_gl_createContextOrig);
		SDL_GLContext glContext = sdl_gl_createContextOrig(window);
		if (!glContext)
		{
			mod::Log(category, "ERROR - CreateContext failed");
			return glContext;
		}

		// Cache window info
		sdlWindow = window;
		SDL_PropertiesID properties = SDL_GetWindowProperties(window);
		if (!properties)
		{
			mod::Log(category, "ERROR - Could not get window properties");
			return glContext;
		}
		windowHandle = static_cast<HWND>(SDL_GetPointerProperty(properties, windowHandlePropertyKey.data(), nullptr));

		// Initialize imgui
		if (!IMGUI_CHECKVERSION())
		{
			mod::Log(category, "ERROR - imgui version mismatch");
			return glContext;
		}

		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

		ImGuiStyle& st = ImGui::GetStyle();
		st.WindowRounding = imguiWindowRounding;

		if (!ImGui_ImplOpenGL3_Init(glslVersion.data()))
		{
			mod::Log(category, "ERROR - ImGui_ImplOpenGL3_Init failed for version \"{}\"", glslVersion);
			return glContext;
		}

		isImguiInitialized = true;
		return glContext;
	}

	bool SDL_GL_SwapWindowHook(SDL_Window* window)
	{
		assert(sdl_gl_swapWindowOrig);
		if (window && isImguiInitialized)
		{
			++frame;
			ImGui_ImplOpenGL3_NewFrame();
			FeedInput(); /* update io.DisplaySize + queue input BEFORE NewFrame */
			if (!glInfoLogged)
			{
				glInfoLogged = true;
				mod::Log(category, "window={} display={}x{}", (void*) window, (int) ImGui::GetIO().DisplaySize.x, (int) ImGui::GetIO().DisplaySize.y);
			}
			ImGui::NewFrame();
			if (showUi) DrawUi();
			ImGui::Render();
			ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		}

		return sdl_gl_swapWindowOrig(window);
	}
}

bool overlay::Init()
{
	assert(!isInitialized);

	const int sdlVersion = SDL_GetVersion();
	mod::Log(category, "SDL Version: {}.{}.{}", SDL_VERSIONNUM_MAJOR(sdlVersion), SDL_VERSIONNUM_MINOR(sdlVersion), SDL_VERSIONNUM_MICRO(sdlVersion));

	// Hook the game's CreateContext function
	if (!hooks::iat::HookIatForModule("SDL3.dll", NAME_OF(SDL_GL_CreateContext), SDL_GL_CreateContextHook, sdl_gl_createContextOrig))
	{
		return false;
	}

	// Hook the game's SwapWindow function
	if (!hooks::iat::HookIatForModule("SDL3.dll", NAME_OF(SDL_GL_SwapWindow), SDL_GL_SwapWindowHook, sdl_gl_swapWindowOrig))
	{
		return false;
	}

	isInitialized = true;
	mod::Log(category, "Overlay initialized");
	return true;
}

void overlay::Loop()
{
	/* hotkey poll: Ctrl+Alt+U toggles UI */
	bool prev = false;
	for (;;)
	{
		Sleep(80);
		bool d = KeyDown(VK_CONTROL) && KeyDown(VK_MENU) && KeyDown('U');
		if (d && !prev)
		{
			showUi = !showUi;
			if (showUi)
			{
				SDL_ShowCursor();
				/* put the cursor somewhere visible: screen center */
				POINT c;
				GetCursorPos(&c);
				SetCursorPos(GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2);
			}
			else
			{
				SDL_HideCursor();
			}
			mod::Log(category, "UI {}", showUi ? "shown (cursor on, centered)" : "hidden (cursor off)");
		}
		prev = d;
	}
}
