#include "hooks_game.h"

#include <cstdio>
#include <MinHook.h>
#include <intrin.h>    /* _ReturnAddress() -- hook caller forensics (x64 C++ intrinsic) */
#include <string>
#include <vector>
#include <enchantum/std_format.hpp>

#include "extensions_enchantum.h"
#include "mod.h"
#include "level_pool.h"

namespace
{
	constexpr std::string_view worldDataCategory = "get-world-data";
	constexpr std::string_view levelCategory = "get-level";

	constexpr uintptr_t getWorldData = 0x2AE190;
	constexpr uintptr_t getLevel = 0x26C1C0;

	void* GetPointerFromAnchor(uintptr_t module_base, uintptr_t anchor)
	{
		return reinterpret_cast<void*>(module_base + anchor);
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
static void* getWorldDataOrig = nullptr; /* MinHook trampoline = original function */
static volatile LONG getWorldDataState = 0; /* 0=off/failed, 1=installed+enabled */
// static int getWorldDataRevertLogged = 0;
/* getLevel state -- declared here; the hook is further below. */
static void* getLevelOrig = nullptr;
static volatile LONG getLevelState = 0;
// static int getLevelRevertLogged = 0;

typedef void* (*FnGetWorldData)(void* out, int worldType);

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
	if (!getWorldDataOrig)
	{
		mod::Log(worldDataCategory, "FATAL -- no trampoline; out buffer left zeroed");
		return out;
	}
	void* r = ((FnGetWorldData) getWorldDataOrig)(out, worldType);
	int* f = (int*) r; /* f[0..3] = d0,d1,d2,d3 */
	const int stock = f[3];
	ad2::WorldType type = ext::enchantum::cast_or(worldType, ad2::WorldType::Invalid);
	auto worldOpt = level_pool::GetWorldByType(type);
	int override = (worldOpt ? worldOpt->get().GetActiveLevelCount() : 0);
	if (override > 0) f[3] = override;
	mod::Log(worldDataCategory, "world={}({}) out={} cnt={} (stock {}){} ra0={:#010X}",
	         worldType, type, r, f[3], stock,
	         override ? " OVERRIDDEN" : "",(uint32_t) ((uintptr_t) _ReturnAddress() - mod::ad2ModuleBase));
	return r;
}

bool hooks::game::InstallGetWorldDataHook()
{
	if (MH_Initialize() != MH_OK)
	{
		mod::Log(worldDataCategory, "MH_Initialize FAILED");
		return false;
	}
	void* target = GetPointerFromAnchor(mod::ad2ModuleBase, getWorldData);
	if (MH_CreateHook(target, reinterpret_cast<void*>(GetWorldDataHook), &getWorldDataOrig) != MH_OK)
	{
		mod::Log(worldDataCategory, "MH_CreateHook FAILED -- REFUSED, stock behavior");
		return false;
	}
	/* EnableHookLL freezes all threads once while it writes the entry; do
	   it after the IAT hooks (render path stable) and well before the
	   ~t+9s preload block that first fires worldDataForType. */
	if (MH_EnableHook(target) != MH_OK)
	{
		mod::Log(worldDataCategory, "MH_EnableHook FAILED -- MH_RemoveHook, stock behavior");
		MH_RemoveHook(target);
		return false;
	}
	getWorldDataState = 1;
	mod::Log(worldDataCategory, "INSTALLED target={} trampoline={}", target, getWorldDataOrig);
	return true;
}

/* ~5s worker-loop check (both MinHook entries): the entry must still be
   our E9 rel32. If VMP re-writes it the hook has silently disengaged
   (stock behavior). log only, no re-arm. */
// static void VerifyEntryHooks()
// {
//     if (InterlockedCompareExchange(&getWorldDataState, 0, 0) == 1) {
//         uint8_t* p = (uint8_t*)(ad2mod::g_module_base + 0x2AE190);
//         if (p[0] != 0xE9 && getWorldDataRevertLogged++ < 3)
//             ad2mod::Log(worldDataCategory, "entry REVERTED (bytes %02x %02x %02x %02x) -- VMP? hook disengaged, stock behavior",
//                 p[0], p[1], p[2], p[3]);
//     }
//     if (InterlockedCompareExchange(&getLevelState, 0, 0) == 1) {
//         uint8_t* p = (uint8_t*)(ad2mod::g_module_base + 0x26C1C0);
//         if (p[0] != 0xE9 && getLevelRevertLogged++ < 3)
//             ad2mod::log("getLevel: entry REVERTED (bytes %02x %02x %02x %02x) -- VMP? hook disengaged, stock behavior",
//                 p[0], p[1], p[2], p[3]);
//     }
// }

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
typedef void* (*FnGetLevel)(void* out, int world, int num, char mode);

/* Game's 32-byte std::string layout (proven from the copy-ctor FUN_140008700
   and dtor FUN_140003000, not guessed):
	 bytes  0-7  word0: heap data ptr (heap mode) or SSO chars[0..7]
	 bytes  8-15 word1: SSO chars[8..15] (stale/garbage in heap mode)
	 bytes 16-23 word2: length
	 bytes 24-31 word3: capacity -- >15 => heap, ==15 => SSO (dtor frees word0 iff word3>15)
   Read-only, clamped. */
static void PeekStdString(void* s, char* buf, size_t bufsize)
{
	if (!s)
	{
		snprintf(buf, bufsize, "(nil)");
		return;
	}
	const uint64_t* w = (const uint64_t*) s;
	uint64_t len = w[2], cap = w[3];
	char* data = (cap > 15) ? (char*) w[0] : (char*) s; /* SSO chars in bytes 0-15 */
	if ((!data && cap > 15) || len > 127)
	{
		snprintf(buf, bufsize, "(bad ptr=%p len=%llu cap=%llu)", (void*) w[0], (unsigned long long) len,
		         (unsigned long long) cap);
		return;
	}
	size_t n = len < bufsize - 1 ? (size_t) len : bufsize - 1;
	memcpy(buf, data, n);
	buf[n] = 0;
}

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
static bool PoolRandPick(level_pool::RandMode randMode, const level_pool::World& world, std::string* out)
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
		*out = "levels/versus/" + *pick.first + "/" + *pick.second;
		return true;
	}

	return false;
}

