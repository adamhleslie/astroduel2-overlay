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

	// Replace the requested world data's level count with our custom count from the level pool
	ad2::WorldData* GetWorldDataHook(ad2::WorldData* worldConfig, int worldType)
	{
		assert(getWorldDataOrig);
		ad2::WorldData* result = getWorldDataOrig(worldConfig, worldType);
		assert(result == worldConfig);

		if (auto worldOpt = level_pool::GetWorld(worldType))
		{
			const level_pool::World& world = *worldOpt;
			int override = world.GetLevelCountForGame();
			if (override != result->levelCount)
			{
				if (override > 0)
				{
					mod::Log(worldDataCategory, "Override: worldType={}({}) levelCount={} -> {}", worldType, world.type, worldConfig->levelCount, override);
					result->levelCount = override;
				}
				else
				{
					mod::Log(worldDataCategory, "Failed Override: worldType={}({}) levelCount={} -> 0", worldType, world.type, worldConfig->levelCount);
				}
			}
		}

		mod::LogVerbose(worldDataCategory, "worldType={}({}) levelCount={} caller={}", worldType,
			ext::enchantum::cast_or(worldType, ad2::WorldType::Invalid), worldConfig->levelCount, ad2::MemoryOffset{GetCallerReturnAddress()});

		return result;
	}

	std::optional<std::reference_wrapper<const level_pool::Level>> GetCustomLevel(int worldType, int slot)
	{
		assert(level_pool::IsInitialized());
		if (*ad2::addresses::versusMode == ad2::VersusMode::CashGrab)
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
					// Already selected
					return std::nullopt;
				}

				mod::Log(levelCategory, "Override: world={}({}) slot={} -> {}", worldType, world.type, slot, level.name);
				return levelOpt;
			}
			else
			{
				mod::Log(levelCategory, "Failed Override: world={}({}) slot={} -> no slot (outside active pool)", worldType, world.type, slot);
				return std::nullopt;
			}
		}

		return std::nullopt;
	}

	// Replace the game's default level path composed string with the slot's actual path in the level pool
	void GetLevelPathHook(std::string* levelPathOut, int worldType, int levelNum, bool forUi)
	{
		assert(getLevelPathOrig);
		getLevelPathOrig(levelPathOut, worldType, levelNum, forUi);
		if (auto levelOpt = GetCustomLevel(worldType, levelNum))
		{
			*levelPathOut = levelOpt->get().gamePath;
		}

		mod::LogVerbose(levelCategory, "world={}({}) levelNum={} forUi={} path=\"{}\" caller={}",
		         worldType, ext::enchantum::cast_or(worldType, ad2::WorldType::Invalid), levelNum, forUi, *levelPathOut, ad2::MemoryOffset{GetCallerReturnAddress()});
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
