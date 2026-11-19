#include "overlay.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_version.h>
#include <atomic>

#include "mod.h"
#include "hooks_iat.h"
#include "level_pool.h"

namespace
{
	constexpr std::string_view category = "overlay";
	constexpr std::string_view glslVersion = "#version 120";
	constexpr int imguiWindowRounding = 4;
	constexpr std::chrono::milliseconds hotkeyPollSleep{80};

	decltype(&SDL_GL_CreateContext) sdl_gl_createContextOrig = nullptr;
	decltype(&SDL_GL_DestroyContext) sdl_gl_destroyContextOrig = nullptr;
	decltype(&SDL_PollEvent) sdl_pollEventOrig = nullptr;
	decltype(&SDL_GL_SwapWindow) sdl_gl_swapWindowOrig = nullptr;

	bool isInitialized = false;
	bool isImguiInitialized = false;
	bool isOverlayVisible = false;
	std::atomic_bool eventShowCursor{false};
	std::atomic_bool eventHideCursor{false};

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
		style.WindowRounding = imguiWindowRounding;

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

	bool SDL_GL_SwapWindowHook(SDL_Window* window)
	{
		assert(sdl_gl_swapWindowOrig);

		if (window)
		{
			// Handle main thread events
			if (eventShowCursor.exchange(false))
			{
				SDL_ShowCursor();
			}
			else if (eventHideCursor.exchange(false))
			{
				SDL_HideCursor();
			}

			if (isImguiInitialized)
			{
				ImGui_ImplSDL3_NewFrame();
				ImGui_ImplOpenGL3_NewFrame();
				ImGui::NewFrame();
				if (isOverlayVisible)
				{
					ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_FirstUseEver);
					ImGui::Begin("mod overlay", nullptr, ImGuiWindowFlags_NoCollapse);
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

	// Hook SDL iat
	if (!hooks::iat::HookIatForModule("SDL3.dll", NAME_OF(SDL_GL_CreateContext), SDL_GL_CreateContextHook, sdl_gl_createContextOrig) ||
		!hooks::iat::HookIatForModule("SDL3.dll", NAME_OF(SDL_GL_DestroyContext), SDL_GL_DestroyContextHook, sdl_gl_destroyContextOrig) ||
		!hooks::iat::HookIatForModule("SDL3.dll", NAME_OF(SDL_PollEvent), SDL_PollEventHook, sdl_pollEventOrig) ||
		!hooks::iat::HookIatForModule("SDL3.dll", NAME_OF(SDL_GL_SwapWindow), SDL_GL_SwapWindowHook, sdl_gl_swapWindowOrig))
	{
		return false;
	}

	isInitialized = true;
	mod::Log(category, "Overlay initialized");
	return true;
}

// TODO: Move this to SDL_GL_SwapWindowHook?
void overlay::PollInputLoop()
{
	// Poll Hotkey
	bool wasHotkeyDown = false;
	const bool* keyboardState = SDL_GetKeyboardState(nullptr);
	while (true)
	{
		bool isHotkeyDown = keyboardState[SDL_SCANCODE_GRAVE];
		if (isHotkeyDown && !wasHotkeyDown)
		{
			isOverlayVisible = !isOverlayVisible;

			// Cache the SDL show/hide cursor events until the next SDL_GL_SwapWindowHook since the api can only be called on the main thread
			if (isOverlayVisible)
			{
				eventShowCursor.store(true);
			}
			else
			{
				eventHideCursor.store(true);
			}

			mod::Log(category, "Overlay {}", isOverlayVisible ? "shown" : "hidden");
		}

		wasHotkeyDown = isHotkeyDown;
		std::this_thread::sleep_for(hotkeyPollSleep);
	}
}
