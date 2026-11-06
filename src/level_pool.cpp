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

volatile long level_pool::poolReady = 0; /* cache + draft built */
/* Random-map selection (render thread writes on combo change; game thread
 * reads in PoolRandPick -- same minor-race pattern as the active flags).
 * 0=Off, 1=WorldActive, 2=World, 3=AnyActive, 4=Any. Only ever reaches
 * PH/TA walk loads: deathmatch (pick=2), cash grab (pick=3) and the
 * deathmatch UI preview (mode=1) are all excluded in GetLevelRemap. */
volatile int level_pool::randMode = 0;

namespace
{
	constexpr std::string module = "level-pool";
	size_t selectedWorldIndex = 0;

	// extern globals
	std::vector<level_pool::World> worlds;
	std::array<int, level_pool::maxWorldType> worldIndexByType; /* index into worlds, -1 = none */

	bool EqualsCaseInsensitive(const std::string& a, const std::string& b)
	{
		return std::ranges::equal(a, b,
			[](char c1, char c2)
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
		if ((world.type >= 1) && (world.type < level_pool::maxWorldType))
		{
			worldIndexByType[world.type] = static_cast<int>(worldIndex);
		}

		if (world.levels.size() < 2)
		{
			mod::Log(module, "WARNING {}: only {} level(s) found (< 2) - {}",
				world.name, world.levels.size(), activeCount, activeCount == 1 ? "pool count faked to 2 (single-map mode)" : "crash expected");
		}

		mod::Log(module, "{}: {} levels in pool ({} cash grab maps skipped){}",
			world.name, world.levels.size(), cashGrabLevels, (world.type < 1) ? " [worldType unknown, not applied]" : "");
	}

	void PoolInit()
	{
		if (mod::resourcesPath.empty())
		{
			mod::Log(module, "Resources Path not defined");
			return;
		}

		std::filesystem::path versusLevelsPath = mod::resourcesPath.append("levels").append("versus");
		if (!std::filesystem::is_directory(versusLevelsPath))
		{
			mod::Log(module, "Versus Levels Path is not a valid directory {}", versusLevelsPath.string());
			return;
		}

		int worlds = 0;
		for (auto& directoryEntry : std::filesystem::directory_iterator(versusLevelsPath, std::filesystem::directory_options::skip_permission_denied))
		{
			if (directoryEntry.is_directory())
			{
				AddWorld(directoryEntry.path());
				++worlds;
			}
		}

		mod::Log(module, "{} worlds found under {}", worlds, versusLevelsPath.string());
	}

	// Set every level to an active state and applies in one shot
	void SetAllLevelsActive(bool active)
	{
		for (level_pool::World& world : worlds)
		{
			world.SetAllLevelsActive(active);
		}

		mod::Log(module, "{} all worlds -- count={} applied to every world", active ? "RESET" : "CLEAR", active);
	}
}

#pragma region World

level_pool::World::World(std::string name, const std::vector<std::string>& levelNames)
	: type{WorldNameToType(name)}
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
	mod::Log(module, "{} set all levels {}", name, active ? "active" : "inactive");
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

/* the count the game actually sees: a single-map world is faked to 2,
   because the walk's exhaust path is an assert (K must be >= 2). Both
   nums (1 and 2) resolve to the lone active file in GetLevelRemap. */
int level_pool::World::GetGameLevelCount() const
{
	int activeLevelCount = GetActiveLevelCount();
	return (activeLevelCount == 1) ? 2 : activeLevelCount;
}

#pragma endregion

void level_pool::DrawUi()
{
	ImGui::Text("VERSUS LEVEL POOL");
	if (!poolReady || worlds.empty())
	{
		ImGui::TextDisabled("no worlds cached (res/levels/versus missing?)");
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
			std::string resourcesPathString = mod::resourcesPath.string();
			ShellExecuteA(nullptr, "open", resourcesPathString.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			mod::Log(module, "Opened res folder {}", resourcesPathString);
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
		// TODO: Change to enum (+ use magic_enum library?)
		constexpr std::array<const char*, 5> randModes{ "Off", "WorldActive", "World", "AnyActive", "Any" };
		int randSelection = randMode;
		if (ImGui::Combo("Random map (PH/TA only)", &randSelection, randModes.data(), randModes.size()))
		{
			randMode = randSelection;
		}
		if ((randMode == 1) && (selectedWorld.GetActiveLevelCount() == 0))
		{
			ImGui::TextDisabled("WorldActive pool empty (0 maps) -- random falls through");
		}

		// World selection dropdown
		if (ImGui::BeginCombo("##world", selectedWorld.name.c_str()))
		{
			for (const auto [i, world] : worlds | std::ranges::views::enumerate)
			{
				if (ImGui::Selectable(world.name.c_str(), selectedWorldIndex == i))
				{
					selectedWorldIndex = i;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::Spacing();

		ImGui::BeginChild("##files", ImVec2{0, 0}, ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened);
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
			context = "(0 maps -- crash expected, below walk minimum)";
		}
		else if (activeCount == 1)
		{
			context = "(1 map -> faked to 2)";
		}
		else if (activeCount > poolCap)
		{
			context = std::format("(only the first {} slots will be used in Pilot Hunters or Team Annihilation)", poolCap);
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
	// Default initialize worldIndexByType to invalid value -1
	worldIndexByType.fill(-1);

	PoolInit();

	InterlockedExchange(&poolReady, 1); //: Refactor to use stl
}

const char* level_pool::WorldTypeToName(int worldType)
{
	const char* names[maxWorldType] = {
		"", "mines", "frontier", "flare", "darklab", "echocity", "highrise",
		"sunken", "graveyard", "gasrigs", "moonbase", "urspace", "void",
		"legacy", "up", "unsorted"
	};
	return ((worldType >= 1) && (worldType < maxWorldType)) ? names[worldType] : "?";
}

int level_pool::WorldNameToType(const std::string& name)
{
	for (int i = 1; i < maxWorldType; ++i)
	{
		if (strcmp(WorldTypeToName(i), name.c_str()) == 0)
		{
			return i;
		}
	}

	return 0;
}

std::optional<std::reference_wrapper<const level_pool::World>> level_pool::GetWorldByType(int worldType)
{
	if ((worldType < 1) || worldType >= maxWorldType)
	{
		return std::nullopt;
	}

	int worldIndex = worldIndexByType[worldType];
	if (worldIndex < 0)
	{
		return std::nullopt;
	}

	return worlds[worldIndex];
}