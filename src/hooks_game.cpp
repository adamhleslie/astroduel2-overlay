#include "hooks_game.h"

#include <windows.h>
#include <cstdio>
#include <MinHook.h>
#include <string>
#include <vector>
#include <enchantum/std_format.hpp>

#include "ad2_addresses.h"
#include "extensions_enchantum.h"
#include "mod.h"
#include "level_pool.h"

namespace
{
	constexpr std::string_view category = "game";
	constexpr std::string_view worldDataCategory = "get-world-data";
	constexpr std::string_view levelCategory = "get-level";

	decltype(ad2::addresses::getWorldData)::Type* getWorldDataOrig = nullptr;
	decltype(ad2::addresses::getLevelPath)::Type* getLevelPathOrig = nullptr;

	// _ReturnAddress() replacement
	void* GetCallerReturnAddress()
	{
		// Skip over the caller of this function, to their caller (the caller's call site)
		PVOID backTrace[1];
		RtlCaptureStackBackTrace(1, 1, backTrace, nullptr);
		return backTrace[0];
	}
}

/* ------------------------------------------------------------------ */
/* MinHook hook on WorldDataLoader::worldDataForType (0x1402ae190).   */
/* Defer-and-patch: GetWorldDataHook calls the trampoline (== the      */
/* original function: stolen prologue re-executes and jmps into the   */
/* continuation), which fills out[0..3] = {d0,d1,d2,d3}; we own ONLY  */
/* d3 (the per-world level count, +0xc) and leave d0/d1/d2 untouched   */
/* -- they always come from the real code. The count override comes   */
/* from the versus level cache (active .tmx files per world); worlds  */
/* not in the cache run 100% stock.                                    */
/*                                                                      */
/* Entry: 5-byte E9 rel32 to a +2^20 relay (jmp GetWorldDataHook).     */
/* The 14-byte stolen prologue lands in the 64-byte slot's trampoline */
/* half. Failure mode: if VMP ever re-writes the entry, the hook      */
/* silently disengages (stock behavior, no crash) -- the verifier     */
/* logs it.                                                          */
/* ------------------------------------------------------------------ */

/* RA forensics: MinHook patches the target entry with an absolute JMP to
   the detour, so the game's CALL has already pushed the real return
   address before we arrive -- _ReturnAddress() (frame-pointer based,
   valid with /Oy- from CMakeLists) == the game caller's rip, i.e. the
   instruction right after their call. log it module-relative; it names
   the exact call site. (Depth-2 walk unavailable: MSVC's C
   _returnaddress(N) is x86-only; a MASM stub could add it if ever
   needed -- ra0 already names the call site.) */
static void* GetWorldDataHook(void* out, int worldType)
{
	assert(getWorldDataOrig);
	void* r = getWorldDataOrig(out, worldType);
	int* f = (int*) r; /* f[0..3] = d0,d1,d2,d3 */
	const int stock = f[3];
	ad2::WorldType type = ext::enchantum::cast_or(worldType, ad2::WorldType::Invalid);
	auto worldOpt = level_pool::GetWorldByType(type);
	int override = (worldOpt ? worldOpt->get().GetActiveLevelCount() : 0);
	if (override > 0) f[3] = override;
	mod::Log(worldDataCategory, "world={}({}) out={} cnt={} (stock {}){} ra0={}",
	         worldType, type, r, f[3], stock,
	         override ? " OVERRIDDEN" : "", ad2::MemoryOffset{GetCallerReturnAddress()});
	return r;
}

/* ------------------------------------------------------------------ */
/* MinHook: LOG-ONLY hook on the level-string composer getLevelString */
/* (0x14026C1C0: rcx=out std::string, rdx=world, r8=level num,        */
/* r9=mode). EVERY level-load path composes its path here -- the whole*/
/* binary has exactly 4 call sites:                                   */
/*   0x140013d9e/0x140013e30   preload, in FUN_140013c00              */
/*   0x140031456               versus pick path 0x140030ee0          */
/*   0x14026cda2               alt cluster 0x14026cce0 (world global  */
/*                             DAT_1405de550, num from obj+0x114),    */
/*                             called from 0x14026b9c0/0x14026bf20    */
/* ra0 tells which composer fired. Per call the hook also logs the    */
/* pick context the picker refreshes before the chain -- pick         */
/* (DAT_1405b9ff0: the versus game-mode enum), spec (the player-      */
/* specified level: DAT_1405de480 when DAT_1405de607 is set, else     */
/* singleton DAT_1405de9c8 + 0x45c) and cnt (DAT_1405de56c = the d3   */
/* level count). Zero behavioral change.                              */
/* ------------------------------------------------------------------ */

