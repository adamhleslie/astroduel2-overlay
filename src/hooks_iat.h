#pragma once

// Import Address Table (IAT) Hooks
// Currently used for logging Windows file apis
namespace hooks::iat
{
	void InstallHooks();
}
