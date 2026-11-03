#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <knownfolders.h>
#include <shtypes.h>
#include <mutex>
#include <fstream>
#include <chrono>

namespace mod
{
	void Main();

	// Main executable module (AstroDuel2.exe)
	extern uintptr_t moduleBase;

	// Our DLL module
	extern HINSTANCE dllInstance;

	// Log file constants - will be located under the Windows Folder specified by logFileFolderId, at the relative path provided by logFile
	const KNOWNFOLDERID logFileFolderId = FOLDERID_LocalAppData;
	const std::wstring logFile = LR"(AstroDuel2\overlay.log)";

	// INTERNAL ONLY - logging values, used by templated Log function below
	namespace internal
	{
		extern std::mutex logMutex;
		extern std::ofstream logFileStream;
	}

	// Log is called from several threads - file stream must be initialized in mod::Main before any invocations
	template<typename... Args>
	void Log(std::format_string<Args...> fmt, Args&&... args)
	{
		if (!internal::logFileStream.is_open())
		{
			// Log file not initialized properly
			// TODO: Display error in overlay
			return;
		}

		std::lock_guard guard{internal::logMutex};
		auto localTime = std::chrono::current_zone()->to_local(std::chrono::system_clock::now());
		internal::logFileStream << std::format("[{:%T}] ", localTime) << std::format(fmt, std::forward<Args>(args)...) << std::endl;
	}
}
