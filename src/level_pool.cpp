#include "level_pool.h"

#include <cassert>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <random>
#include <ranges>
#include <shellapi.h>
#include <enchantum/array.hpp>
#include <imgui.h>

#include "mod.h"
#include "ad2_addresses.h"
#include "extensions_enchantum.h"

namespace
{
	constexpr std::string_view category = "level-pool";

	bool isInitialized = false;
	std::vector<level_pool::World> worlds;
	enchantum::array<ad2::WorldType, size_t> worldIndexByType; // index into worlds, (size_t)-1 is invalid

	// Options in overlay
	size_t selectedWorldIndex = 0;
	level_pool::RandMode randMode = level_pool::RandMode::Off;

	std::mt19937 randEngine{std::random_device{}()};

	bool EqualsCaseInsensitive(const std::string& a, const std::string& b)
	{
		return std::ranges::equal(a, b, [](char c1, char c2)
		{
			return std::tolower(static_cast<unsigned char>(c1)) == std::tolower(static_cast<unsigned char>(c2));
		});
	}

	// Extracts the trailing number of a map filename for the specified world ("mines12.tmx" -> 12), -1 if none
	int GetDefaultLevelNumber(const std::string& worldName, const std::string& filename)
	{
		size_t stemEnd = filename.rfind('.');
		if (stemEnd == std::string::npos)
		{
			stemEnd = filename.size();
		}

		size_t i = stemEnd;
		while ((i > 0) && std::isdigit(filename[i - 1]))
		{
			--i;
		}

		if (i == stemEnd)
		{
			// No number at end of stem
			return -1;
		}

		if (!EqualsCaseInsensitive(worldName, filename.substr(0, i)))
		{
			// Stem before number doesn't match world name
			return -1;
		}

		std::string substr = filename.substr(i, stemEnd - i);
		int value = std::stoi(substr);
		return value;
	}

	// Prioritizes map names in the form of "<worldName><#>.tmx", comparing their #. Otherwise direct string comparison.
	bool MapFilenameComparator(const std::string& worldName, const std::string& a, const std::string& b)
	{
		int na = GetDefaultLevelNumber(worldName, a);
		int nb = GetDefaultLevelNumber(worldName, b);
		if ((na < 0) != (nb < 0)) return na >= 0;
		if (na != nb) return na < nb;
		return a < b;
	}

	// Scan the provided world directory into the pool
	void AddWorld(const std::filesystem::path& worldPath)
	{
		assert(std::filesystem::is_directory(worldPath) && worldPath.has_filename());

		// Collect map files under the world directory
		int cashGrabLevels = 0;
		std::vector<std::string> mapFilenames;
		for (auto& directoryEntry : std::filesystem::directory_iterator(worldPath, std::filesystem::directory_options::skip_permission_denied))
		{
			if (!directoryEntry.is_regular_file())
			{
				continue;
			}

			const std::filesystem::path& filePath = directoryEntry.path();
			if (!EqualsCaseInsensitive(filePath.extension().string(), ".tmx"))
			{
				// Not a ".tmx" file
				continue;
			}

			std::string mapFilename = filePath.filename().string();
			if (mapFilename.starts_with("cg_"))
			{
				++cashGrabLevels;
				continue;
			}

			mapFilenames.emplace_back(mapFilename);
		}

		std::string worldName = worldPath.filename().string();
		std::ranges::sort(mapFilenames, [worldName](const std::string& a, const std::string& b)
		{
			return MapFilenameComparator(worldName, a, b);
		});

		// Construct world with the found maps
		level_pool::World world{worldName, mapFilenames};

		int activeCount = world.GetActiveLevelCount();
		size_t worldIndex = worlds.size();
		worlds.push_back(world);

		// Cache mapping from world type to world index
		if (enchantum::contains(world.type))
		{
			worldIndexByType[world.type] = worldIndex;
		}

		if (world.levels.size() < 2)
		{
			mod::Log(category, "WARNING {}: only {} level(s) found (< 2) - {}",
				world.name, world.levels.size(), activeCount, (activeCount == 1 ? "pool count faked to 2 (single-map mode)" : "crash expected"));
		}

		mod::Log(category, "{}: {} levels in pool ({} cash grab maps skipped){}",
			world.name, world.levels.size(), cashGrabLevels, (!enchantum::contains(world.type)) ? " [worldType unknown, will be unused]" : "");
	}

