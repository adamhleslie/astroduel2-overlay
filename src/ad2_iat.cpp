#include "ad2_iat.h"

#include "mod.h"
#include "ad2_addresses.h"

namespace
{
	constexpr std::string_view category = "iat-hook";
}

bool ad2::iat::HookIat(uintptr_t address, uintptr_t hookAddress, const std::string& name)
{
	if (!address)
	{
		mod::Log(category, "{} - address is null", name);
		return false;
	}

	// TODO: Investigate smarter IAT reading
	// https://f3real.github.io/iat_hooking.html
	// https://medium.com/@s12deff/import-address-table-hooking-68d519a1da43
	// https://skr1x.github.io/portable-executable-format/ & https://skr1x.github.io/reflective-loading-portable-executable-memory/
	// Scan the whole IAT region
	for (MemoryOffset slotOffset = addresses::iatRegionStart; slotOffset < addresses::iatRegionEnd; slotOffset += 8)
	{
		uintptr_t* slotAddress = slotOffset.ToAddress<uintptr_t>();
		if (*slotAddress == address)
		{
			DWORD newProtect = PAGE_READWRITE;
			DWORD oldProtect = 0;
			if (!VirtualProtect(slotAddress, 8, newProtect, &oldProtect))
			{
				mod::Log(category, "{} - VirtualProtect FAILED {}", name, GetLastError());
				return false;
			}
			*slotAddress = hookAddress;
			VirtualProtect(slotAddress, 8, oldProtect, &newProtect);
			mod::Log(category, "{} - PATCHED slot @base0x+{} (orig={})", name, slotOffset, reinterpret_cast<void*>(address));
			return true;
		}
	}
	mod::Log(category, "{} - slot NOT FOUND in IAT {}..{}", name, addresses::iatRegionStart, addresses::iatRegionEnd);
	return false;
}