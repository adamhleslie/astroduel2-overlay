// steam_api64 proxy: forward every real export to the renamed original
// (steam_api64_real.dll) at link time. Ordinals are preserved 1:1.
//
// This translation unit is compiled into the steam_api64 target ONLY -- the
// standalone ad2ui.dll must not link it.
//
// Regenerate steam_forwards.inc after a game update:
//   ad2modkit/tools/gen_steam_proxy.py
#include "steam_forwards.inc"