	void PoolInit()
	{
		if (ad2::resourcesPath.empty())
		{
			mod::Log(category, "Resources Path not defined");
			return;
		}

		std::filesystem::path versusLevelsPath = ad2::resourcesPath.append("levels").append("versus");
		if (!std::filesystem::is_directory(versusLevelsPath))
		{
			mod::Log(category, "Versus Levels Path is not a valid directory {}", versusLevelsPath.string());
			return;
		}

		int worldCount = 0;
		for (auto& directoryEntry : std::filesystem::directory_iterator(versusLevelsPath, std::filesystem::directory_options::skip_permission_denied))
		{
			if (directoryEntry.is_directory())
			{
				AddWorld(directoryEntry.path());
				++worldCount;
			}
		}

		mod::Log(category, "{} worlds found under {}", worldCount, versusLevelsPath.string());
	}

	void SetAllLevelsActive(bool active)
	{
		for (level_pool::World& world : worlds)
		{
			world.SetLevelsActive(active);
		}

		mod::Log(category, "{} all worlds -- count={} applied to every world", active ? "RESET" : "CLEAR", active);
	}

	void SetCustomLevelsActive(bool active)
	{
		for (level_pool::World& world : worlds)
		{
			world.SetCustomLevelsActive(active);
		}

		mod::Log(category, "{} all worlds -- count={} applied to every world", active ? "RESET" : "CLEAR", active);
	}
}

#pragma region World

level_pool::World::World(std::string name, const std::vector<std::string>& levelNames)
	: type{ext::enchantum::cast_lower_or(name, ad2::WorldType::Invalid)}
	, name{std::move(name)}
	, levels{levelNames.size()}
{
	for (const auto [i, levelName] : levelNames | std::ranges::views::enumerate)
	{
		levels[i] = Level
		{
			.name = levelName,
			.gamePath = std::format("levels/versus/{}/{}", this->name, levelName),
			.active = true,
			.defaultSlot = GetDefaultLevelNumber(this->name, levelName)
		};
	}

	UpdateSlots();
}

// Updates level slots within the provided world based on their active state
// Returns the number of slots taken up, which is equivalent to the number of active levels
int level_pool::World::UpdateSlots()
{
	int slotCount = 0;
	for (Level& level: levels)
	{
		level.slot = (level.active ? ++slotCount : 0);
	}

	return slotCount;
}

void level_pool::World::SetLevelsActive(bool active)
{
	for (Level& level : levels)
	{
		level.active = active;
	}

	UpdateSlots();
	mod::Log(category, "{} set all levels {}", name, active ? "active" : "inactive");
}

void level_pool::World::SetLevelsActive(std::function<bool(const Level&, size_t)> activeCallback)
{
	int activeCount = 0;
	int inactiveCount = 0;
	for (const auto [i, level] : levels | std::ranges::views::enumerate)
	{
		level.active = activeCallback(level, i);
		if (level.active)
		{
			++activeCount;
		}
		else
		{
			++inactiveCount;
		}
	}

	UpdateSlots();
	mod::Log(category, "{} set {} levels active, {} levels inactive", name, activeCount, inactiveCount);
}

void level_pool::World::SetLevelsActive(bool active, std::function<bool(const Level&, size_t)> shouldModifyCallback)
{
	int modifiedCount = 0;
	for (const auto [i, level] : levels | std::ranges::views::enumerate)
	{
		if (shouldModifyCallback(level, i))
		{
			level.active = active;
			++modifiedCount;
		}
	}

	UpdateSlots();
	mod::Log(category, "{} set {} levels {}", name, modifiedCount, active ? "active" : "inactive");
}

