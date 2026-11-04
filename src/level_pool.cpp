#include "level_pool.h"

#include "mod.h"

#include <string>
#include <vector>
#include <algorithm>
#include <shellapi.h>

#include "imgui.h"

namespace
{
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
	char resDir[MAX_PATH] = {0}; /* <gamedir>\res -- explorer button target (set in PoolCacheBuild) */

	/* trailing number of a tmx name ("mines12.tmx" -> 12); -1 when none */
	int PoolTmxNumber(const std::string& n)
	{
		std::string b = n;
		size_t dot = b.rfind('.');
		if (dot != std::string::npos) b.erase(dot);
		size_t i = b.size();
		while (i > 0 && b[i - 1] >= '0' && b[i - 1] <= '9') i--;
		if (i == b.size()) return -1;
		int num = 0;
		for (size_t k = i; k < b.size(); k++) num = num * 10 + (b[k] - '0');
		return num;
	}

	/* natural order: numbered by ascending number, non-numbered last */
	bool PoolTmxBefore(const std::string& a, const std::string& b)
	{
		int na = PoolTmxNumber(a), nb = PoolTmxNumber(b);
		if ((na < 0) != (nb < 0)) return na >= 0;
		if (na != nb) return na < nb;
		return a < b;
	}

	/* compact slots 1..K over the active files (already sort-ordered); returns K */
	int PoolWorldAssignSlots(level_pool::PoolWorld& w)
	{
		int slot = 0;
		for (level_pool::PoolFile& pf: w.files)
			pf.slot = pf.active ? ++slot : 0; /* inactive must lose any stale slot */
		return slot;
	}

	/* count of active files in a worldType's cache entry; 0 = no entry (stock) */
	int PoolActiveCount(int worldType)
	{
		if (worldType < 1 || worldType >= level_pool::poolWorldMax) return 0;
		int idx = level_pool::poolWorldById[worldType];
		if (idx < 0) return 0;
		int n = 0;
		for (const level_pool::PoolFile& f: level_pool::poolWorlds[idx].files)
			if (f.active) n++;
		return n;
	}

	/* scan one world dir (<base>\\res\\levels\\versus\\<dir>) into the cache */
	void PoolWorldScan(const char* dirA, const char* dirName)
	{
		wchar_t pat[MAX_PATH + 16];
		if (!MultiByteToWideChar(CP_ACP, 0, dirA, -1, pat, MAX_PATH + 16)) return;
		wcscat(pat, L"\\*.tmx");
		std::vector<std::string> names;
		WIN32_FIND_DATAW fd;
		HANDLE h = FindFirstFileW(pat, &fd);
		if (h != INVALID_HANDLE_VALUE)
		{
			do
			{
				if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
				{
					char an[260];
					if (WideCharToMultiByte(CP_ACP, 0, fd.cFileName, -1, an, 260, nullptr, nullptr))
						names.push_back(an);
				}
			} while (FindNextFileW(h, &fd));
			FindClose(h);
		}
		else
		{
			mod::Log("poolcache: {}: scan FAILED (err {})", dirA, GetLastError());
			return;
		}
		std::sort(names.begin(), names.end(), PoolTmxBefore);

		level_pool::PoolWorld w;
		w.id = level_pool::PoolWorldId(dirName); /* 0 = not in the worldType enum */
		w.dir = dirName;
		int cg = 0, nonCg = 0;
		for (const std::string& nm: names)
		{
			if (nm.rfind("cg_", 0) == 0)
			{
				cg++;
				continue;
			} /* cash grab: not pooled at all */
			level_pool::PoolFile pf;
			pf.name = nm;
			pf.path = std::string("levels/versus/") + dirName + "/" + nm; /* res-relative */
			nonCg++;
			pf.active = true; /* default: everything on */
			pf.slot = 0; /* assigned below */
			w.files.push_back(pf);
		}
		int act = PoolWorldAssignSlots(w);
		int idx = (int) level_pool::poolWorlds.size();
		level_pool::poolWorlds.push_back(w);
		if (w.id >= 1 && w.id < level_pool::poolWorldMax) level_pool::poolWorldById[w.id] = idx;
		if (nonCg < 2)
			mod::Log(
				"poolcache ERROR: {}: only {} level(s) found (< 2) -- a pool of {} is below the walk's safe minimum (its exhaust path is an assert); {}",
				dirName, nonCg, act,
				act == 1 ? "count faked to 2 to survive (single-map mode)" : "CRASH/FREEZE expected in this world");
		mod::Log("poolcache: {}: {} tmx, {} active (all on by default, cg_ skipped: {}){} -> count={}",
		         dirName, (int) w.files.size(), act, cg,
		         (w.id < 1) ? " [worldType unknown, not applied]" : "", act);
	}

