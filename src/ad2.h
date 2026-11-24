#pragma once

#include <windows.h>
#include <filesystem>
#include <enchantum/enchantum.hpp>

// Astro Duel 2 base game content
namespace ad2
{
	// AstroDuel2 executable module
	extern HINSTANCE ad2Module;

	// The game's resources directory ("Astro Duel 2/res")
	extern std::filesystem::path resourcesPath;

	void Init();

#pragma region Game Data / Types
	constexpr int levelPoolCap = 12; // Maximum number of levels to select from in the versus level pool (Pilot Hunters / Team Annihilation)
	constexpr int levelDeathmatchCap = 20; // Maximum number of levels shown in the deathmatch level select UI

	enum class VersusMode
	{
		PilotHunters,
		TeamAnnihilation,
		Deathmatch,
		CashGrab
	};

	// See enchantum traits below
	enum class WorldType
	{
		Invalid = 0,

		Mines,
		Frontier,
		Flare,
		DarkLab,
		EchoCity,
		Highrise,
		Sunken,
		Graveyard,
		GasRigs,
		MoonBase,
		Urspace,
		Void,
		Legacy,
		Up,
		Unsorted,
	};

	struct WorldData
	{
		int data0;
		int data1;
		int data3;
		int levelCount;
	};
	static_assert((sizeof(WorldData) == 16), "WorldData must be 16 bytes");

#pragma endregion
}

template<>
struct enchantum::enum_traits<ad2::WorldType>
{
	constexpr static auto min = std::to_underlying(ad2::WorldType::Mines);
	constexpr static auto max = std::to_underlying(ad2::WorldType::Unsorted);
};