void level_pool::World::SetCustomLevelsActive(bool active)
{
	SetLevelsActive(active, [](const Level& level, size_t) { return level.defaultSlot == -1; });
}

int level_pool::World::GetActiveLevelCount() const
{
	int n = 0;
	for (const Level& level : levels)
	{
		if (level.active)
		{
			n++;
		}
	}

	return n;
}

// the count the game actually sees: a single-map world is faked to 2, handled in GetLevelRemap
int level_pool::World::GetLevelCountForGame() const
{
	int activeLevelCount = GetActiveLevelCount();
	return (activeLevelCount == 1) ? 2 : activeLevelCount;
}

std::optional<std::reference_wrapper<const level_pool::Level>> level_pool::World::GetLevelAtSlot(int slot) const
{
	int count = 0;
	long long firstSlotIndex = -1;
	for (const auto [i, level] : levels | std::ranges::views::enumerate)
	{
		if (level.slot == slot)
		{
			return level;
		}

		if (level.slot > 0)
		{
			++count;
			if (level.slot == 1)
			{
				assert(firstSlotIndex == -1);
				firstSlotIndex = i;
			}
		}
	}

	if (count == 1)
	{
		assert(firstSlotIndex != -1);
		return levels[firstSlotIndex];
	}

	return std::nullopt;
}

#pragma endregion

