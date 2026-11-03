#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace mod
{
	void Main();
	void Log(const char* fmt, ...);

	extern uintptr_t moduleBase; /* main module (AstroDuel2.exe) */
	extern HINSTANCE dllInstance; /* our module */
}
