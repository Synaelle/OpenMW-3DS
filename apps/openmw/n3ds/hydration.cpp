#include "hydration.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwworld/cell.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/livecellref.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/refdata.hpp"
#include "../mwworld/scene.hpp"

#include <components/esm/position.hpp>

#include <platform3ds/log.hpp>

#include <algorithm>
#include <unordered_set>

namespace omw3ds::hydration
{
    namespace
    {
        osg::Vec3f gLoadCenter;

        // Objects that failed to insert (a broken model): not retried on every pass.
        std::unordered_set<const MWWorld::LiveCellRefBase*> gFailed;
        std::vector<MWWorld::CellStore*> gCells; // active cells as of the last pass
        std::size_t gCursor = 0;

        // A few insertions per frame: a new model loads from the BSA (tens of ms). Walking speed leaves ~2 s
        // between kInsertRadius and the viewing distance.
        constexpr int kMaxInsertsPerFrame = 4;

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
        const float limit = kInsertRadius * kInsertRadius;
        const std::size_t before = toInsert.size();
        toInsert.erase(std::remove_if(toInsert.begin(), toInsert.end(),
                           [&](const MWWorld::Ptr& ptr) { return ptr != player && distance2(ptr, gLoadCenter) > limit; }),
            toInsert.end());
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
        }
        MWWorld::CellStore* cell = gCells[gCursor++ % gCells.size()];
        if (!cell->isExterior())
            return;

        const osg::Vec3f center = player.getRefData().getPosition().asVec3();
        const float insertLimit = kInsertRadius * kInsertRadius;
        const float removeLimit = kRemoveRadius * kRemoveRadius;
        std::vector<MWWorld::Ptr> toInsert;
        std::vector<MWWorld::Ptr> toRemove;
        // Collect first: inserting may change the cell (a levelled creature list spawns its creature).
        cell->forEach([&](const MWWorld::Ptr& ptr) {
            if (ptr == player || !managed(ptr))
                return true;
            const bool inScene = ptr.getRefData().getBaseNode() != nullptr;
            const float d2 = distance2(ptr, center);
            if (!inScene && d2 < insertLimit && !gFailed.contains(ptr.mRef))
                toInsert.push_back(ptr);
            else if (inScene && d2 > removeLimit)
                toRemove.push_back(ptr);
            return true;
        });

        for (const MWWorld::Ptr& ptr : toRemove)
            scene.removeObjectFromScene(ptr);

        // Nearest first.
        std::sort(toInsert.begin(), toInsert.end(), [&](const MWWorld::Ptr& a, const MWWorld::Ptr& b) {
            return distance2(a, center) < distance2(b, center);
        });
        int inserted = 0;
        for (const MWWorld::Ptr& ptr : toInsert)
        {
            if (inserted++ == kMaxInsertsPerFrame)
                break;
            scene.addObjectToScene(ptr);
            if (ptr.getRefData().getBaseNode() == nullptr)
                gFailed.insert(ptr.mRef);
        }
    }
}
