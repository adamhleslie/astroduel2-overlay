#include "hooks_game.h"

#include <windows.h>
#include <cstdio>
#include <MinHook.h>
#include <string>
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
	auto worldOpt = level_pool::GetWorld(type);
	int override = (worldOpt ? worldOpt->get().GetActiveLevelCount() : 0);
	if (override > 0) f[3] = override;
	mod::Log(worldDataCategory, "world={}({}) out={} cnt={} (stock {}){} ra0={}",
	         worldType, type, r, f[3], stock,
	         override ? " OVERRIDDEN" : "", ad2::MemoryOffset{GetCallerReturnAddress()});
	return r;
}

static std::optional<std::reference_wrapper<const level_pool::Level>> GetCustomLevel(int worldType, int slot, bool cashGrab)
{
	assert(level_pool::IsInitialized());
	if (cashGrab || (*ad2::addresses::versusMode == ad2::VersusMode::CashGrab))
	{
		// Cash grab not supported
		return std::nullopt;
	}

	if (auto worldOpt = level_pool::GetWorld(worldType))
	{
		const level_pool::World& world = *worldOpt;

		// If not deathmatch, apply custom randomization
		if (*ad2::addresses::versusMode != ad2::VersusMode::Deathmatch)
		{
			if (auto levelOpt = level_pool::GetRandomLevel(world.type))
			{
				const level_pool::Level& level = levelOpt.value();
				mod::Log(levelCategory, "Random: {}", level.gamePath);
				return levelOpt;
			}
		}

		if (auto levelOpt = world.GetLevelAtSlot(slot))
		{
			const level_pool::Level& level = levelOpt.value();
			if (level.IsAtDefaultSlot())
			{
				// No remap needed
				return std::nullopt;
			}

			mod::Log(levelCategory, "Remap: world={}({}) slot={} -> {} | default={}", worldType, world.type, slot, level.name, level.defaultSlot);
			return levelOpt;
		}
		else
		{
			mod::Log(levelCategory, "Failed Remap: world={}({}) slot={} -> no slot (outside active pool)", worldType, world.type, slot);
			return std::nullopt;
		}
	}

	return std::nullopt;
}

// Replace the game's default level path composed string to the slot's actual .tmx per the live cache.
static void GetLevelPathHook(std::string* levelPathOut, int worldType, int levelNum, bool cashGrab)
{
	assert(getLevelPathOrig);
	getLevelPathOrig(levelPathOut, worldType, levelNum, cashGrab);
	if (auto levelOpt = GetCustomLevel(worldType, levelNum, cashGrab))
	{
		*levelPathOut = levelOpt->get().gamePath;
	}

	mod::Log(levelCategory, "world={}({}) num={} cashGrab={} path=\"{}\" caller={}",
	         worldType, ext::enchantum::cast_or(worldType, ad2::WorldType::Invalid), levelNum, cashGrab, *levelPathOut, ad2::MemoryOffset{GetCallerReturnAddress()});
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
