#include "mod.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shlobj_core.h>
#include <winnt.h>

#include "ad2.h"
#include "hooks_iat.h"
#include "hooks_game.h"
#include "level_pool.h"
#include "overlay.h"

// extern globals
HINSTANCE mod::dllModule = nullptr;
std::mutex mod::detail::logMutex;
std::ofstream mod::detail::logStream;

namespace
{
	constexpr std::string_view category = "mod";
	constexpr std::string_view unpackCategory = "unpack-gate";
	
	// When DLL loaded, the VMP stub has NOT run yet and not unpacked the game's executable yet.
	// Poll until the unpacked image is verifiable, checking the following 20-byte prologue.
	constexpr uintptr_t unpackPrologueOffset = 0x26cce0;
	constexpr std::array<uint8_t, 20> unpackedPrologue =
	{
		0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
		0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20
	};

	constexpr int unpackTestIterations = 450;
	constexpr int unpackTestDelayMs = 45;
	constexpr int unpackMaxLogCount = 10;
	constexpr int unpackLogModulo = 5;

	bool WaitForUnpack()
	{
		const uint8_t* prologue = static_cast<const uint8_t*>(ad2::GetAddressFromAnchor(unpackPrologueOffset));
		auto tStart = std::chrono::high_resolution_clock::now();
		auto getDuration = [tStart]
		{
			auto tEnd = std::chrono::high_resolution_clock::now();
			return std::chrono::duration_cast<std::chrono::duration<double>>(tEnd - tStart).count();
		};

		int logCount = 0;
		for (int i = 0; i < unpackTestIterations; ++i)
		{
			if (memcmp(prologue, unpackedPrologue.data(), unpackedPrologue.size()) == 0)
			{
				mod::Log(unpackCategory, "Unpacked after {:.2f}s", getDuration());
				return true;
			}
			if ((i % unpackLogModulo == 0) && (++logCount <= unpackMaxLogCount))
			{
				mod::Log(unpackCategory, "Still packed after {:.2f}s ...", getDuration());
			}
			Sleep(unpackTestDelayMs);
		}

		mod::Log(unpackCategory, "TIMEOUT after {} - packed bytes at +0x26cce0: {:02X} {:02X} {:02X} .. {:02X} {:02X} {:02X}", getDuration(), prologue[0], prologue[1], prologue[2], prologue[17], prologue[18], prologue[19]);
		return false;
	}

	bool InitLog()
	{
		// Construct the path to the log file
		std::filesystem::path logFilePath;
		{
			// Get the local app data path on windows, which our log file is relative to
			PWSTR appDataPath;
			if (SHGetKnownFolderPath(mod::logFileFolderId, 0, nullptr, &appDataPath) != S_OK)
			{
				// Could not retrieve the local app data path
				CoTaskMemFree(appDataPath);
				return false;
			}
			logFilePath = std::filesystem::path{appDataPath};
			CoTaskMemFree(appDataPath);
		}
		logFilePath.append(mod::logFile);

		if (!logFilePath.has_filename())
		{
			// No file provided
			return false;
		}

		if (logFilePath.has_parent_path())
		{
			std::error_code error;
			std::filesystem::create_directories(logFilePath.parent_path(), error);
			if (error.value())
			{
				// Could not create directories
				return false;
			}
		}

		mod::detail::logStream.open(logFilePath, std::ofstream::out | std::ofstream::app);
		return mod::detail::logStream.is_open();
	}
}

void mod::Main()
{
	if (!InitLog())
	{
		// TODO: Display error in overlay
		return;
	}

	ad2::Init();
	Log(category, "build {} {} | pid={} | dllModuleBase={} ad2ModuleBase={}", __DATE__, __TIME__, GetCurrentProcessId(), reinterpret_cast<void*>(dllModule), reinterpret_cast<void*>(ad2::ad2ModuleBase));

	// Read files from disk to build our level pool asap, since we are in a race condition against the game thread's unpack + file loading
	level_pool::Init();

	// Wait until VMP has unpacked the image in place
	if (!WaitForUnpack())
	{
		Log(category, "ABORTING - game not unpacked");
		return;
	}

	// Initialize the overlay
	if (!overlay::Init())
	{
		Log(category, "ABORTING - overlay not initialized");
		return;
	}

	// Install Import Address Table hooks
	hooks::iat::InstallHooks();

	// Install game hooks
	hooks::game::InstallGetWorldDataHook();
	hooks::game::InstallGetLevelHook();

	// Run the overlay loop
	overlay::Loop();
}
