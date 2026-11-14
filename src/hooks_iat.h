#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

// TODO: Move hooking library to ad2 namespace, and actual hooking to mod
// Import Address Table (IAT) Hooks
// Currently used for logging Windows file apis
namespace hooks::iat
{
	void InstallHooks();

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
