#include "hydration.hpp"

#include "../mwbase/environment.hpp"
#include "../mwworld/class.hpp"
#include "../mwbase/world.hpp"
#include "../mwworld/cell.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/livecellref.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/refdata.hpp"
#include "../mwworld/scene.hpp"

#include <components/esm/position.hpp>
#include <components/esm3/loadarmo.hpp>
#include <components/esm3/loadbody.hpp>
#include <components/esm3/loadclot.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadweap.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/sceneutil/workqueue.hpp>

#include <platform3ds/log.hpp>
#include <platform3ds/memprobe.hpp>

#include <3ds.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
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
        // Objects in range whose preload was still running: inserted once it's done (see kNearRadius, kMaxWaitMs).
        struct Pending
        {
            std::vector<std::shared_ptr<SceneUtil::WorkItem>> mItems;
            uint64_t mSince = 0;
        };
        std::unordered_map<const MWWorld::LiveCellRefBase*, Pending> gPending;
        // An object in range waits for its preload unless it is this close, or has waited this long (the work queue
        // is FIFO: after a cell grid change it holds every model of the preload ring).
        constexpr float kNearRadius = 1280.f;
        constexpr double kMaxWaitMs = 3000.0;
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

        // Npc::getModelsToPreload lists equipment only once the NPC's inventory exists, and OpenMW creates it when
        // the NPC is inserted: until then, the clothes, armor and weapons in its record (what it will most likely
        // wear) stand in. An Imperial Guard's armor alone is half a dozen models.
        void addRecordInventoryModels(const MWWorld::Ptr& ptr, std::vector<VFS::Path::NormalizedView>& models)
        {
            if (ptr.getType() != ESM::NPC::sRecordId || ptr.getRefData().getCustomData() != nullptr)
                return;
            const ESM::NPC* npc = ptr.get<ESM::NPC>()->mBase;
            const bool female = !npc->isMale();
            const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
            const auto addParts = [&](const std::vector<ESM::PartReference>& parts) {
                for (const ESM::PartReference& partRef : parts)
                {
                    // Same choice as Npc::getModelsToPreload.
                    const ESM::RefId& partname = (female && !partRef.mFemale.empty()) || (!female && partRef.mMale.empty())
                        ? partRef.mFemale
                        : partRef.mMale;
                    const ESM::BodyPart* part = store.get<ESM::BodyPart>().search(partname);
                    if (part != nullptr && !part->mModel.empty())
                        models.push_back(part->mModel.getNormalized());
                }
            };
            for (const ESM::ContItem& item : npc->mInventory.mList)
            {
                if (const ESM::Clothing* clothing = store.get<ESM::Clothing>().search(item.mItem))
                    addParts(clothing->mParts.mParts);
                else if (const ESM::Armor* armor = store.get<ESM::Armor>().search(item.mItem))
                    addParts(armor->mParts.mParts);
                else if (const ESM::Weapon* weapon = store.get<ESM::Weapon>().search(item.mItem))
                {
                    if (!weapon->mModel.empty())
                        models.push_back(weapon->mModel.getNormalized());
                }
            }
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
            gPending.clear();
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
            const float preloadRadius = radius + kPreloadMargin;
            if (!inScene && d2 < radius * radius && !gFailed.contains(ptr.mRef))
                toInsert.push_back(ptr);
            else if (!inScene && d2 < preloadRadius * preloadRadius)
            {
                if (!gPreloaded.contains(ptr.mRef))
                    toPreload.push_back(ptr);
            }
            else if (inScene && d2 > removeLimit)
                toRemove.push_back(ptr);
            else if (!inScene && gPreloaded.erase(ptr.mRef) != 0)
                gPending.erase(ptr.mRef); // left the ring: let the cache expire its models
            return true;
        });

        for (const MWWorld::Ptr& ptr : toRemove)
            scene.removeObjectFromScene(ptr);

        // Every model the object uses: an NPC is a dozen (body parts, equipment), ~0.3-0.5 s to load on the main
        // thread otherwise. Paths corrected as CellPreloader does.
        const uint64_t now = svcGetSystemTick();
        std::vector<VFS::Path::NormalizedView> models;
        const auto preload = [&](const MWWorld::Ptr& ptr) {
            gPreloaded.insert(ptr.mRef);
            models.clear();
            ptr.getClass().getModelsToPreload(ptr, models);
            addRecordInventoryModels(ptr, models);
            Pending pending;
            pending.mSince = now;
            // The first model is the object's own (the one physics uses): only it needs a collision shape.
            for (std::size_t i = 0; i < models.size(); ++i)
                if (auto item = scene.preload(
                        std::string(Misc::ResourceHelpers::correctMeshPath(models[i]).value()), true, i == 0))
                    pending.mItems.push_back(std::move(item));
            return pending;
        };
        for (const MWWorld::Ptr& ptr : toPreload)
        {
            Pending pending = preload(ptr);
            if (!pending.mItems.empty())
                gPending.emplace(ptr.mRef, std::move(pending));
        }

        // In range without a finished preload (it came within range before the preload ring saw it, e.g. its cell
        // just became active): preload first, insert when done.
        const auto ready = [&](const MWWorld::Ptr& ptr) {
            if (!gPreloaded.contains(ptr.mRef))
            {
                Pending pending = preload(ptr);
                if (!pending.mItems.empty())
                    gPending.emplace(ptr.mRef, std::move(pending));
            }
            const auto found = gPending.find(ptr.mRef);
            if (found == gPending.end())
                return true;
            const bool done = std::all_of(found->second.mItems.begin(), found->second.mItems.end(),
                [](const std::shared_ptr<SceneUtil::WorkItem>& item) { return item->isDone(); });
            const double waitedMs = double(now - found->second.mSince) * 1000.0 / SYSCLOCK_ARM11;
            const float d2 = distance2(ptr, center);
            if (!done && d2 > kNearRadius * kNearRadius && waitedMs < kMaxWaitMs)
                return false;
            if (!done)
                P3DS_LOGI("hydration: %s goes in before its preload finished (waited %.0f ms, %.0f away)",
                    ptr.getCellRef().getRefId().toDebugString().c_str(), waitedMs, std::sqrt(d2));
            // The entry (and the models it holds) goes once the object is in: a model held past the cache's 5 s
            // expiry is dropped at its next update once released, and the time budget can defer the insertion.
            return true;
        };
        toInsert.erase(std::remove_if(toInsert.begin(), toInsert.end(),
                           [&](const MWWorld::Ptr& ptr) { return !ready(ptr); }),
            toInsert.end());

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
            p3ds::perf::detail::setLogMisses(true); // names what the preload missed
            scene.addObjectToScene(ptr);
            p3ds::perf::detail::setLogMisses(false);
            gPending.erase(ptr.mRef);
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
