#include "worldmodel.hpp"

#include <algorithm>
#include <cassert>
#include <optional>
#include <stdexcept>
#ifdef OPENMW_PLATFORM_3DS
#include <sstream>
#endif

#include <components/debug/debuglog.hpp>
#include <components/esm/defs.hpp>
#include <components/esm3/cellid.hpp>
#include <components/esm3/cellref.hpp>
#include <components/esm3/cellstate.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/formatversion.hpp>
#include <components/esm3/loadregn.hpp>
#include <components/esm4/loadwrld.hpp>
#include <components/loadinglistener/loadinglistener.hpp>
#include <components/settings/values.hpp>

#include "cellstore.hpp"
#include "esmstore.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/luamanager.hpp"

namespace MWWorld
{
    namespace
    {
        const ESM::RefId draftCellId = ESM::RefId::index(ESM::REC_CSTA, 0);

        template <class T>
        CellStore& emplaceCellStore(ESM::RefId id, const T& cell, ESMStore& store, ESM::ReadersCache& readers,
            std::unordered_map<ESM::RefId, CellStore>& cells)
        {
            const auto [it, inserted] = cells.emplace(
                std::piecewise_construct, std::forward_as_tuple(id), std::forward_as_tuple(Cell(cell), store, readers));
            assert(inserted);
            return it->second;
        }

        CellStore* emplaceInteriorCellStore(std::string_view name, ESMStore& store, ESM::ReadersCache& readers,
            std::unordered_map<ESM::RefId, CellStore>& cells)
        {
            if (const ESM::Cell* cell = store.get<ESM::Cell>().search(name))
                return &emplaceCellStore(cell->mId, *cell, store, readers, cells);
            if (const ESM4::Cell* cell = store.get<ESM4::Cell>().searchCellName(name);
                cell != nullptr && !cell->isExterior())
            {
                return &emplaceCellStore(cell->mId, *cell, store, readers, cells);
            }
            return nullptr;
        }

        const ESM::Cell* createEsmCell(ESM::ExteriorCellLocation location, ESMStore& store)
        {
            ESM::Cell record = {};
            record.mData.mFlags = ESM::Cell::HasWater;
            record.mData.mX = location.mX;
            record.mData.mY = location.mY;
            record.updateId();
            return store.insert(record);
        }

        const ESM4::Cell* createEsm4Cell(ESM::ExteriorCellLocation location, ESMStore& store)
        {
            ESM4::Cell record = {};
            record.mParent = location.mWorldspace;
            record.mX = location.mX;
            record.mY = location.mY;
            return store.insert(record);
        }

        std::tuple<Cell, bool> createExteriorCell(ESM::ExteriorCellLocation location, ESMStore& store)
        {
            if (ESM::isEsm4Ext(location.mWorldspace))
            {
                if (store.get<ESM4::World>().search(location.mWorldspace) == nullptr)
                    throw std::runtime_error(
                        "Exterior ESM4 world is not found: " + location.mWorldspace.toDebugString());
                const ESM4::Cell* cell = store.get<ESM4::Cell>().searchExterior(location);
                bool created = cell == nullptr;
                if (created)
                    cell = createEsm4Cell(location, store);
                assert(cell != nullptr);
                return { MWWorld::Cell(*cell), created };
            }

            const ESM::Cell* cell = store.get<ESM::Cell>().search(location.mX, location.mY);
            bool created = cell == nullptr;
            if (created)
                cell = createEsmCell(location, store);
            assert(cell != nullptr);
            return { Cell(*cell), created };
        }

        std::optional<Cell> createCell(ESM::RefId id, const ESMStore& store)
        {
            if (const ESM4::Cell* cell = store.get<ESM4::Cell>().search(id))
                return Cell(*cell);
            if (const ESM::Cell* cell = store.get<ESM::Cell>().search(id))
                return Cell(*cell);
            return std::nullopt;
        }

        CellStore* getOrCreateExterior(const ESM::ExteriorCellLocation& location,
            std::map<ESM::ExteriorCellLocation, MWWorld::CellStore*>& exteriors, ESMStore& store,
            ESM::ReadersCache& readers, std::unordered_map<ESM::RefId, CellStore>& cells, bool triggerEvent)
        {
            if (const auto it = exteriors.find(location); it != exteriors.end())
            {
                assert(it->second != nullptr);
                return it->second;
            }
            auto [cell, created] = createExteriorCell(location, store);
            const ESM::RefId id = cell.getId();
            CellStore* const cellStore = &emplaceCellStore(id, std::move(cell), store, readers, cells);
            exteriors.emplace(location, cellStore);
            if (created && triggerEvent)
                MWBase::Environment::get().getLuaManager()->exteriorCreated(*cellStore);
            return cellStore;
        }