/* Private PRNG (splitmix64) -- must NOT use srand/rand: the game imports
 * the CRT rand (IAT-verified) and its own randomization rides that single
 * shared generator; reseeding or consuming from it would perturb the
 * game's sequences. Only the game thread ever calls this. */
static uint64_t rngState = 0;

static int RandBelow(int n)
{
	if (!rngState)
		rngState = (uint64_t) GetTickCount() ^ (uint64_t) (uintptr_t) &rngState;
	uint64_t z = (rngState += 0x9E3779B97F4A7C15ULL);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return (int) ((z ^ (z >> 31)) % (uint64_t) n);
}

/* Random-map candidate pick for one PH/TA load. Returns false (caller runs
 * the normal slot/identity logic) for Off randMode or an empty candidate pool;
 * otherwise fills *out with a uniformly random candidate's full relative
 * path ("levels/versus/<world>/<file>") -- the world in the path may differ
 * from the one being loaded (Any/AnyActive). */
static bool PoolRandPick(level_pool::RandMode randMode, const level_pool::World& world, std::string& out)
{
	if (randMode == level_pool::RandMode::Off)
	{
		return false;
	}

	// candidate = (world dir, file name) references into the cache; the relative path is built once, only for the picked entry.
	std::vector<std::pair<const std::string*, const std::string*>> c;
	auto addLevels = [&c](std::optional<std::reference_wrapper<const level_pool::World>> worldOpt, bool activeOnly)
	{
		if (worldOpt)
		{
			const level_pool::World& world = *worldOpt;
			for (const level_pool::Level& level : world.levels)
			{
				if (!activeOnly || level.active)
				{
					c.emplace_back(&world.name, &level.name);
				}
			}
		}
	};

	switch (randMode)
	{
		case level_pool::RandMode::WorldActive: addLevels(world, true);
			break;
		case level_pool::RandMode::World: addLevels(world, false);
			break;
		case level_pool::RandMode::AnyActive: for (ad2::WorldType worldType : enchantum::values_generator<ad2::WorldType>) addLevels(level_pool::GetWorldByType(worldType), true);
			break;
		case level_pool::RandMode::Any: for (ad2::WorldType worldType : enchantum::values_generator<ad2::WorldType>) addLevels(level_pool::GetWorldByType(worldType), false);
			break;
	}

	if (!c.empty())
	{
		const auto& pick = c[RandBelow((int) c.size())];
		out = "levels/versus/" + *pick.first + "/" + *pick.second;
		return true;
	}

	return false;
}

/* glv remap (stage 2): the composer built levels/versus/<world>/<world><num>.tmx
   with num = pool slot (1..K, via the WdHook count). Rewrite the composed
   string to the slot's actual .tmx per the live cache.
   Guards, in order (each = silent no-op unless noted):
	 versusMode == 3            cash grab -- composer ignores num (cg_<world>.tmx);
						  the 4th param is 0 in BOTH branches (proven in the
						  caller FUN_140013c00: it sets DAT_1405b9ff0 but passes
						  0); versusMode is the real discriminator
	 world out of range   void/default/unknown are not pooled
	 no slot == num       outside the active pool (count grew) -> ERROR log
	 name == <w><num>.tmx identity -- the file IS its slot; no rewrite
   Random-map cashGrab (randMode != 0) redirects the load to a uniformly
   random candidate's full path BEFORE the slot lookup -- PH/TA loads only:
   deathmatch (versusMode == 2, player-specified) and the deathmatch UI preview
   (cashGrab == 1) keep the normal slot remap below; the 1/N identity versusMode is
   negligible and rewrites anyway.
   Deathmatch loads DO get the normal slot remap: the picker is bounded by
   the WdHook count, so num is in [1..K] and the table must stay total over
   it (a player-specified level lands on a valid active map).
   Rewrites via assign() on the game's 32-byte std::string: the overlay's
   std::string is layout-identical (same MSVC 32-byte SSO-15: ptr/SSO@0x00,
   len@0x10, cap@0x18, SSO sentinel 0xf, heap cap = len|0xf min 16 -- verified
   against the game's own CtorCopy 0x140008700 / Dtor 0x140003000 and observed
   live in the log: len=30, cap=31); shared UCRT process heap, so assign()
   may realloc the slot's buffer safely. */
