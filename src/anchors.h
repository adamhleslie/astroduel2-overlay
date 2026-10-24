#pragma once

#include <cstdint>

namespace anchors
{
    inline constexpr uintptr_t get_world_data = 0x2AE190;
    inline constexpr uintptr_t get_level = 0x26C1C0;

    static void* GetPointerFromAnchor(uintptr_t module_base, uintptr_t anchor)
    {
        return reinterpret_cast<void *>(module_base + anchor);
    }
}
