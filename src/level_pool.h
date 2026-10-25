#pragma once

#include <string>
#include <vector>

namespace level_pool
{
    void DrawUI();

    void PoolCacheBuild();
    void PoolDraftInit();

    int PoolWorldEffectiveCount(int worldType);

    const char* PoolWorldName(int id);

    struct PoolFile { std::string name; std::string path; bool active; int slot; };
    struct PoolWorld { int id; std::string dir; std::vector<PoolFile> files; };
    extern std::vector<PoolWorld> s_pool_worlds;
    extern volatile long s_pool_ready;

    constexpr int pool_world_max = 16; /* world ids 1..15 */
    constexpr int pool_cap = 12; /* reachable ceiling: the walk pool only ever emits 1..12 */
    extern int s_pool_world_by_id[pool_world_max];

    extern volatile int s_rand_mode;
}
