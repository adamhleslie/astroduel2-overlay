#include "overlay.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_version.h>

#include "mod.h"
#include "ad2_iat.h"
#include "level_pool.h"

namespace
{
	constexpr std::string_view category = "overlay";
	constexpr std::string_view glslVersion = "#version 120";
	constexpr int windowRounding = 4;
	constexpr ImVec2 initialWindowSize{640, 0};
	constexpr std::string_view windowName = "mod overlay";

	decltype(&SDL_GL_CreateContext) sdl_gl_createContextOrig = nullptr;
	decltype(&SDL_GL_DestroyContext) sdl_gl_destroyContextOrig = nullptr;
	decltype(&SDL_PollEvent) sdl_pollEventOrig = nullptr;
	decltype(&SDL_GL_SwapWindow) sdl_gl_swapWindowOrig = nullptr;

	bool isInitialized = false;
	bool isImguiInitialized = false;
	bool isOverlayVisible = false;

	const bool* keyboardState = nullptr;
	bool wasHotkeyDown = false;

	SDL_GLContext SDL_GL_CreateContextHook(SDL_Window* window)
	{
		assert(sdl_gl_createContextOrig);
		SDL_GLContext glContext = sdl_gl_createContextOrig(window);
		if (!glContext)
		{
			mod::Log(category, "ERROR - CreateContext failed");
			return glContext;
		}

		if (!IMGUI_CHECKVERSION())
		{
			mod::Log(category, "ERROR - imgui version mismatch");
			return glContext;
		}

		// Setup Dear ImGui context
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
		io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

		// Setup Dear ImGui style
		ImGuiStyle& style = ImGui::GetStyle();
		style.WindowRounding = windowRounding;

		float scale = SDL_GetWindowDisplayScale(window);
		if (scale > 1.0f)
		{
			style.ScaleAllSizes(scale);
			style.FontScaleDpi = scale;
		}

		// Setup imgui backends
		if (!ImGui_ImplSDL3_InitForOpenGL(window, glContext))
		{
			mod::Log(category, "ERROR - ImGui_ImplSDL3_InitForOpenGL failed");
			return glContext;
		}

		if (!ImGui_ImplOpenGL3_Init(glslVersion.data()))
		{
			mod::Log(category, "ERROR - ImGui_ImplOpenGL3_Init failed for glsl version \"{}\"", glslVersion);
			return glContext;
		}

		isImguiInitialized = true;
		return glContext;
	}

	bool SDL_GL_DestroyContextHook(SDL_GLContext context)
	{
		assert(sdl_gl_destroyContextOrig);

		// Cleanup
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplSDL3_Shutdown();
		ImGui::DestroyContext();

		mod::Log(category, "Overlay deinitialized");
		return sdl_gl_destroyContextOrig(context);
	}

	bool SDL_PollEventHook(SDL_Event* event)
	{
		assert(sdl_pollEventOrig);
		bool hasEvent = sdl_pollEventOrig(event);
		if (isOverlayVisible && hasEvent)
		{
			ImGui_ImplSDL3_ProcessEvent(event);
		}

		return hasEvent;
	}

	void PollInput()
	{
		bool isHotkeyDown = keyboardState[SDL_SCANCODE_GRAVE];
		if (isHotkeyDown && !wasHotkeyDown)
		{
			isOverlayVisible = !isOverlayVisible;
			if (isOverlayVisible)
			{
				SDL_ShowCursor();
			}
			else
			{
				SDL_HideCursor();
			}

			mod::Log(category, "Overlay {}", isOverlayVisible ? "shown" : "hidden");
		}

		wasHotkeyDown = isHotkeyDown;
	}

	bool SDL_GL_SwapWindowHook(SDL_Window* window)
	{
		assert(sdl_gl_swapWindowOrig);
		if (window)
		{
			if (isImguiInitialized)
			{
				PollInput();

				ImGui_ImplSDL3_NewFrame();
				ImGui_ImplOpenGL3_NewFrame();
				ImGui::NewFrame();
				if (isOverlayVisible)
				{
					ImGui::SetNextWindowSize(initialWindowSize, ImGuiCond_FirstUseEver);
					ImGui::Begin(windowName.data(), nullptr, ImGuiWindowFlags_NoCollapse);
					ImGui::Separator();

					// Level Pool Editor
					level_pool::DrawUi();

					ImGui::End();
				}
				ImGui::Render();
				ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
			}
		}

		return sdl_gl_swapWindowOrig(window);
	}
}

bool overlay::Init()
{
	assert(!isInitialized);

	const int sdlVersion = SDL_GetVersion();
	mod::Log(category, "SDL Version: {}.{}.{}", SDL_VERSIONNUM_MAJOR(sdlVersion), SDL_VERSIONNUM_MINOR(sdlVersion), SDL_VERSIONNUM_MICRO(sdlVersion));

	// Cache pointer to keyboard for future polling
	keyboardState = SDL_GetKeyboardState(nullptr);

	// Hook SDL iat
	if (!ad2::iat::HookIatForModule("SDL3.dll", NAME_OF(SDL_GL_CreateContext), SDL_GL_CreateContextHook, sdl_gl_createContextOrig) ||
		!ad2::iat::HookIatForModule("SDL3.dll", NAME_OF(SDL_GL_DestroyContext), SDL_GL_DestroyContextHook, sdl_gl_destroyContextOrig) ||
		!ad2::iat::HookIatForModule("SDL3.dll", NAME_OF(SDL_PollEvent), SDL_PollEventHook, sdl_pollEventOrig) ||
		!ad2::iat::HookIatForModule("SDL3.dll", NAME_OF(SDL_GL_SwapWindow), SDL_GL_SwapWindowHook, sdl_gl_swapWindowOrig))
	{
		return false;
	}

	isInitialized = true;
	mod::Log(category, "Overlay initialized");
	return true;
}
