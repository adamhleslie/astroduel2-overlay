#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shtypes.h>
#include <filesystem>
#include <mutex>
#include <fstream>
#include <chrono>
#include <format>
#include <enchantum/std_format.hpp>

namespace mod
{
	/* Mod's Main function run on a thread spun up on DllMain. This is the entry point for:
	 *   1. Setting up logging
	 *   2. Waiting for VMP to unpack the executable in place
	 *   3. Initializing the imgui overlay
	 *   4. Installing hooks
	 *   5. Running the overlay loop
	 */
	void Main();

	// Main executable module (AstroDuel2.exe)
	extern uintptr_t ad2ModuleBase;

	// Our DLL module
	extern HINSTANCE dllModule;

	// The game's resources directory ("Astro Duel 2/res")
	extern std::filesystem::path resourcesPath;

	// Log file constants - file will be located under the Windows Folder specified by logFileFolderId, at the relative path provided by logFile
	const KNOWNFOLDERID logFileFolderId = FOLDERID_LocalAppData;
	const std::wstring logFile = LR"(AstroDuel2\overlay.log)";

	// INTERNAL ONLY - logging values, used by templated Log function below
	namespace detail
	{
		extern std::mutex logMutex;
		extern std::ofstream logStream;
	}

	// TODO: Replace "module" nomenclature with "category"
	// Log is called from several threads - file stream must be initialized in mod::Main before any invocations
	template<typename... Args>
	void Log(const std::string& module, std::format_string<Args...> fmt, Args&&... args)
	{
		if (!detail::logStream.is_open())
		{
			// Log file not initialized before call to Log
			// TODO: Display error in overlay
			return;
		}

		std::lock_guard guard{detail::logMutex};
		auto localTime = std::chrono::current_zone()->to_local(std::chrono::system_clock::now());
		detail::logStream << std::format("[{:%T}] [{}] {}", localTime, module, std::format(fmt, std::forward<Args>(args)...)) << std::endl;
	}
}
