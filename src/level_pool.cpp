#include "level_pool.h"

#include "mod.h"
#include "imgui.h"

#include <cassert>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <shellapi.h>

namespace
{
	constexpr std::string module = "level-pool";

	/* ------------------------------------------------------------------ */
	/* versus level cache -- per-world .tmx inventory + the count source  */
	/*                                                                     */
	/* Built once at startup: enumerate every subdirectory of             */
	/* <gamedir>/res/levels/versus (the overlay DLL sits in the game dir  */
	/* as steaapi64.dll, so its module path is the base) and, for each  */
	/* world dir, every *.tmx in it. One cache object per world; each     */
	/* file entry carries:                                                */
	/*   name   -- the file's basename                                    */
	/*   active -- in the live pool; default = every scanned file on;    */
	/*             "cg_" (Cash Grab) files are skipped entirely at scan  */
	/*             time and never enter the pool                         */
	/*   slot   -- the preload slot the file will be pre-loaded to;       */
	/*             active files take slots 1..K in sort order, inactive   */
	/*             files hold no slot (0)                                 */
	/* Sort is natural: numbered files by ascending trailing number,      */
	/* then non-numbered files alphabetically (cg_* files never reach    */
	/* the sort -- they are dropped at scan time).                        */
	/* The world's count is K = number of active files (Apply is blocked  */
	/* only at 0); slots above POOL_CAP are harmless but unreachable --   */
	/* the walk's pool tops out at 12. The game                          */
	/* preloads slots 1..K contiguously, so this table doubles as the glv */
	/* remap (num -> file), enforced in GetLevelRemap. A world with no    */
	/* cache entry (or no active files) runs stock.                       */
	/* for active-flag persistence (a future UI step) -- not read here.   */
	/* ------------------------------------------------------------------ */

	// Updates level slots within the provided world based on their active state
	// Returns the number of slots taken up, which is equivalent to the number of active levels
	int AssignSlots(level_pool::World& world)
	{
		int slotCount = 0;
		for (level_pool::Level& level: world.levels)
		{
			level.slot = level.active ? ++slotCount : 0;
		}

		return slotCount;
	}

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
		std::vector<std::string> mapFiles;
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

			mapFiles.emplace_back(mapFilename);
		}

		std::string worldName = worldPath.filename().string();
		std::ranges::sort(mapFiles, [worldName](const std::string& a, const std::string& b)
		{
			return MapFilenameComparator(worldName, a, b);
		});

		// Construct world with the found maps
		level_pool::World world
		{
			.id = level_pool::WorldNameToId(worldName),
			.name = worldName
		};
		for (const std::string& mapFile : mapFiles)
		{
			world.levels.emplace_back(level_pool::Level
			{
				.name = mapFile,
				.gamePath = std::format("levels/versus/{}/{}", world.name, mapFile),
			});
		}

		int slotCount = AssignSlots(world);
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
				world.name, world.levels.size(), slotCount, slotCount == 1 ? "pool count faked to 2 (single-map mode)" : "crash expected");
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

	/* ------------------------------------------------------------------ */
	/* pool editor draft + apply (UI)                                      */
	/*                                                                     */
	/* The UI edits a DRAFT copy of the active flags (one vector per world,*/
	/* parallel to worlds). Apply copies the selected world's draft*/
	/* into the live cache and recompacts its slots. PoolActiveCount (and */
	/* and the glv remap) read the LIVE cache, so the count only moves    */
	/* on Apply. Threading: the draft + live cache are touched by the      */
	/* render thread (UI) only after poolReady; the game thread reads   */
	/* the live cache via PoolActiveCount (minor race, single-player local*/
	/* mod -- the UI is used at menus, not during level loads).            */
	/* ------------------------------------------------------------------ */
	int poolSelectedWorld = 0; /* index into worlds */
	std::vector<std::vector<char> > poolDraft; /* draft active flags (0/1) */

	/* draft = current live flags; once, after PoolInit */
	void PoolDraftInit()
	{
		poolDraft.resize(level_pool::worlds.size());
		for (size_t i = 0; i < level_pool::worlds.size(); i++)
		{
			poolDraft[i].resize(level_pool::worlds[i].levels.size());
			for (size_t j = 0; j < level_pool::worlds[i].levels.size(); j++)
				poolDraft[i][j] = level_pool::worlds[i].levels[j].active ? 1 : 0;
		}
		InterlockedExchange(&level_pool::poolReady, 1);
	}

	/* copy the selected world's draft into the live cache + recompact slots */
	void PoolApplyWorld(int i)
	{
		if (i < 0 || i >= (int) level_pool::worlds.size()) return;
		level_pool::World& w = level_pool::worlds[i];
		int changed = 0;
		for (size_t j = 0; j < w.levels.size() && j < poolDraft[i].size(); j++)
		{
			bool a = poolDraft[i][j] != 0;
			if (a != w.levels[j].active)
			{
				w.levels[j].active = a;
				changed++;
			}
		}
		int count = AssignSlots(w);
		mod::Log(module, "APPLY {} -- count={} ({} flag change(s))", w.name.c_str(), count, changed);
	}

	/* set every world's draft to a value (1=select all, 0=clear all) and apply
	 * all worlds into the live cache in one shot. */
	void PoolApplyAll(int value)
	{
		for (size_t i = 0; i < level_pool::worlds.size() && i < poolDraft.size(); i++)
		{
			for (char& c: poolDraft[i]) c = (char) value;
			PoolApplyWorld((int) i); /* copies draft -> live + recompacts slots */
		}
		mod::Log(module, "{} all worlds -- count={} applied to every world",
		         value ? "RESET" : "CLEAR", value);
	}

	/* count of active files in a worldType's cache entry; 0 = no entry (stock) */
	int PoolActiveCount(int worldType)
	{
		if (worldType < 1 || worldType >= level_pool::maxWorldId) return 0;
		int idx = level_pool::poolWorldById[worldType];
		if (idx < 0) return 0;
		int n = 0;
		for (const level_pool::Level& f: level_pool::worlds[idx].levels)
			if (f.active) n++;
		return n;
	}
}

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

