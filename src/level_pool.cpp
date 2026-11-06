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

// extern globals
std::vector<level_pool::World> level_pool::worlds;
int level_pool::poolWorldById[] = {-1}; /* index into worlds, -1 = none */
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
	int selectedWorldIndex = 0;

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
		size_t worldIndex = level_pool::worlds.size();
		level_pool::worlds.push_back(world);

		// Cache mapping from world id to world index
		if ((world.id >= 1) && (world.id < level_pool::maxWorldId))
		{
			level_pool::poolWorldById[world.id] = static_cast<int>(worldIndex);
		}

		if (world.levels.size() < 2)
		{
			mod::Log(module, "WARNING {}: only {} level(s) found (< 2) - {}",
				world.name, world.levels.size(), activeCount, activeCount == 1 ? "pool count faked to 2 (single-map mode)" : "crash expected");
		}

		mod::Log(module, "{}: {} levels in pool ({} cash grab maps skipped){}",
			world.name, world.levels.size(), cashGrabLevels, (world.id < 1) ? " [worldType unknown, not applied]" : "");
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
		for (level_pool::World& world : level_pool::worlds)
		{
			world.SetAllLevelsActive(active);
		}

		mod::Log(module, "{} all worlds -- count={} applied to every world", active ? "RESET" : "CLEAR", active);
	}

	std::optional<std::reference_wrapper<level_pool::World>> GetWorldForType(int worldType)
	{
		if ((worldType < 1) || worldType >= level_pool::maxWorldId)
		{
			return std::nullopt;
		}

		int worldIndex = level_pool::poolWorldById[worldType];
		if (worldIndex < 0)
		{
			return std::nullopt;
		}

		return level_pool::worlds[worldIndex];
	}
}

#pragma region World

level_pool::World::World(std::string name, const std::vector<std::string>& levelNames)
	: id{WorldNameToId(name)}
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
		if (selectedWorldIndex < 0 || selectedWorldIndex >= (int) worlds.size())
			selectedWorldIndex = 0;
		World& sel = worlds[selectedWorldIndex];
		if (ImGui::BeginCombo("##world", sel.name.c_str()))
		{
			for (int i = 0; i < (int) worlds.size(); i++)
			{
				bool s = ImGui::Selectable(worlds[i].name.c_str(), selectedWorldIndex == i);
				if (s) selectedWorldIndex = i;
			}
			ImGui::EndCombo();
		}
		ImGui::Spacing();
		/* global actions (all worlds) */
		if (ImGui::Button("Open res folder"))
		{
			std::string resourcesPathString = mod::resourcesPath.string();
			ShellExecuteA(NULL, "open", resourcesPathString.c_str(), NULL, NULL, SW_SHOWNORMAL);
			mod::Log(module, "opened res folder {}", resourcesPathString);
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset All Worlds"))
			SetAllLevelsActive(true);
		ImGui::SameLine();
		if (ImGui::Button("Clear All Worlds"))
			SetAllLevelsActive(false);
		ImGui::Spacing();
		ImGui::BeginChild("##files", ImVec2(0, 300), true);
		for (size_t j = 0; j < sel.levels.size(); j++)
		{
			if (ImGui::Checkbox(sel.levels[j].name.c_str(), &sel.levels[j].active))
			{
				sel.UpdateSlots();
			}
			ImGui::SameLine();
			ImGui::TextDisabled("%s", sel.levels[j].gamePath.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled(sel.levels[j].active ? "[slot %d]" : "[off]", sel.levels[j].slot);
		}
		ImGui::EndChild();
		int activeCountRaw = sel.GetActiveLevelCount();
		int activeCount = sel.GetActiveLevelCount();
		char hint[112] = "";
		if (activeCountRaw == 0) snprintf(hint, sizeof hint, "  (0 maps -- crash expected, below walk minimum)");
		else if (activeCountRaw == 1) snprintf(hint, sizeof hint, "  (1 map -> faked to 2)");
		else if (activeCountRaw > poolCap) snprintf(hint, sizeof hint, " (only the first %d slots will be used in Pilot Hunters or Team Annihilation)", poolCap);
		ImGui::Text("active count: %d%s", activeCount, hint);
		if (ImGui::Button("Select All"))
		{
			sel.SetAllLevelsActive(true);
		}
		ImGui::SameLine();
		if (ImGui::Button("Clear Selection"))
		{
			sel.SetAllLevelsActive(false);
		}

		/* random-map selection (PH/TA loads only; deathmatch / cash grab /
		   deathmatch preview are excluded in GetLevelRemap) */
		const char* kRandModes[] = {"Off", "WorldActive", "World", "AnyActive", "Any"};
		int randSel = randMode;
		if (ImGui::Combo("Random map (PH/TA only)", &randSel, kRandModes, 5))
			randMode = randSel;
		if (randMode == 1 && activeCount == 0)
			ImGui::TextDisabled("WorldActive pool empty (0 maps) -- random falls through");
	}
}

void level_pool::Init()
{
	PoolInit();
	InterlockedExchange(&poolReady, 1); // TODO: Refactor to use stl
}

/* the count the game actually sees: a single-map world is faked to 2,
   because the walk's exhaust path is an assert (K must be >= 2). Both
   nums (1 and 2) resolve to the lone active file in GetLevelRemap. */
int level_pool::GetActiveLevelCountForType(int worldType)
{
	if (auto worldOption = GetWorldForType(worldType))
	{
		World& world = *worldOption;
		int activeLevelCount = world.GetActiveLevelCount();
		return (activeLevelCount == 1) ? 2 : activeLevelCount;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* world names (log labels)                                            */
/* ------------------------------------------------------------------ */

const char* level_pool::WorldIdToName(int id)
{
	const char* names[maxWorldId] = {
		"", "mines", "frontier", "flare", "darklab", "echocity", "highrise",
		"sunken", "graveyard", "gasrigs", "moonbase", "urspace", "void",
		"legacy", "up", "unsorted"
	};
	return (id >= 1 && id < maxWorldId) ? names[id] : "?";
}

int level_pool::WorldNameToId(const std::string& name)
{
	for (int i = 1; i < maxWorldId; i++)
	{
		if (strcmp(WorldIdToName(i), name.c_str()) == 0)
		{
			return i;
		}
	}

	return 0;
}
