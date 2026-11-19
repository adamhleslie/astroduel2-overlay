#pragma once

#include <windows.h>
#include <string>

namespace ad2::iat
{
	// Overwrites an address in the game's import address table.
	bool HookIat(uintptr_t address, uintptr_t hookAddress, const std::string& name);

	template <typename T>
	bool HookIatForModule(const std::string& module, const std::string& name, T hookAddress, T& replacedAddress)
	{
		HMODULE handle = GetModuleHandleA(module.c_str());
		uintptr_t address = handle ? reinterpret_cast<uintptr_t>(GetProcAddress(handle, name.c_str())) : 0;
		if (HookIat(address, reinterpret_cast<uintptr_t>(hookAddress), name))
		{
			replacedAddress = reinterpret_cast<T>(address);
			return true;
		}
		else
		{
			replacedAddress = nullptr;
			return false;
		}
	}

	// Kernel DLL Hook variant: slots may hold the kernelbase target or the kernel32 forwarder
	template <typename T>
	bool HookIatForKernel(const std::string& name, T hookAddress, T& replacedAddress)
	{
		return HookIatForModule("kernel32.dll", name, hookAddress, replacedAddress) ||
			   HookIatForModule("kernelbase.dll", name, hookAddress, replacedAddress);
	}
}