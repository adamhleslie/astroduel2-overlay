#pragma once

// Function declarations - do not call, they are not defined!
namespace ad2::functions
{
	void* GetWorldData(void* out, int worldType); // ad2::addresses::getWorldData
	void* GetLevel(void* out, int world, int num, char mode); // ad2::addresses::getLevel
}