void level_pool::DrawUi()
{
	ImGui::Text("VERSUS LEVEL POOL");
	if (!isInitialized || worlds.empty())
	{
		ImGui::TextDisabled("no worlds loaded");
	}
	else
	{
		if ((selectedWorldIndex < 0) || (selectedWorldIndex >= worlds.size()))
		{
			selectedWorldIndex = 0;
		}
		World& selectedWorld = worlds[selectedWorldIndex];

		// Global Actions
		if (ImGui::Button("Open res folder"))
		{
			std::string resourcesPathString = ad2::resourcesPath.string();
			ShellExecuteA(nullptr, "open", resourcesPathString.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			mod::Log(category, "Opened res folder {}", resourcesPathString);
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset All Worlds"))
		{
			SetAllLevelsActive(true);
		}
		ImGui::SameLine();
		if (ImGui::Button("Select All Custom"))
		{
			SetCustomLevelsActive(true);
		}
		ImGui::SameLine();
		if (ImGui::Button("Clear All Worlds"))
		{
			SetAllLevelsActive(false);
		}
		ImGui::Spacing();

		// Custom randomized map selection (Pilot Hunters / Team Annihilation only)
		int randModeSelection = static_cast<std::underlying_type_t<RandMode>>(randMode);
		auto& names = ext::enchantum::names_c_str<RandMode>;
		if (ImGui::Combo("Random map (PH/TA only)", &randModeSelection, names.data(), static_cast<int>(names.size())))
		{
			randMode = ext::enchantum::cast_or(randModeSelection, RandMode::Off);
		}
		if ((randModeSelection == 1) && (selectedWorld.GetActiveLevelCount() == 0))
		{
			ImGui::TextDisabled("WorldActive pool empty (0 maps) -- random falls through");
		}

		// World selection dropdown
		if (ImGui::BeginCombo("##world", selectedWorld.name.c_str(), ImGuiComboFlags_HeightLargest))
		{
			for (const auto [i, world] : worlds | std::ranges::views::enumerate)
			{
				if (ImGui::Selectable(world.name.c_str(), selectedWorldIndex == static_cast<size_t>(i)))
				{
					selectedWorldIndex = i;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::Spacing();

		ImGui::BeginChild("##files", ImVec2{0, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened);
		for (const auto [i, level] : selectedWorld.levels | std::ranges::views::enumerate)
		{
			if (ImGui::Checkbox(level.name.c_str(), &level.active))
			{
				selectedWorld.UpdateSlots();
			}
			if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonRight))
			{
				if (ImGui::Button("Select Exclusive"))
				{
					selectedWorld.SetLevelsActive(std::function{[i](const Level&, size_t index) { return index == i;}});
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
			ImGui::SameLine();
			ImGui::TextDisabled("%s", level.gamePath.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled((level.active ? "[slot %d]" : "[off]"), level.slot);
		}
		ImGui::EndChild();

		int activeCount = selectedWorld.GetActiveLevelCount();
		std::string context;
		if (activeCount == 0)
		{
			context = "(0 maps - crash expected)";
		}
		else if (activeCount == 1)
		{
			context = "(1 map -> faked to 2)";
		}
		else if (activeCount > ad2::levelPoolCap)
		{
			context = std::format("(only the first {} slots will be used in Pilot Hunters or Team Annihilation)", ad2::levelPoolCap);
		}
		ImGui::Text("active count: %d %s", activeCount, context.c_str());

		if (ImGui::Button("Select All"))
		{
			selectedWorld.SetLevelsActive(true);
		}
		ImGui::SameLine();
		if (ImGui::Button("Select Custom"))
		{
			selectedWorld.SetCustomLevelsActive(true);
		}
		ImGui::SameLine();
		if (ImGui::Button("Clear Selection"))
		{
			selectedWorld.SetLevelsActive(false);
		}
	}
}

void level_pool::Init()
{
	assert(!isInitialized);

	// Default initialize worldIndexByType to invalid value (size_t)-1
	worldIndexByType.fill(static_cast<size_t>(-1));

	PoolInit();
	isInitialized = true;
}

bool level_pool::IsInitialized() { return isInitialized; }

std::optional<std::reference_wrapper<const level_pool::Level>> level_pool::GetRandomLevel(ad2::WorldType worldType)
{
	if (randMode == RandMode::Off)
	{
		return std::nullopt;
	}

	std::vector<std::reference_wrapper<const Level>> levels;
	auto addLevels = [&levels](std::optional<std::reference_wrapper<const World>> worldOpt, bool activeOnly)
	{
		if (worldOpt)
		{
			const World& world = *worldOpt;
			for (const Level& level : world.levels)
			{
				if (level.active || !activeOnly)
				{
					levels.emplace_back(level);
				}
			}
		}
	};

	switch (randMode)
	{
		case RandMode::WorldActive:
		{
			addLevels(GetWorld(worldType), true);
			break;
		}
		case RandMode::World:
		{
			addLevels(GetWorld(worldType), false);
			break;
		}
		case RandMode::AnyActive:
		{
			for (ad2::WorldType worldTypeCustom : enchantum::values_generator<ad2::WorldType>)
			{
				addLevels(GetWorld(worldTypeCustom), true);
			}
			break;
		}
		case RandMode::Any:
		{
			for (ad2::WorldType worldTypeCustom : enchantum::values_generator<ad2::WorldType>)
			{
				addLevels(GetWorld(worldTypeCustom), false);
			}
			break;
		}
	}

	if (levels.empty())
	{
		return std::nullopt;
	}

	std::uniform_int_distribution randDistribution{1, static_cast<int>(levels.size())};
	return levels[randDistribution(randEngine)];
}

std::optional<std::reference_wrapper<const level_pool::World>> level_pool::GetWorld(ad2::WorldType type)
{
	if (enchantum::contains(type))
	{
		size_t index = worldIndexByType[type];
		if (index != static_cast<size_t>(-1))
		{
			return worlds[index];
		}
	}

	return std::nullopt;
}

std::optional<std::reference_wrapper<const level_pool::World>> level_pool::GetWorld(std::underlying_type_t<ad2::WorldType> underlyingType)
{
	if (auto typeOpt = enchantum::cast<ad2::WorldType>(underlyingType))
	{
		return GetWorld(*typeOpt);
	}

	return std::nullopt;
}
