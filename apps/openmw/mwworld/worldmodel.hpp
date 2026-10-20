#ifndef GAME_MWWORLD_WORLDMODEL_H
#define GAME_MWWORLD_WORLDMODEL_H

#include <functional>
#include <list>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>

#include <components/esm/exteriorcelllocation.hpp>
#include <components/misc/algorithm.hpp>

#include "cellstore.hpp"
#include "ptr.hpp"
#include "ptrregistry.hpp"

namespace ESM
{
    class ESMReader;
    class ESMWriter;
    class ReadersCache;
    struct Cell;
}

namespace ESM4
{
    struct Cell;
}

namespace Loading
{
    class Listener;
}

namespace MWWorld
{
    class ESMStore;

    /// \brief Cell container
    class WorldModel
    {
    public:
        explicit WorldModel(ESMStore& store, ESM::ReadersCache& reader);

        WorldModel(const WorldModel&) = delete;
        WorldModel& operator=(const WorldModel&) = delete;

        void clear();

        CellStore& getExterior(ESM::ExteriorCellLocation location, bool forceLoad = true) const;

        CellStore* findCell(ESM::RefId id, bool forceLoad = true) const;

        CellStore& getCell(ESM::RefId id, bool forceLoad = true) const;

        // Returns a special cell that is never active. Can be used for creating objects
        // without adding them to the scene.
        CellStore& getDraftCell() const;

        CellStore* findInterior(std::string_view name, bool forceLoad = true) const;

        CellStore& getInterior(std::string_view name, bool forceLoad = true) const;

        CellStore* findCell(std::string_view name, bool forceLoad = true) const;

        CellStore& getCell(std::string_view name, bool forceLoad = true) const;

        Ptr getPtrByRefId(const ESM::RefId& name);

        Ptr getPtr(ESM::RefNum refNum) const { return mPtrRegistry.getOrEmpty(refNum); }

        PtrRegistryView getPtrRegistryView() const { return PtrRegistryView(mPtrRegistry); }

        ESM::RefNum getLastGeneratedRefNum() const { return mPtrRegistry.getLastGenerated(); }

        void setLastGeneratedRefNum(ESM::RefNum v) { mPtrRegistry.setLastGenerated(v); }

        std::size_t getPtrRegistryRevision() const { return mPtrRegistry.getRevision(); }

        void registerPtr(const Ptr& ptr);

        void deregisterLiveCellRef(LiveCellRefBase& ref) noexcept;

        void assignSaveFileRefNum(ESM::CellRef& ref) { mPtrRegistry.assign(ref); }

        template <typename Fn>
        void forEachLoadedCellStore(Fn&& fn)
        {
            for (auto& [_, store] : mCells)
                fn(store);
        }

        /// Get all Ptrs referencing \a name in exterior cells
        /// @note Due to the current implementation of getPtr this only supports one Ptr per cell.
        /// @note name must be lower case
        void getExteriorPtrs(const ESM::RefId& name, std::vector<MWWorld::Ptr>& out);

        std::vector<MWWorld::Ptr> getAll(ESM::RefId id, ESM::RefId worldSpace = {}, bool searchUnloaded = false);

        size_t countSavedGameRecords() const;

        void write(ESM::ESMWriter& writer, Loading::Listener& progress) const;

        bool readRecord(ESM::ESMReader& reader, uint32_t type);

#ifdef OPENMW_PLATFORM_3DS
        // M9: OpenMW keeps every cell the player visited, whole, until a new game or a load. Here the cells left
        // longest ago are written as a save writes them (only what changed, one in-memory REC_CSTA record each),
        // freed, and read back on top of the content files when something needs them again (CellStore::load).
        void touchCell(const CellStore& cell); ///< The scene loaded it: most recently used.
        /// Evicts the least recently used loaded cells beyond @a maxKept that @a pinned doesn't keep. Returns how
        /// many were evicted.
        std::size_t evictCells(const std::function<bool(const CellStore&)>& pinned, std::size_t maxKept);
        bool isEvicted(const ESM::RefId& id) const { return mEvicted.contains(id); }
        void restoreEvicted(CellStore& cell);
        std::size_t evictedCount() const { return mEvicted.size(); }
        std::size_t evictedBytes() const;
#ifdef OPENMW3DS_DEBUG
        /// Debug: reads up to @a count cells evicted with changes back and compares their references (count,
        /// enabled, position) with what they were when written. Returns false on a mismatch.
        bool selfTestEviction(std::size_t count);
#endif
#endif

    private:
        struct GetCellStoreCallback;

        PtrRegistry mPtrRegistry; // defined before mCells because during destruction it should be the last

        MWWorld::ESMStore& mStore;
        ESM::ReadersCache& mReaders;
        mutable std::unordered_map<ESM::RefId, CellStore> mCells;
        mutable std::map<std::string, CellStore*, Misc::StringUtils::CiComp> mInteriors;
        mutable std::map<ESM::ExteriorCellLocation, CellStore*> mExteriors;
        ESM::Cell mDraftCell;
        std::vector<std::pair<ESM::RefId, CellStore*>> mIdCache;
        std::size_t mIdCacheIndex = 0;
#ifdef OPENMW_PLATFORM_3DS
        std::unordered_map<ESM::RefId, std::string> mEvicted; // REC_CSTA records
        std::string mEvictHeader; // the TES3 header the records are read back with
        std::unordered_map<ESM::RefId, uint64_t> mCellUse;
        uint64_t mCellUseClock = 0;
#ifdef OPENMW3DS_DEBUG
        std::unordered_map<ESM::RefId, uint64_t> mEvictedSignature;
        static uint64_t signature(CellStore& cell);
#endif
#endif

        CellStore& getOrInsertCellStore(const ESM::Cell& cell);

        CellStore& insertCellStore(const ESM::Cell& cell);
        CellStore& insertCellStore(const ESM4::Cell& cell);

        Ptr getPtrAndCache(const ESM::RefId& name, CellStore& cellStore);

        void writeCell(ESM::ESMWriter& writer, CellStore& cell) const;
    };
}

#endif
