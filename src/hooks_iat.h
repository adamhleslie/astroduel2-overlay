#pragma once

// Import Address Table (IAT) Hooks
// Currently used for logging windows file apis
namespace hooks::iat
{
	void InstallHooks();
}
