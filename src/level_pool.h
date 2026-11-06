#pragma once

#include <optional>
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
	struct Level
	{
		std::string name;
		std::string gamePath;
		bool active;
		int slot;
	};

	struct World
	{
		World(std::string name, const std::vector<std::string>& levelNames);

		int UpdateSlots();
		void SetAllLevelsActive(bool active);
		int GetActiveLevelCount() const;
		int GetGameLevelCount() const; // TODO: Rename this

		int type;
		std::string name;
		std::vector<Level> levels;
	};

	void Init();
	void DrawUi();
	const char* WorldTypeToName(int worldType);
	int WorldNameToType(const std::string& name);

	std::optional<std::reference_wrapper<const World>> GetWorldByType(int worldType);

	extern volatile long poolReady;
	extern volatile int randMode;

	// TODO: Move these to a Game Constants header / namespace
	// TODO: Refactor world type to enum
	// TODO: Add Min World Type as well
	constexpr int maxWorldType = 16; // world types 1..15
	constexpr int poolCap = 12; // reachable ceiling: the walk pool only ever emits 1..12
}