        ESM::RefId getWorldSpace(const ESM4::Cell& cell)
        {
            if (cell.mCellFlags & ESM4::CELL_Interior)
                return cell.mId;
            return cell.mParent;
        }
    }
}

MWWorld::CellStore& MWWorld::WorldModel::getOrInsertCellStore(const ESM::Cell& cell)
{
    const auto it = mCells.find(cell.mId);
    if (it != mCells.end())
        return it->second;
    return insertCellStore(cell);
}

MWWorld::CellStore& MWWorld::WorldModel::insertCellStore(const ESM::Cell& cell)
{
    CellStore& cellStore = emplaceCellStore(cell.mId, cell, mStore, mReaders, mCells);
    if (cell.mData.mFlags & ESM::Cell::Interior)
        mInteriors.emplace(cell.mName, &cellStore);
    else
        mExteriors.emplace(
            ESM::ExteriorCellLocation(cell.getGridX(), cell.getGridY(), ESM::Cell::sDefaultWorldspaceId), &cellStore);
    return cellStore;
}

MWWorld::CellStore& MWWorld::WorldModel::insertCellStore(const ESM4::Cell& cell)
{
    CellStore& cellStore = emplaceCellStore(cell.mId, cell, mStore, mReaders, mCells);
    if (cellStore.isExterior())
        mExteriors.emplace(cellStore.getCell()->getExteriorCellLocation(), &cellStore);
    else
        mInteriors.emplace(cellStore.getCell()->getNameId(), &cellStore);
    return cellStore;
}

void MWWorld::WorldModel::clear()
{
    mPtrRegistry.clear();
    mInteriors.clear();
    mExteriors.clear();
    mCells.clear();
    std::fill(mIdCache.begin(), mIdCache.end(), std::make_pair(ESM::RefId(), (MWWorld::CellStore*)nullptr));
    mIdCacheIndex = 0;
#ifdef OPENMW_PLATFORM_3DS
    mEvicted.clear();
    mCellUse.clear();
#endif
}

MWWorld::Ptr MWWorld::WorldModel::getPtrAndCache(const ESM::RefId& name, CellStore& cellStore)
{
    Ptr ptr = cellStore.getPtr(name);

    if (!ptr.isEmpty() && ptr.isInCell())
    {
        mIdCache[mIdCacheIndex].first = name;
        mIdCache[mIdCacheIndex].second = &cellStore;
        if (++mIdCacheIndex >= mIdCache.size())
            mIdCacheIndex = 0;
    }

    return ptr;
}

void MWWorld::WorldModel::writeCell(ESM::ESMWriter& writer, CellStore& cell) const
{
    if (cell.getState() != CellStore::State_Loaded)
        cell.load();

    ESM::CellState cellState;

    cell.saveState(cellState);

    writer.startRecord(ESM::REC_CSTA);

    writer.writeCellId(cellState.mId);
    cellState.save(writer);
    cell.writeFog(writer);
    cell.writeReferences(writer);
    writer.endRecord(ESM::REC_CSTA);
}

MWWorld::WorldModel::WorldModel(MWWorld::ESMStore& store, ESM::ReadersCache& readers)
    : mStore(store)
    , mReaders(readers)
    , mIdCache(Settings::cells().mPointersCacheSize, { ESM::RefId(), nullptr })
{
    mDraftCell.mId = draftCellId;
}

namespace MWWorld
{
    CellStore& WorldModel::getExterior(ESM::ExteriorCellLocation location, bool forceLoad) const
    {
        CellStore* cellStore = getOrCreateExterior(location, mExteriors, mStore, mReaders, mCells, true);

        if (forceLoad && cellStore->getState() != CellStore::State_Loaded)
            cellStore->load();

        return *cellStore;
    }

