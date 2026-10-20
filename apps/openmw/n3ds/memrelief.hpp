#ifndef OPENMW_APPS_OPENMW_N3DS_MEMRELIEF_H
#define OPENMW_APPS_OPENMW_N3DS_MEMRELIEF_H

namespace Resource
{
    class ResourceSystem;
}

// The memory watchdog (M9, the Vita port's idea):
// - at every cell change, between unloading the old cells and loading the new ones (a loading screen): when the
//   heap has less than kLowHeadroom left (an exact heap walk, as at every load), every resource cache entry nothing
//   uses is dropped (not only those older than the 5 s expiry) and Lua runs a full collection;
// - on the first frame after it, once the player, the maps and the scripts point at the new cells: cell eviction.
//   Beyond kKeptCells loaded cells, the ones left longest ago are written and freed (WorldModel::evictCells),
//   except the active cells, the player's and the Mark spell's; only 4 when the heap was short (test #73: out of
//   memory in Gnisis after a long trip).
namespace omw3ds::memrelief
{
    constexpr unsigned kKeptCells = 12;

    void onCellChange(Resource::ResourceSystem& resources);
    void onFrame();
}

#endif