/* glv remap (stage 2): the composer built levels/versus/<world>/<world><num>.tmx
   with num = pool slot (1..K, via the WdHook count). Rewrite the composed
   string to the slot's actual .tmx per the live cache.
   Guards, in order (each = silent no-op unless noted):
	 pick == 3            cash grab -- composer ignores num (cg_<world>.tmx);
						  the 4th param is 0 in BOTH branches (proven in the
						  caller FUN_140013c00: it sets DAT_1405b9ff0 but passes
						  0); pick is the real discriminator
	 world out of range   void/default/unknown are not pooled
	 no slot == num       outside the active pool (count grew) -> ERROR log
	 name == <w><num>.tmx identity -- the file IS its slot; no rewrite
   Random-map mode (randMode != 0) redirects the load to a uniformly
   random candidate's full path BEFORE the slot lookup -- PH/TA loads only:
   deathmatch (pick == 2, player-specified) and the deathmatch UI preview
   (mode == 1) keep the normal slot remap below; the 1/N identity pick is
   negligible and rewrites anyway.
   Deathmatch loads DO get the normal slot remap: the picker is bounded by
   the WdHook count, so num is in [1..K] and the table must stay total over
   it (a player-specified level lands on a valid active map).
   Rewrites via assign() on the game's 32-byte std::string (layout proven from
   its own copy-ctor FUN_140008700 / dtor FUN_140003000; shared UCRT heap,
   so assign() may realloc safely). */