    CellStore* WorldModel::findInterior(std::string_view name, bool forceLoad) const
    {
        const auto it = mInteriors.find(name);
        CellStore* cellStore = nullptr;

        if (it == mInteriors.end())
        {
            cellStore = emplaceInteriorCellStore(name, mStore, mReaders, mCells);
            if (cellStore == nullptr)
                return cellStore;
            mInteriors.emplace(name, cellStore);
        }
        else
        {
            assert(it->second != nullptr);
            cellStore = it->second;
        }

        if (forceLoad && cellStore->getState() != CellStore::State_Loaded)
            cellStore->load();

        return cellStore;
    }

    CellStore& WorldModel::getInterior(std::string_view name, bool forceLoad) const
    {
        CellStore* const cellStore = findInterior(name, forceLoad);
        if (cellStore == nullptr)
            throw std::runtime_error("Interior cell is not found: '" + std::string(name) + "'");
        return *cellStore;
    }

    CellStore* WorldModel::findCell(ESM::RefId id, bool forceLoad) const
    {
        auto it = mCells.find(id);
        if (it != mCells.end())
        {
            CellStore& cellStore = it->second;
            if (forceLoad && cellStore.getState() != CellStore::State_Loaded)
                cellStore.load();
            return &cellStore;
        }

        if (id == draftCellId)
        {
            CellStore& cellStore = emplaceCellStore(id, Cell(mDraftCell), mStore, mReaders, mCells);
            cellStore.load();
            return &cellStore;
        }

        if (const auto* exteriorId = id.getIf<ESM::ESM3ExteriorCellRefId>())
            return &getExterior(
                ESM::ExteriorCellLocation(exteriorId->getX(), exteriorId->getY(), ESM::Cell::sDefaultWorldspaceId),
                forceLoad);

        std::optional<Cell> cell = createCell(id, mStore);
        if (!cell.has_value())
            return nullptr;

        CellStore& cellStore = emplaceCellStore(id, std::move(*cell), mStore, mReaders, mCells);

        if (cellStore.isExterior())
            mExteriors.emplace(ESM::ExteriorCellLocation(cellStore.getCell()->getGridX(),
                                   cellStore.getCell()->getGridY(), cellStore.getCell()->getWorldSpace()),
                &cellStore);
        else
            mInteriors.emplace(cellStore.getCell()->getNameId(), &cellStore);

        if (forceLoad && cellStore.getState() != CellStore::State_Loaded)
            cellStore.load();

        return &cellStore;
    }

    CellStore& WorldModel::getCell(ESM::RefId id, bool forceLoad) const
    {
        CellStore* const result = findCell(id, forceLoad);
        if (result == nullptr)
            throw std::runtime_error("Cell does not exist: " + id.toDebugString());
        return *result;
    }

    CellStore& WorldModel::getDraftCell() const
    {
        return getCell(draftCellId);
    }

    CellStore* WorldModel::findCell(std::string_view name, bool forceLoad) const
    {
        if (CellStore* const cellStore = findInterior(name, forceLoad))
            return cellStore;

        // try named exteriors
        const ESM::Cell* cell = nullptr;
        const Store<ESM::Cell>& cells = mStore.get<ESM::Cell>();
        const Store<ESM::GameSetting>& gmsts = mStore.get<ESM::GameSetting>();
        const Store<ESM::Region>& regions = mStore.get<ESM::Region>();
        static const std::string& defaultName = gmsts.find("sDefaultCellname")->mValue.getString();

        for (auto it = cells.extBegin(); it != cells.extEnd(); ++it)
        {
            std::string_view resolvedName = defaultName;
            if (!it->mName.empty())
                resolvedName = it->mName;
            else if (!it->mRegion.empty())
            {
                const ESM::Region* region = regions.search(it->mRegion);
                if (region != nullptr)
                    resolvedName = !region->mName.empty() ? region->mName : region->mId.getRefIdString();
            }

            if (Misc::StringUtils::ciEqual(resolvedName, name))
            {
                cell = &(*it);
                break;
            }
        }

        if (cell != nullptr)
            return &getExterior(
                ESM::ExteriorCellLocation(cell->getGridX(), cell->getGridY(), ESM::Cell::sDefaultWorldspaceId),
                forceLoad);

        if (const ESM4::Cell* cell4 = mStore.get<ESM4::Cell>().searchCellName(name);
            cell4 != nullptr && cell4->isExterior())
        {
            return &getExterior(cell4->getExteriorCellLocation(), forceLoad);
        }

        return nullptr;
    }

