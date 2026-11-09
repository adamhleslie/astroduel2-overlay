#include "ad2.h"

#include "mod.h"

// extern globals
uintptr_t ad2::ad2ModuleBase = 0;
std::filesystem::path ad2::resourcesPath;

namespace
{
	std::string_view category = "ad2";
}

void ad2::Init()
{
	// Cache base address for game
	HMODULE ad2Module = GetModuleHandleA(nullptr);
	ad2ModuleBase = reinterpret_cast<uintptr_t>(ad2Module);

	// Cache the location of the resources directory
	char ad2ModulePathString[MAX_PATH] = {};
	if (GetModuleFileNameA(ad2Module, ad2ModulePathString, MAX_PATH))
	{
		std::filesystem::path ad2ModulePath = ad2ModulePathString;
		if (ad2ModulePath.has_parent_path())
		{
			resourcesPath = ad2ModulePath.parent_path().append("res");
		}
		else
		{
			mod::Log(category, "No parent path for ad2Module {}", ad2ModulePathString);
		}
	}
	else
	{
		mod::Log(category, "GetModuleFileName for ad2Module FAILED");
	}
}
