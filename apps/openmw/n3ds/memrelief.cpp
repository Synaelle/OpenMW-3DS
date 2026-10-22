#include "memrelief.hpp"

#include <cstdio>

#include <components/debug/debuglog.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/settings/values.hpp>

#include <platform3ds/log.hpp>
#include <platform3ds/memstats.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/scriptmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwlua/luamanagerimp.hpp"
#include "../mwscript/globalscripts.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/player.hpp"
#include "../mwworld/scene.hpp"
#include "../mwworld/worldmodel.hpp"

namespace omw3ds::memrelief
{
    namespace
    {
        constexpr uint32_t kLowHeadroom = 24u * 1024u * 1024u;
        bool gEvictPending = false;
        bool gLowMemory = false; // the last cell change found less than kLowHeadroom left
        constexpr unsigned kKeptCellsLow = 4;

        // Debug builds: sdmc:/openmw/evict-test keeps 1 cell, so a short emulator run evicts and restores.
        unsigned keptCells()
        {
#ifdef OPENMW3DS_DEBUG
            static const bool test = [] {
                std::FILE* f = std::fopen("sdmc:/openmw/evict-test", "r");
                if (f != nullptr)
                    std::fclose(f);
                return f != nullptr;
            }();
            if (test)
                return 1;
#endif
            return gLowMemory ? kKeptCellsLow : kKeptCells;
        }
    }

    void onCellChange(Resource::ResourceSystem& resources)
    {
        gEvictPending = true;
        const p3ds::mem::Stats before = p3ds::mem::queryExact();
        gLowMemory = before.heapUsed + kLowHeadroom >= before.heapTotal;
        if (!gLowMemory)
            return;
        resources.clearCache();
        MWBase::LuaManager* luaManager = MWBase::Environment::get().getLuaManager();
        if (auto* lua = dynamic_cast<MWLua::LuaManager*>(luaManager))
            lua->gcStep(1 << 20); // steps of 1 KB: a full cycle
        const p3ds::mem::Stats after = p3ds::mem::queryExact();
        P3DS_LOGI("memrelief: heap %.1f of %.1f MB, caches and Lua freed %.1f MB", p3ds::mem::toMB(before.heapUsed),
            p3ds::mem::toMB(before.heapTotal), (double(before.heapUsed) - double(after.heapUsed)) / 1048576.0);
    }

    void onFrame()
    {
        if (!gEvictPending)
            return;
        gEvictPending = false;
        // Cell preloading keeps CellStore pointers of its own: no eviction with it on. In debug builds,
        // sdmc:/openmw/no-evict turns eviction off.
        static const bool disabled = [] {
#ifndef OPENMW3DS_DEBUG
            return false;
#endif
            std::FILE* f = std::fopen("sdmc:/openmw/no-evict", "r");
            if (f != nullptr)
                std::fclose(f);
            if (f != nullptr)
                P3DS_LOGI("memrelief: cell eviction off (sdmc:/openmw/no-evict)");
            return f != nullptr;
        }();
        if (disabled || Settings::cells().mPreloadEnabled)
            return;
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        MWWorld::Scene& scene = *MWBase::Environment::get().getWorldScene();
        MWWorld::WorldModel& model = *MWBase::Environment::get().getWorldModel();
        const MWWorld::Ptr player = world.getPlayerPtr();
        const MWWorld::CellStore* playerCell = player.isInCell() ? player.getCell() : nullptr;
        MWWorld::CellStore* marked = nullptr;
        ESM::Position markedPosition;
        world.getPlayer().getMarkedPosition(marked, markedPosition);
        MWScript::GlobalScripts& globalScripts = MWBase::Environment::get().getScriptManager()->getGlobalScripts();
        const std::size_t evicted = model.evictCells(
            [&](const MWWorld::CellStore& cell) {
                if (&cell == marked || &cell == playerCell || scene.isCellActive(cell))
                    return true;
                // Global scripts targeting its objects forget the pointer (harmless for the cells that stay: they
                // resolve it again on use).
                globalScripts.releaseTargetsIn(cell);
                return false;
            },
            keptCells());
        if (evicted != 0)
            P3DS_LOGI("memrelief: %u cells evicted, %u kept as saved changes (%u KB)", unsigned(evicted),
                unsigned(model.evictedCount()), unsigned(model.evictedBytes() / 1024));
#ifdef OPENMW3DS_DEBUG
        if (keptCells() == 1 && model.evictedCount() != 0)
            P3DS_LOGI("memrelief: eviction self-test %s", model.selfTestEviction(2) ? "passed" : "FAILED");
#endif
    }
}
