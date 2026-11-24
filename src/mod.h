#pragma once

#include <windows.h>
#include <knownfolders.h>
#include <shtypes.h>
#include <mutex>
#include <fstream>
#include <chrono>
#include <format>
#include <enchantum/std_format.hpp>

#define NAME_OF(x) #x

// Core of the mod, including the Main function and logging functionality
namespace mod
{
	// Mod's DLL module
	extern HINSTANCE dllModule;

	// Log file constants - file will be located under the Windows Folder specified by logFileFolderId, at the relative path provided by logFile
	const KNOWNFOLDERID logFileFolderId = FOLDERID_LocalAppData;
	const std::wstring logFile = LR"(AstroDuel2\overlay.log)";
	constexpr bool verboseLoggingEnabled = false;

	// Mod's Main function run on a thread spun up on DllMain
	void Main();

	namespace detail
	{
		extern std::mutex logMutex;
		extern std::ofstream logStream;
	}

	// TODO: Add log view in overlay
	// Log is called from several threads - stream must be initialized in mod::Main before any invocations
	template<typename... Args>
	void Log(std::string_view category, std::format_string<Args...> fmt, Args&&... args)
	{
		if (!detail::logStream.is_open())
		{
			// Log file not initialized before call to Log
			// TODO: Display error in overlay
			return;
		}

		std::lock_guard guard{detail::logMutex};
		auto localTime = std::chrono::current_zone()->to_local(std::chrono::system_clock::now());
		detail::logStream << std::format("[{:%T}] [{}] {}", localTime, category, std::format(fmt, std::forward<Args>(args)...)) << std::endl;
	}

	template<typename... Args>
#pragma warning(suppress: 4100)
	void LogVerbose(std::string_view category, std::format_string<Args...> fmt, Args&&... args)
	{
		if constexpr (verboseLoggingEnabled)
		{
			Log(category, fmt, std::forward<Args>(args)...);
		}
	}
}