static void ApplyLevelPoolRename(std::string& out, int worldType, int num, bool cashGrab)
{
	if (!level_pool::IsInitialized()) return;
	if (*ad2::addresses::versusMode == 3) return; /* cash grab */

	if (auto worldOpt = level_pool::GetWorldByType(worldType))
	{
		const level_pool::World& world = *worldOpt;

		/* random map selection -- PH/TA loads only (deathmatch and its UI
		preview take the normal slot remap below). Off cashGrab or an empty
		candidate pool: fall through. */
		if (*ad2::addresses::versusMode != 2 && !cashGrab)
		{
			level_pool::RandMode randMode = level_pool::GetRandMode();
			if (PoolRandPick(randMode, world, out))
			{
				mod::Log(levelCategory, "RAND cashGrab={} world={}({}) num={} -> {}", static_cast<int>(randMode), worldType, world.type, num, out);
				return;
			}
		}

		const level_pool::Level* pf = nullptr;
		for (size_t j = 0; j < world.levels.size(); j++)
			if (world.levels[j].slot == num)
			{
				pf = &world.levels[j];
				break;
			}
		if (!pf)
		{
			/* single-map world: count is faked to 2, so num may be 2 with only
			   slot 1 populated -- resolve any in-range num to the lone file */
			int act = 0;
			const level_pool::Level* only = nullptr;
			for (const level_pool::Level& f: world.levels)
				if (f.active)
				{
					act++;
					only = &f;
				}
			if (act == 1) pf = only;
		}
		if (!pf)
		{
			mod::Log(levelCategory, "REMAP MISS - world={}({}) num={} - no slot (outside active pool)", worldType, world.type, num);
			return;
		}
		const std::string ident = std::format("{}{}.tmx", world.name, num);
		if (pf->name == ident) return; /* identity: file IS slot num */
		out = std::format("levels/versus/{}/{}", world.name, pf->name);
		mod::Log(levelCategory, "REMAP - world={}({}) slot={} {} -> {}", worldType, world.type, num, ident, pf->name);
	}
}

static void GetLevelPathHook(std::string* out, int worldType, int levelNum, bool cashGrab)
{
	assert(getLevelPathOrig);
	getLevelPathOrig(out, worldType, levelNum, cashGrab);
	ApplyLevelPoolRename(*out, worldType, levelNum, cashGrab);

	mod::Log(levelCategory, "world={}({}) num={} cashGrab={} path=\"{}\" caller={}",
	         worldType, ext::enchantum::cast_or(worldType, ad2::WorldType::Invalid), levelNum, cashGrab, *out, ad2::MemoryOffset{GetCallerReturnAddress()});
}

template <typename T>
bool HookGameFunction(const ad2::MemoryOffset& target, T* hook, T*& original, const std::string& name)
{
	LPVOID targetAddress = target.ToAddress();
	LPVOID hookAddress = reinterpret_cast<LPVOID>(hook);
	LPVOID* originalAddress = reinterpret_cast<LPVOID*>(&original);
	if (MH_CreateHook(targetAddress, hookAddress, originalAddress) != MH_OK)
	{
		mod::Log(category, "{} - MH_CreateHook failed at {}", name, target);
		return false;
	}
	if (MH_EnableHook(targetAddress) != MH_OK)
	{
		mod::Log(category, "{} - MH_EnableHook failed at {}", name, target);
		MH_RemoveHook(targetAddress);
		return false;
	}
	mod::Log(category, "{} - Hook installed at {}", name, target);
	return true;
}

bool hooks::game::InstallHooks()
{
	// TODO: Call MH_Uninitialize
	if (MH_Initialize() != MH_OK)
	{
		mod::Log(category, "MH_Initialize FAILED");
		return false;
	}

	return HookGameFunction(ad2::addresses::getLevelPath, GetLevelPathHook, getLevelPathOrig, NAME_OF(ad2::addresses::getLevelPath)) &&
		HookGameFunction(ad2::addresses::getWorldData, GetWorldDataHook, getWorldDataOrig, NAME_OF(ad2::addresses::getWorldData));
}
