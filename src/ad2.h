#pragma once

#include <enchantum/enchantum.hpp>

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
}

template<>
struct enchantum::enum_traits<ad2::WorldType>
{
	constexpr static auto min = std::to_underlying(ad2::WorldType::Mines);
	constexpr static auto max = std::to_underlying(ad2::WorldType::Unsorted);
};
