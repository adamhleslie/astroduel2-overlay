#pragma once

#include "ad2_memory_offset.h"

namespace ad2::addresses
{
	constexpr MemoryOffset getWorldData{0x2AE190}; // ad2::functions::GetWorldData
	constexpr MemoryOffset getLevel{0x26C1C0}; // ad2::functions::GetLevel

	constexpr MemoryOffset iatRegionStart{0x47e000};
	constexpr MemoryOffset iatRegionEnd{0x495a00};

	// 20-byte prologue, readable post-VMP unpack
	constexpr MemoryOffset unpackPrologue{0x26cce0};
	constexpr std::array<uint8_t, 20> unpackedPrologue =
	{
		0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
		0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20
	};
}
