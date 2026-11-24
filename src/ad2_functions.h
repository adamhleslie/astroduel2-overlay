#pragma once

#include "ad2.h"

// Function declarations - do not call, they are not defined!
// See template type usage in ad2::addresses
namespace ad2::functions
{
	WorldData* GetWorldData(WorldData* out, int worldType);
	void GetLevelPath(std::string* levelPathOut, int worldType, int levelNum, bool forUi);
}