	void PoolCacheBuild()
	{
		char base[MAX_PATH] = {0};
		if (!GetModuleFileNameA(mod::dllModuleBase, base, MAX_PATH))
		{
			mod::Log("poolcache: GetModuleFileName FAILED");
			return;
		}
		char* slash = strrchr(base, '\\');
		if (!slash)
		{
			mod::Log("poolcache: no dir in {}", base);
			return;
		}
		*slash = 0;
		char dir[MAX_PATH + 32];
		snprintf(dir, sizeof dir, "%s\\res\\levels\\versus", base);
		snprintf(resDir, sizeof resDir, "%s\\res", base);

		wchar_t pat[MAX_PATH + 32];
		if (!MultiByteToWideChar(CP_ACP, 0, dir, -1, pat, MAX_PATH + 32)) return;
		wcscat(pat, L"\\*");
		WIN32_FIND_DATAW fd;
		HANDLE h = FindFirstFileW(pat, &fd);
		if (h == INVALID_HANDLE_VALUE)
		{
			mod::Log("poolcache: {} not found (err {}) -- all worlds stock", dir, GetLastError());
			return;
		}
		int worlds = 0;
		do
		{
			if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' || (
				                                fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
				continue;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				char an[MAX_PATH];
				if (WideCharToMultiByte(CP_ACP, 0, fd.cFileName, -1, an, MAX_PATH, nullptr, nullptr))
				{
					char wdir[MAX_PATH + 64];
					snprintf(wdir, sizeof wdir, "%s\\%s", dir, an);
					PoolWorldScan(wdir, an);
					worlds++;
				}
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
		mod::Log("poolcache: {} world dir(s) cached under {}", worlds, dir);
	}

	/* ------------------------------------------------------------------ */
	/* pool editor draft + apply (UI)                                      */
	/*                                                                     */
	/* The UI edits a DRAFT copy of the active flags (one vector per world,*/
	/* parallel to poolWorlds). Apply copies the selected world's draft*/
	/* into the live cache and recompacts its slots. PoolActiveCount (and */
	/* and the glv remap) read the LIVE cache, so the count only moves    */
	/* on Apply. Threading: the draft + live cache are touched by the      */
	/* render thread (UI) only after poolReady; the game thread reads   */
	/* the live cache via PoolActiveCount (minor race, single-player local*/
	/* mod -- the UI is used at menus, not during level loads).            */
	/* ------------------------------------------------------------------ */
	int poolSelectedWorld = 0; /* index into poolWorlds */
	std::vector<std::vector<char> > poolDraft; /* draft active flags (0/1) */

	/* draft = current live flags; once, after PoolCacheBuild */
	void PoolDraftInit()
	{
		poolDraft.resize(level_pool::poolWorlds.size());
		for (size_t i = 0; i < level_pool::poolWorlds.size(); i++)
		{
			poolDraft[i].resize(level_pool::poolWorlds[i].files.size());
			for (size_t j = 0; j < level_pool::poolWorlds[i].files.size(); j++)
				poolDraft[i][j] = level_pool::poolWorlds[i].files[j].active ? 1 : 0;
		}
		InterlockedExchange(&level_pool::poolReady, 1);
	}

	/* copy the selected world's draft into the live cache + recompact slots */
	void PoolApplyWorld(int i)
	{
		if (i < 0 || i >= (int) level_pool::poolWorlds.size()) return;
		level_pool::PoolWorld& w = level_pool::poolWorlds[i];
		int changed = 0;
		for (size_t j = 0; j < w.files.size() && j < poolDraft[i].size(); j++)
		{
			bool a = poolDraft[i][j] != 0;
			if (a != w.files[j].active)
			{
				w.files[j].active = a;
				changed++;
			}
		}
		int count = PoolWorldAssignSlots(w);
		mod::Log("poolui: APPLY {} -- count={} ({} flag change(s))", w.dir.c_str(), count, changed);
	}

	/* set every world's draft to a value (1=select all, 0=clear all) and apply
	 * all worlds into the live cache in one shot. */
	void PoolApplyAll(int value)
	{
		for (size_t i = 0; i < level_pool::poolWorlds.size() && i < poolDraft.size(); i++)
		{
			for (char& c: poolDraft[i]) c = (char) value;
			PoolApplyWorld((int) i); /* copies draft -> live + recompacts slots */
		}
		mod::Log("poolui: {} all worlds -- count={} applied to every world",
		         value ? "RESET" : "CLEAR", value);
	}
}

std::vector<level_pool::PoolWorld> level_pool::poolWorlds;
int level_pool::poolWorldById[] = {-1}; /* index into poolWorlds, -1 = none */
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
	if (!poolReady || poolWorlds.empty())
	{
		ImGui::TextDisabled("no worlds cached (res/levels/versus missing?)");
	}
	else
	{
		if (poolSelectedWorld < 0 || poolSelectedWorld >= (int) poolWorlds.size())
			poolSelectedWorld = 0;
		const PoolWorld& sel = poolWorlds[poolSelectedWorld];
		if (ImGui::BeginCombo("##world", sel.dir.c_str()))
		{
			for (int i = 0; i < (int) poolWorlds.size(); i++)
			{
				bool s = ImGui::Selectable(poolWorlds[i].dir.c_str(), poolSelectedWorld == i);
				if (s) poolSelectedWorld = i;
			}
			ImGui::EndCombo();
		}
		ImGui::Spacing();
		/* global actions (all worlds) */
		if (ImGui::Button("Open res folder"))
		{
			ShellExecuteA(NULL, "open", resDir, NULL, NULL, SW_SHOWNORMAL);
			mod::Log("poolui: opened res folder {}", resDir);
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
		for (size_t j = 0; j < sel.files.size() && j < draft.size(); j++)
		{
			bool b = draft[j] != 0;
			if (ImGui::Checkbox(sel.files[j].name.c_str(), &b))
				draft[j] = b ? 1 : 0;
			ImGui::SameLine();
			ImGui::TextDisabled("%s", sel.files[j].path.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled(sel.files[j].active ? "[slot %d]" : "[off]", sel.files[j].slot);
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
		if (ImGui::Button(("Apply " + sel.dir).c_str()))
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
	PoolCacheBuild();
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

const char* level_pool::PoolWorldName(int id)
{
	const char* names[poolWorldMax] = {
		"", "mines", "frontier", "flare", "darklab", "echocity", "highrise",
		"sunken", "graveyard", "gasrigs", "moonbase", "urspace", "void",
		"legacy", "up", "unsorted"
	};
	return (id >= 1 && id < poolWorldMax) ? names[id] : "?";
}

int level_pool::PoolWorldId(const char* name)
{
	for (int i = 1; i < poolWorldMax; i++)
		if (strcmp(PoolWorldName(i), name) == 0) return i;
	return 0;
}