    CellStore& WorldModel::getCell(std::string_view name, bool forceLoad) const
    {
        CellStore* const result = findCell(name, forceLoad);
        if (result == nullptr)
            throw std::runtime_error(std::string("Can't find cell with name ") + std::string(name));
        return *result;
    }

    void WorldModel::registerPtr(const Ptr& ptr)
    {
        if (ptr.mRef == nullptr)
            throw std::logic_error("Ptr with nullptr mRef is not allowed to be registered");
        mPtrRegistry.insert(ptr);
        ptr.mRef->mWorldModel = this;
    }

    void WorldModel::deregisterLiveCellRef(LiveCellRefBase& ref) noexcept
    {
        mPtrRegistry.remove(ref);
        ref.mWorldModel = nullptr;
    }
}

MWWorld::Ptr MWWorld::WorldModel::getPtrByRefId(const ESM::RefId& name)
{
    for (const auto& [cachedId, cellStore] : mIdCache)
    {
        if (cachedId != name || cellStore == nullptr)
            continue;
        Ptr ptr = cellStore->getPtr(name);
        if (!ptr.isEmpty())
            return ptr;
    }

    // Then check cells that are already listed
    // Search in reverse, this is a workaround for an ambiguous chargen_plank reference in the vanilla game.
    // there is one at -22,16 and one at -2,-9, the latter should be used.
    for (auto iter = mExteriors.rbegin(); iter != mExteriors.rend(); ++iter)
    {
        Ptr ptr = getPtrAndCache(name, *iter->second);
        if (!ptr.isEmpty())
            return ptr;
    }

    for (auto iter = mInteriors.begin(); iter != mInteriors.end(); ++iter)
    {
        Ptr ptr = getPtrAndCache(name, *iter->second);
        if (!ptr.isEmpty())
            return ptr;
    }

    // Now try the other cells
    for (const ESM::Cell* cell : mStore.getRefCells(name))
    {
        if (mCells.contains(cell->mId))
            continue;

        Ptr ptr = getPtrAndCache(name, insertCellStore(*cell));

        if (!ptr.isEmpty())
            return ptr;
    }

    // giving up
    return Ptr();
}

void MWWorld::WorldModel::getExteriorPtrs(const ESM::RefId& name, std::vector<MWWorld::Ptr>& out)
{
    const MWWorld::Store<ESM::Cell>& cells = mStore.get<ESM::Cell>();
    for (MWWorld::Store<ESM::Cell>::iterator iter = cells.extBegin(); iter != cells.extEnd(); ++iter)
    {
        CellStore& cellStore = getOrInsertCellStore(*iter);

        Ptr ptr = getPtrAndCache(name, cellStore);

        if (!ptr.isEmpty())
            out.push_back(ptr);
    }
}

std::vector<MWWorld::Ptr> MWWorld::WorldModel::getAll(ESM::RefId id, ESM::RefId worldSpace, bool searchUnloaded)
{
    std::vector<Ptr> result;
    const auto addFromStore = [&](MWWorld::CellStore& cellStore) {
        if (!worldSpace.empty() && cellStore.getCell()->getWorldSpace() != worldSpace)
            return;
        if (cellStore.getState() == CellStore::State_Unloaded)
            cellStore.preload();
        if (cellStore.getState() == CellStore::State_Preloaded)
        {
            if (!cellStore.hasId(id))
                return;
            cellStore.load();
        }
        cellStore.forEach([&](const Ptr& ptr) {
            if (ptr.getCellRef().getRefId() == id)
                result.push_back(ptr);
            return true;
        });
    };
    for (auto& [cellId, cellStore] : mCells)
        addFromStore(cellStore);

    if (!searchUnloaded)
        return result;

    const MWWorld::Store<ESM::Cell>& cells = mStore.get<ESM::Cell>();
    if (worldSpace.empty() || worldSpace == ESM::Cell::sDefaultWorldspaceId)
    {
        for (auto iter = cells.extBegin(); iter != cells.extEnd(); ++iter)
        {
            if (mCells.contains(iter->mId))
                continue;
            addFromStore(insertCellStore(*iter));
        }
    }
    if (worldSpace.empty())
    {
        for (auto iter = cells.intBegin(); iter != cells.intEnd(); ++iter)
        {
            if (mCells.contains(iter->mId))
                continue;
            addFromStore(insertCellStore(*iter));
        }
    }
    else if (worldSpace.is<ESM::StringRefId>() && !mCells.contains(worldSpace))
    {
        if (const ESM::Cell* cell = cells.search(worldSpace); cell != nullptr)
            addFromStore(insertCellStore(*cell));
    }
    if (worldSpace.empty() || worldSpace.is<ESM::FormId>())
    {
        for (const ESM4::Cell& cell : mStore.get<ESM4::Cell>())
        {
            if (mCells.contains(cell.mId))
                continue;
            if (worldSpace.empty() || getWorldSpace(cell) == worldSpace)
                addFromStore(insertCellStore(cell));
        }
    }
    return result;
}

