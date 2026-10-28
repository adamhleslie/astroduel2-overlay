#pragma once

#include <string>
#include <vector>

namespace level_pool
{
	void DrawUi();
	void Init();
	int PoolWorldEffectiveCount(int worldType);
	const char* PoolWorldName(int id);
	int PoolWorldId(const char* name);

	struct PoolFile
	{
		std::string name;
		std::string path;
		bool active;
		int slot;
	};

	struct PoolWorld
	{
		int id;
		std::string dir;
		std::vector<PoolFile> files;
	};

	extern std::vector<PoolWorld> poolWorlds;
	extern volatile long poolReady;

	constexpr int poolWorldMax = 16; /* world ids 1..15 */
	constexpr int poolCap = 12; /* reachable ceiling: the walk pool only ever emits 1..12 */
	extern int poolWorldById[poolWorldMax];

	extern volatile int randMode;
}
