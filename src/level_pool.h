#pragma once

#include <string>
#include <vector>

/* versus level pool - per-world level loading and pool customization
 *
 * Built once at startup: enumerates every subdirectory of <resources>/levels/versus
 * and for each world directory caches every *.tmx in it. Each *.tmx is treated as a
 * unique level that can be set active or inactive for the world. When
 * active, the level becomes associated with a slot, mapping from the
 * default level naming convention for that world, to that level filename,
 * allowing any map to be played at runtime for any world.
 */
namespace level_pool
{
	void Init();
	void DrawUi();
	int PoolWorldEffectiveCount(int worldType);
	const char* WorldIdToName(int id);
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