size_t MWWorld::WorldModel::countSavedGameRecords() const
{
    return std::count_if(mCells.begin(), mCells.end(), [](const auto& v) { return v.second.hasState(); })
#ifdef OPENMW_PLATFORM_3DS
        + mEvicted.size()
#endif
        ;
}

void MWWorld::WorldModel::write(ESM::ESMWriter& writer, Loading::Listener& progress) const
{
    for (auto& [id, cellStore] : mCells)
        if (cellStore.hasState())
        {
            writeCell(writer, cellStore);
            progress.increaseProgress();
        }
#ifdef OPENMW_PLATFORM_3DS
    // Evicted cells: their record as it was written (outside any open record, so no size bookkeeping).
    for (const auto& [id, record] : mEvicted)
    {
        writer.write(record.data(), record.size());
        progress.increaseProgress();
    }
#endif
}

#ifdef OPENMW_PLATFORM_3DS
void MWWorld::WorldModel::touchCell(const CellStore& cell)
{
    mCellUse[cell.getCell()->getId()] = ++mCellUseClock;
}

std::size_t MWWorld::WorldModel::evictedBytes() const
{
    std::size_t bytes = 0;
    for (const auto& [id, record] : mEvicted)
        bytes += record.size();
    return bytes;
}

std::size_t MWWorld::WorldModel::evictCells(const std::function<bool(const CellStore&)>& pinned, std::size_t maxKept)
{
    std::vector<std::pair<uint64_t, CellStore*>> candidates;
    for (auto& [id, cellStore] : mCells)
    {
        if (cellStore.getState() != CellStore::State_Loaded || cellStore.getCell()->isEsm4()
            || cellStore.hasMovedRefs() || pinned(cellStore))
            continue;
        const auto use = mCellUse.find(id);
        candidates.emplace_back(use == mCellUse.end() ? 0 : use->second, &cellStore);
    }
    if (candidates.size() <= maxKept)
        return 0;
    std::sort(candidates.begin(), candidates.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    candidates.resize(candidates.size() - maxKept);

    std::size_t evicted = 0;
    for (const auto& [use, cellStore] : candidates)
    {
        const ESM::RefId id = cellStore->getCell()->getId();
        if (cellStore->hasState())
        {
            try
            {
                ESM::ESMWriter writer;
                writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
                writer.setVersion(0);
                writer.setType(0);
                writer.setAuthor("");
                writer.setDescription("");
                std::ostringstream stream;
                writer.save(stream);
                const std::size_t headerEnd = static_cast<std::size_t>(stream.tellp());
                writeCell(writer, *cellStore);
                writer.close();
                std::string bytes = stream.str();
                if (mEvictHeader.empty())
                    mEvictHeader = bytes.substr(0, headerEnd);
                mEvicted[id] = bytes.substr(headerEnd);
#ifdef OPENMW3DS_DEBUG
                mEvictedSignature[id] = signature(*cellStore);
#endif
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "Cell eviction: could not write " << id << ", kept: " << e.what();
                mEvicted.erase(id);
                continue;
            }
        }
        Log(Debug::Info) << "Cell eviction: " << cellStore->getCell()->getDescription()
                         << (mEvicted.contains(id) ? " written and freed" : " freed (unchanged)");
        if (cellStore->isExterior())
            mExteriors.erase(cellStore->getCell()->getExteriorCellLocation());
        else
            mInteriors.erase(std::string(cellStore->getCell()->getNameId()));
        for (auto& entry : mIdCache)
            if (entry.second == cellStore)
                entry = { ESM::RefId(), nullptr };
        mCellUse.erase(id);
        mCells.erase(id); // its references leave the Ptr registry as they are destroyed
        ++evicted;
    }
    return evicted;
}

#ifdef OPENMW3DS_DEBUG
uint64_t MWWorld::WorldModel::signature(CellStore& cell)
{
    uint64_t sum = 0;
    std::size_t refs = 0;
    cell.forEach(
        [&](const Ptr& ptr) {
            const ESM::Position& pos = ptr.getRefData().getPosition();
            sum = sum * 1099511628211ull + uint64_t(ptr.getCellRef().getRefNum().mIndex)
                + uint64_t(ptr.getCellRef().getCount(false)) * 7 + (ptr.getRefData().isEnabled() ? 1 : 0)
                + uint64_t(int64_t(pos.pos[0] * 4) + int64_t(pos.pos[1] * 4) * 3 + int64_t(pos.pos[2] * 4) * 5);
            ++refs;
            return true;
        },
        true);
    return sum ^ (uint64_t(refs) << 48);
}

bool MWWorld::WorldModel::selfTestEviction(std::size_t count)
{
    bool ok = true;
    std::vector<ESM::RefId> ids;
    for (const auto& [id, record] : mEvicted)
        if (ids.size() < count)
            ids.push_back(id);
    for (const ESM::RefId& id : ids)
    {
        const uint64_t want = mEvictedSignature[id];
        CellStore* cell = findCell(id, true);
        const uint64_t got = cell != nullptr ? signature(*cell) : 0;
        const bool same = got == want;
        ok = ok && same;
        Log(same ? Debug::Info : Debug::Error)
            << "Cell eviction self-test: " << (cell != nullptr ? cell->getCell()->getDescription() : id.toDebugString())
            << (same ? " read back identical" : " DIFFERS after reading back");
    }
    return ok;
}
#endif

void MWWorld::WorldModel::restoreEvicted(CellStore& cell)
{
    const auto it = mEvicted.find(cell.getCell()->getId());
    if (it == mEvicted.end())
        return;
    std::string record = std::move(it->second);
    mEvicted.erase(it);
    try
    {
        ESM::ESMReader reader;
        reader.open(std::make_unique<std::istringstream>(mEvictHeader + record), "evicted cell");
        if (reader.hasMoreRecs())
        {
            const ESM::NAME name = reader.getRecName();
            reader.getRecHeader();
            readRecord(reader, name.toInt());
        }
        Log(Debug::Info) << "Cell eviction: " << cell.getCell()->getDescription() << " read back (" << record.size()
                         << " bytes of changes)";
    }
    catch (const std::exception& e)
    {
        Log(Debug::Error) << "Cell eviction: could not read back " << cell.getCell()->getId() << ": " << e.what();
    }
}
#endif

struct MWWorld::WorldModel::GetCellStoreCallback : public CellStore::GetCellStoreCallback
{
public:
    GetCellStoreCallback(WorldModel& worldModel)
        : mWorldModel(worldModel)
    {
    }

    WorldModel& mWorldModel;

    CellStore* getCellStore(const ESM::RefId& cellId) override
    {
        if (const auto* exteriorId = cellId.getIf<ESM::ESM3ExteriorCellRefId>())
        {
            ESM::ExteriorCellLocation location(exteriorId->getX(), exteriorId->getY(), ESM::Cell::sDefaultWorldspaceId);
            return getOrCreateExterior(
                location, mWorldModel.mExteriors, mWorldModel.mStore, mWorldModel.mReaders, mWorldModel.mCells, false);
        }
        return mWorldModel.findCell(cellId);
    }
};

bool MWWorld::WorldModel::readRecord(ESM::ESMReader& reader, uint32_t type)
{
    if (type == ESM::REC_CSTA)
    {
        ESM::CellState state;
        state.mId = reader.getCellId();

        GetCellStoreCallback callback(*this);

        CellStore* const cellStore = callback.getCellStore(state.mId);

        if (cellStore == nullptr)
        {
            Log(Debug::Warning) << "Dropping state for cell " << state.mId << " (cell no longer exists)";
            reader.skipRecord();
            return true;
        }

        state.load(reader);
        cellStore->loadState(state);

        if (state.mHasFogOfWar)
            cellStore->readFog(reader);

        if (cellStore->getState() != CellStore::State_Loaded)
            cellStore->load();

        cellStore->readReferences(reader, &callback);

        return true;
    }

    return false;
}
