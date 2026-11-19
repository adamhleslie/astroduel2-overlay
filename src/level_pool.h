#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ad2.h"

/* versus level pool - per-world level loading and pool customization

 * Built at startup: enumerates every subdirectory of <resources>/levels/versus/<world>
 * and for each world directory caches info about *.tmx in it. Each *.tmx is treated as a
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
		int GetLevelCountForGame() const;

		ad2::WorldType type;
		std::string name;
		std::vector<Level> levels;
	};

	enum class RandMode
	{
		Off,
		WorldActive,
		World,
		AnyActive,
		Any
	};

	void Init();
	void DrawUi();

	bool IsInitialized();
	RandMode GetRandMode();

	std::optional<std::reference_wrapper<const World>> GetWorldByType(ad2::WorldType type);
	std::optional<std::reference_wrapper<const World>> GetWorldByType(std::underlying_type_t<ad2::WorldType> underlyingType);
}
