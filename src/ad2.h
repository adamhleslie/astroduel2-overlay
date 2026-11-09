#pragma once

#include <filesystem>
#include <enchantum/enchantum.hpp>

// Astro Duel 2 base game content
namespace ad2
{
	constexpr int levelPoolCap = 12; // Maximum number of levels to select from in the versus level pool (Pilot Hunters / Team Annihilation)
	constexpr int levelDeathmatchCap = 20; // Maximum number of levels shown in the deathmatch level select UI

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

	// Executable module (AstroDuel2.exe)
	extern uintptr_t ad2ModuleBase;

	// The game's resources directory ("Astro Duel 2/res")
	extern std::filesystem::path resourcesPath;

	void Init();

	inline void* GetAddressFromAnchor(uintptr_t anchor)
	{
		return reinterpret_cast<void*>(ad2ModuleBase + anchor);
	}

	inline uintptr_t GetAnchorFromAddress(void* address)
	{
		return (reinterpret_cast<uintptr_t>(address) - ad2ModuleBase);
	}
}

template<>
struct enchantum::enum_traits<ad2::WorldType>
{
	constexpr static auto min = std::to_underlying(ad2::WorldType::Mines);
	constexpr static auto max = std::to_underlying(ad2::WorldType::Unsorted);
};
