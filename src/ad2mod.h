#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace ad2mod
{
    void Log(const char* fmt, ...);

    extern uintptr_t s_module_base; /* main module (AstroDuel2.exe) */
}