static void GetLevelRemap(void* out, int worldType, int num, int pick, int mode)
{
	if (!level_pool::isInitialized.load() || !out) return;
	if (pick == 3) return; /* cash grab */

	if (auto worldOpt = level_pool::GetWorldByType(worldType))
	{
		const level_pool::World& world = *worldOpt;

		/* random map selection -- PH/TA loads only (deathmatch and its UI
		preview take the normal slot remap below). Off mode or an empty
		candidate pool: fall through. */
		if (pick != 2 && mode != 1)
		{
			std::string rpath;
			level_pool::RandMode randMode = level_pool::randMode.load();
			if (PoolRandPick(randMode, world, &rpath))
			{
				reinterpret_cast<std::string*>(out)->assign(rpath);
				mod::Log(levelCategory, "RAND mode={} world={}({}) num={} -> {}", static_cast<int>(randMode), worldType, world.type, num, rpath);
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
		char ident[48];
		snprintf(ident, sizeof ident, "%s%d.tmx", world.name.c_str(), num);
		if (strcmp(pf->name.c_str(), ident) == 0) return; /* identity: file IS slot num */
		char np[192];
		snprintf(np, sizeof np, "levels/versus/%s/%s", world.name.c_str(), pf->name.c_str());
		reinterpret_cast<std::string*>(out)->assign(np, strlen(np));
		mod::Log(levelCategory, "REMAP - world={}({}) slot={} {} -> {}", worldType, world.type, num, ident, pf->name.c_str());
	}
}

static void* GetLevelHook(void* out, int worldType, int num, char mode)
{
	void* ret = getLevelOrig ? ((FnGetLevel) getLevelOrig)(out, worldType, num, mode) : nullptr;
	if (!getLevelOrig)
	{
		mod::Log(levelCategory, "FATAL - no trampoline; composer NOT run");
		return ret;
	}
	/* probe: pick context FUN_14026B9C0 refreshes before the chain. */
	uintptr_t b = mod::ad2ModuleBase;
	int pick = *(volatile int32_t*) (b + 0x5B9FF0); /* DAT_1405b9ff0 */
	int cnt = *(volatile int32_t*) (b + 0x5DE56C); /* DAT_1405de56c (d3) */
	int spec = -1;
	if (*(volatile int8_t*) (b + 0x5DE607)) /* DAT_1405de607 */
		spec = (int) *(volatile int32_t*) (b + 0x5DE480); /* DAT_1405de480 */
	else
	{
		uintptr_t s = *(volatile uintptr_t*) (b + 0x5DE9C8); /* singleton */
		if (s >= 0x10000) spec = (int) *(volatile int32_t*) (s + 0x45C);
	}
	GetLevelRemap(out, worldType, num, pick, mode); /* slot -> file (stage 2); logs on rewrite/miss */
	char path[160];
	PeekStdString(out, path, sizeof(path)); /* post-rewrite: the final path */
	mod::Log(levelCategory, "world={}({}) num={} mode={} pick={} spec={} cnt={} path=\"{}\" ret={} ra0={:#010X}",
	         worldType, ext::enchantum::cast_or(worldType, ad2::WorldType::Invalid), num, (int) mode, pick, spec, cnt, path, ret,
	         (uint32_t) ((uintptr_t) _ReturnAddress() - mod::ad2ModuleBase));
	return ret;
}

bool hooks::game::InstallGetLevelHook()
{
	void* target = GetPointerFromAnchor(mod::ad2ModuleBase, getLevel);
	if (MH_CreateHook(target, reinterpret_cast<void*>(GetLevelHook), &getLevelOrig) != MH_OK)
	{
		mod::Log(levelCategory, "MH_CreateHook FAILED -- REFUSED, stock behavior");
		return false;
	}
	if (MH_EnableHook(target) != MH_OK)
	{
		mod::Log(levelCategory, "MH_EnableHook FAILED -- MH_RemoveHook, stock behavior");
		MH_RemoveHook(target);
		return false;
	}
	getLevelState = 1;
	mod::Log(levelCategory, "INSTALLED target={} trampoline={}", target, getLevelOrig);
	return true;
}
