#include "level_pool.h"

#include "mod.h"
#include "imgui.h"

#include <cassert>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <ranges>
#include <shellapi.h>
#include <enchantum/array.hpp>

#include "extensions_enchantum.h"

namespace
{
	constexpr std::string_view category = "level-pool";
	std::vector<level_pool::World> worlds;
	enchantum::array<ad2::WorldType, size_t> worldIndexByType; // index into worlds, (size_t)-1 is invalid
	size_t selectedWorldIndex = 0;
	bool isInitialized = false;
	level_pool::RandMode randMode = level_pool::RandMode::Off;

	bool EqualsCaseInsensitive(const std::string& a, const std::string& b)
	{
		return std::ranges::equal(a, b, [](char c1, char c2)
		{
			return std::tolower(static_cast<unsigned char>(c1)) == std::tolower(static_cast<unsigned char>(c2));
		});
	}

	// Extracts the trailing number of a map filename for the specified world ("mines12.tmx" -> 12), -1 if none
	int ExtractTmxNumber(const std::string& worldName, const std::string& filename)
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
		int na = ExtractTmxNumber(worldName, a);
		int nb = ExtractTmxNumber(worldName, b);
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

	// Set every level to an active state and applies in one shot
	void SetAllLevelsActive(bool active)
	{
		for (level_pool::World& world : worlds)
		{
			world.SetAllLevelsActive(active);
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
			.gamePath = std::format("levels/versus/{}/{}", name, levelName),
			.active = true,
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

void level_pool::World::SetAllLevelsActive(bool active)
{
	for (Level& level : levels)
	{
		level.active = active;
	}

	UpdateSlots();
	mod::Log(category, "{} set all levels {}", name, active ? "active" : "inactive");
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
		if (ImGui::BeginCombo("##world", selectedWorld.name.c_str()))
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
		for (Level& level : selectedWorld.levels)
		{
			if (ImGui::Checkbox(level.name.c_str(), &level.active))
			{
				selectedWorld.UpdateSlots();
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
			selectedWorld.SetAllLevelsActive(true);
		}
		ImGui::SameLine();
		if (ImGui::Button("Clear Selection"))
		{
			selectedWorld.SetAllLevelsActive(false);
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
level_pool::RandMode level_pool::GetRandMode() { return randMode; }

std::optional<std::reference_wrapper<const level_pool::World>> level_pool::GetWorldByType(ad2::WorldType type)
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

std::optional<std::reference_wrapper<const level_pool::World>> level_pool::GetWorldByType(std::underlying_type_t<ad2::WorldType> underlyingType)
{
	if (auto typeOpt = enchantum::cast<ad2::WorldType>(underlyingType))
	{
		return GetWorldByType(*typeOpt);
	}

	return std::nullopt;
}