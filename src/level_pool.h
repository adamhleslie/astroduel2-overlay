#pragma once

#include <string>
#include <vector>

namespace level_pool
{
	void DrawUi();
	void Init();
	int PoolWorldEffectiveCount(int worldType);
	const char* IdToWorldName(int id);
	int WorldNameToId(const std::string& name);

	struct Level
	{
		std::string name;
		std::string gamePath;
		bool active = true;
		int slot = 0;
	};

	struct World
	{
		int id;
		std::string name;
		std::vector<Level> levels;
	};

	extern std::vector<World> worlds;
	extern volatile long poolReady;

	constexpr int maxWorldId = 16; /* world ids 1..15 */
	constexpr int poolCap = 12; /* reachable ceiling: the walk pool only ever emits 1..12 */
	extern int poolWorldById[maxWorldId];

	extern volatile int randMode;
}
