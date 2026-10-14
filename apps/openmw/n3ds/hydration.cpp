#include "hydration.hpp"

#include "../mwbase/environment.hpp"
#include "../mwworld/class.hpp"
#include "../mwbase/world.hpp"
#include "../mwworld/cell.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/livecellref.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/refdata.hpp"
#include "../mwworld/scene.hpp"

#include <components/esm/position.hpp>
#include <components/misc/resourcehelpers.hpp>

#include <platform3ds/log.hpp>

#include <3ds.h>

#include <algorithm>
#include <string>
#include <unordered_set>

namespace omw3ds::hydration
{
    namespace
    {
        osg::Vec3f gLoadCenter;

        // Objects that failed to insert (a broken model): not retried on every pass.
        std::unordered_set<const MWWorld::LiveCellRefBase*> gFailed;
        std::vector<MWWorld::CellStore*> gCells; // active cells as of the last pass
        std::unordered_set<const MWWorld::LiveCellRefBase*> gPreloaded; // asked for once per set of active cells
        std::size_t gCursor = 0;

        // Insertions stop for the frame once this much time went into them (a model not loaded yet comes from
        // the BSA on the SD card: tens of ms). At least one per frame. Walking speed leaves ~2 s between
        // kInsertRadius and the viewing distance.
        constexpr double kInsertBudgetMs = 4.0;

        float insertRadius(const MWWorld::Ptr& ptr)
        {
            return ptr.getClass().isActor() ? kActorInsertRadius : kInsertRadius;
        }

        float distance2(const MWWorld::Ptr& ptr, const osg::Vec3f& center)
        {
            const osg::Vec3f pos = ptr.getRefData().getPosition().asVec3();
            const float dx = pos.x() - center.x();
            const float dy = pos.y() - center.y();
            return dx * dx + dy * dy;
        }

        bool managed(const MWWorld::Ptr& ptr)
        {
            // Disabled objects are out of the scene on purpose (World::enable/disable owns them).
            return !ptr.mRef->isDeleted() && ptr.getRefData().isEnabled();
        }
    }

    void setLoadCenter(const osg::Vec3f& position)
    {
        gLoadCenter = position;
    }

    void filterInsert(std::vector<MWWorld::Ptr>& toInsert)
    {
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        const std::size_t before = toInsert.size();
        toInsert.erase(std::remove_if(toInsert.begin(), toInsert.end(),
                           [&](const MWWorld::Ptr& ptr) {
                               const float radius = insertRadius(ptr);
                               return ptr != player && distance2(ptr, gLoadCenter) > radius * radius;
                           }),
            toInsert.end());
        // Scenery before actors, so actors land on their floor (the visitor inserts in this order).
        std::stable_partition(
            toInsert.begin(), toInsert.end(), [](const MWWorld::Ptr& ptr) { return !ptr.getClass().isActor(); });
        P3DS_LOGV("hydration: %zu of %zu objects inserted", toInsert.size(), before);
    }

    void update(MWWorld::Scene& scene)
    {
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        if (player.isEmpty() || !player.isInCell() || !player.getCell()->isExterior())
            return;

        const MWWorld::Scene::CellStoreCollection& active = scene.getActiveCells();
        if (active.empty())
            return;
        if (gCells.size() != active.size() || !std::equal(gCells.begin(), gCells.end(), active.begin()))
        {
            gCells.assign(active.begin(), active.end());
            gFailed.clear();
            gPreloaded.clear();
        }
        MWWorld::CellStore* cell = gCells[gCursor++ % gCells.size()];
        if (!cell->isExterior())
            return;

        const osg::Vec3f center = player.getRefData().getPosition().asVec3();
        const float removeLimit = kRemoveRadius * kRemoveRadius;
        std::vector<MWWorld::Ptr> toInsert;
        std::vector<MWWorld::Ptr> toRemove;
        std::vector<MWWorld::Ptr> toPreload;
        // Collect first: inserting may change the cell (a levelled creature list spawns its creature).
        cell->forEach([&](const MWWorld::Ptr& ptr) {
            if (ptr == player || !managed(ptr))
                return true;
            const bool inScene = ptr.getRefData().getBaseNode() != nullptr;
            const float d2 = distance2(ptr, center);
            const float radius = insertRadius(ptr);
            if (!inScene && d2 < radius * radius && !gFailed.contains(ptr.mRef))
                toInsert.push_back(ptr);
            else if (!inScene && d2 < (radius + kPreloadMargin) * (radius + kPreloadMargin)
                && !gPreloaded.contains(ptr.mRef))
                toPreload.push_back(ptr);
            else if (inScene && d2 > removeLimit)
                toRemove.push_back(ptr);
            return true;
        });

        for (const MWWorld::Ptr& ptr : toRemove)
            scene.removeObjectFromScene(ptr);

        // Every model the object uses: an NPC is a dozen (body parts, equipment), ~0.3-0.5 s to load on the main
        // thread otherwise. Paths corrected as CellPreloader does.
        std::vector<VFS::Path::NormalizedView> models;
        for (const MWWorld::Ptr& ptr : toPreload)
        {
            gPreloaded.insert(ptr.mRef);
            models.clear();
            ptr.getClass().getModelsToPreload(ptr, models);
            for (VFS::Path::NormalizedView model : models)
                scene.preload(std::string(Misc::ResourceHelpers::correctMeshPath(model).value()), true);
        }

        // Scenery before actors, then nearest first.
        std::sort(toInsert.begin(), toInsert.end(), [&](const MWWorld::Ptr& a, const MWWorld::Ptr& b) {
            const bool actorA = a.getClass().isActor(), actorB = b.getClass().isActor();
            if (actorA != actorB)
                return actorB;
            return distance2(a, center) < distance2(b, center);
        });
        const uint64_t start = svcGetSystemTick();
        const uint64_t budget = uint64_t(kInsertBudgetMs * SYSCLOCK_ARM11 / 1000.0);
        for (const MWWorld::Ptr& ptr : toInsert)
        {
            const uint64_t before = svcGetSystemTick();
            scene.addObjectToScene(ptr);
            if (ptr.getRefData().getBaseNode() == nullptr)
                gFailed.insert(ptr.mRef);
            const uint64_t now = svcGetSystemTick();
            const double ms = double(now - before) * 1000.0 / SYSCLOCK_ARM11;
            if (ms > 50.0) // what still stalls (the model and collision shape were preloaded)
                P3DS_LOGI("hydration: inserting %s took %.0f ms", ptr.getCellRef().getRefId().toDebugString().c_str(), ms);
            if (now - start > budget)
                break;
        }
    }
}