void level_pool::DrawUi()
{
	ImGui::Text("VERSUS LEVEL POOL");
	if (!poolReady || worlds.empty())
	{
		ImGui::TextDisabled("no worlds cached (res/levels/versus missing?)");
	}
	else
	{
		if (poolSelectedWorld < 0 || poolSelectedWorld >= (int) worlds.size())
			poolSelectedWorld = 0;
		const World& sel = worlds[poolSelectedWorld];
		if (ImGui::BeginCombo("##world", sel.name.c_str()))
		{
			for (int i = 0; i < (int) worlds.size(); i++)
			{
				bool s = ImGui::Selectable(worlds[i].name.c_str(), poolSelectedWorld == i);
				if (s) poolSelectedWorld = i;
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
			PoolApplyAll(1);
		ImGui::SameLine();
		if (ImGui::Button("Clear All Worlds"))
			PoolApplyAll(0);
		ImGui::Spacing();
		ImGui::BeginChild("##files", ImVec2(0, 300), true);
		std::vector<char>& draft = poolDraft[poolSelectedWorld];
		for (size_t j = 0; j < sel.levels.size() && j < draft.size(); j++)
		{
			bool b = draft[j] != 0;
			if (ImGui::Checkbox(sel.levels[j].name.c_str(), &b))
				draft[j] = b ? 1 : 0;
			ImGui::SameLine();
			ImGui::TextDisabled("%s", sel.levels[j].gamePath.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled(sel.levels[j].active ? "[slot %d]" : "[off]", sel.levels[j].slot);
		}
		ImGui::EndChild();
		int act = 0;
		for (char c: draft) if (c) act++;
		int liveRaw = PoolActiveCount(sel.id);
		int live = PoolWorldEffectiveCount(sel.id);
		char hint[112] = "";
		if (liveRaw == 0) snprintf(hint, sizeof hint, "  (0 maps -- crash expected, below walk minimum)");
		else if (liveRaw == 1) snprintf(hint, sizeof hint, "  (1 map -> faked to 2)");
		else if (liveRaw > poolCap)
			snprintf(hint, sizeof hint,
			         "  (only the first %d slots will be used in Pilot Hunters or Team Annihilation)",
			         poolCap);
		ImGui::Text("draft count: %d    live count: %d%s", act, live, hint);
		if (ImGui::Button(("Apply " + sel.name).c_str()))
			PoolApplyWorld(poolSelectedWorld);
		ImGui::SameLine();
		if (ImGui::Button("Select All"))
			for (char& c: draft) c = 1;
		ImGui::SameLine();
		if (ImGui::Button("Clear Selection"))
			for (char& c: draft) c = 0;

		/* random-map selection (PH/TA loads only; deathmatch / cash grab /
		   deathmatch preview are excluded in GetLevelRemap) */
		const char* kRandModes[] = {"Off", "WorldActive", "World", "AnyActive", "Any"};
		int randSel = 0;
		if (ImGui::Combo("Random map (PH/TA only)", &randSel, kRandModes, 5))
			randMode = randSel;
		if (randMode == 1 && live == 0)
			ImGui::TextDisabled("WorldActive pool empty (0 maps) -- random falls through");
	}
}

void level_pool::Init()
{
	PoolInit();
	PoolDraftInit();
}

/* the count the game actually sees: a single-map world is faked to 2,
   because the walk's exhaust path is an assert (K must be >= 2). Both
   nums (1 and 2) resolve to the lone active file in GetLevelRemap. */
int level_pool::PoolWorldEffectiveCount(int worldType)
{
	int n = PoolActiveCount(worldType);
	return (n == 1) ? 2 : n;
}

/* ------------------------------------------------------------------ */
/* world names (log labels)                                            */
/* ------------------------------------------------------------------ */

const char* level_pool::IdToWorldName(int id)
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
		if (strcmp(IdToWorldName(i), name.c_str()) == 0)
		{
			return i;
		}
	}

	return 0;
}
