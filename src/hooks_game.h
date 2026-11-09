#pragma once

// Game Hooks via Minhook library
// TODO: Cleanup
namespace hooks::game
{
	bool InstallGetWorldDataHook();
	bool InstallGetLevelHook();
}
