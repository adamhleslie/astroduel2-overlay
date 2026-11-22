#pragma once

// Function declarations - do not call, they are not defined!
// See template type usage in ad2::addresses
namespace ad2::functions
{
	void* GetWorldData(void* out, int worldType);
	void GetLevelPath(std::string* out, int worldType, int levelNum, bool cashGrab);
}