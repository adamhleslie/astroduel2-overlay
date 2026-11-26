#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ad2.h"

/* versus level pool - per-world level loading and pool customization
 * When initialized, enumerates every subdirectory of <resources>/levels/versus/<world>
 * and for each world caches info about every *.tmx in it.
 * Each *.tmx is treated as a unique level that can be set active or inactive for the world.
 * When active, the level becomes associated with a slot, mapping from the default level naming
 * convention for that world, to an actual loaded filename, allowing any map to be played at
 * runtime for any world.
 */
namespace level_pool
{
	struct Level
	{
		std::string name;
		std::string gamePath;
		bool active;
		int slot;
		int defaultSlot;

		bool IsAtDefaultSlot() const { return (defaultSlot > 0) && (slot == defaultSlot); }
	};

	struct World
	{
		World(std::string name, const std::vector<std::string>& levelNames);

		int UpdateSlots();
		void SetLevelsActive(bool active);
		void SetLevelsActive(std::function<bool(const Level&, size_t)> activeCallback);
		void SetLevelsActive(bool active, std::function<bool(const Level&, size_t)> shouldModifyCallback);
		void SetCustomLevelsActive(bool active);
		int GetActiveLevelCount() const;
		int GetLevelCountForGame() const;

		std::optional<std::reference_wrapper<const Level>> GetLevelAtSlot(int slot) const;

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

	std::optional<std::reference_wrapper<const Level>> GetRandomLevel(ad2::WorldType worldType);

	std::optional<std::reference_wrapper<const World>> GetWorld(ad2::WorldType type);
	std::optional<std::reference_wrapper<const World>> GetWorld(std::underlying_type_t<ad2::WorldType> underlyingType);
}
