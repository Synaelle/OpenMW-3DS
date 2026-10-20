#include "esmstore.hpp"

#include <algorithm>
#include <fstream>
#include <tuple>

#include <components/debug/debuglog.hpp>

#include <components/esm/records.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/readerscache.hpp>
#include <components/esm4/common.hpp>
#include <components/esm4/reader.hpp>
#include <components/esm4/readerutils.hpp>
#include <components/esmloader/load.hpp>
#include <components/loadinglistener/loadinglistener.hpp>
#include <components/lua/configuration.hpp>
#include <components/misc/algorithm.hpp>

#include "../mwmechanics/spelllist.hpp"

#ifdef OPENMW_PLATFORM_3DS
#include <cstdio>
#include <cstring>

#include <platform3ds/crash.hpp>
#include <platform3ds/memprobe.hpp>
#endif

namespace
{
#ifdef OPENMW_PLATFORM_3DS
    // Counting the references re-reads every cell's references from the SD card (12 s on hardware, at every start).
    // The result depends only on the content files: it is kept in this file, keyed by their names and sizes.
    constexpr char sRefInfoCachePath[] = "sdmc:/openmw/cache/refinfo.bin";
    constexpr char sRefInfoCacheMagic[4] = { 'O', '3', 'R', 'I' };
    constexpr int sRefInfoCacheVersion = 1;

    void appendU32(std::string& out, std::uint32_t value)
    {
        out.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    void appendString(std::string& out, std::string_view value)
    {
        appendU32(out, static_cast<std::uint32_t>(value.size()));
        out.append(value);
    }

    struct CacheReader
    {
        const char* mPos;
        const char* mEnd;
        bool mOk = true;

        std::uint32_t u32()
        {
            std::uint32_t value = 0;
            if (mEnd - mPos < static_cast<std::ptrdiff_t>(sizeof(value)))
                mOk = false;
            else
            {
                std::memcpy(&value, mPos, sizeof(value));
                mPos += sizeof(value);
            }
            return value;
        }

        std::string_view string()
        {
            const std::uint32_t size = u32();
            if (!mOk || static_cast<std::uint32_t>(mEnd - mPos) < size)
            {
                mOk = false;
                return {};
            }
            const std::string_view value(mPos, size);
            mPos += size;
            return value;
        }

        bool magic()
        {
            if (mEnd - mPos < 4 || std::memcmp(mPos, sRefInfoCacheMagic, 4) != 0)
                return mOk = false;
            mPos += 4;
            return mOk;
        }
    };
#endif

    struct Ref
    {
        ESM::RefNum mRefNum;
        std::size_t mRefID;
        std::size_t mCellIndex;

        Ref(ESM::RefNum refNum, std::size_t refID, std::size_t cellIndex)
            : mRefNum(refNum)
            , mRefID(refID)
            , mCellIndex(cellIndex)
        {
        }
    };

    constexpr std::size_t deletedRefID = std::numeric_limits<std::size_t>::max();

    void readRefs(const ESM::Cell& cell, std::size_t cellIndex, std::vector<Ref>& refs, std::vector<ESM::RefId>& refIDs,
        std::set<ESM::RefId>& keyIDs, ESM::ReadersCache& readers)
    {
        // TODO: we have many similar copies of this code.
        for (size_t i = 0; i < cell.mContextList.size(); i++)
        {
            const std::size_t index = static_cast<std::size_t>(cell.mContextList[i].index);
            const ESM::ReadersCache::BusyItem reader = readers.get(index);
            cell.restore(*reader, i);
            ESM::CellRef ref;
            bool deleted = false;
            while (cell.getNextRef(*reader, ref, deleted))
            {
                if (deleted)
                    refs.emplace_back(ref.mRefNum, deletedRefID, cellIndex);
                else if (std::find(cell.mMovedRefs.begin(), cell.mMovedRefs.end(), ref.mRefNum)
                    == cell.mMovedRefs.end())
                {
                    if (!ref.mKey.empty())
                        keyIDs.insert(std::move(ref.mKey));
                    refs.emplace_back(ref.mRefNum, refIDs.size(), cellIndex);
                    refIDs.push_back(std::move(ref.mRefID));
                }
            }
        }
        for (const auto& [value, deleted] : cell.mLeasedRefs)
        {
            if (deleted)
                refs.emplace_back(value.mRefNum, deletedRefID, cellIndex);
            else
            {
                if (!value.mKey.empty())
                    keyIDs.insert(std::move(value.mKey));
                refs.emplace_back(value.mRefNum, refIDs.size(), cellIndex);
                refIDs.push_back(value.mRefID);
            }
        }
    }

    const ESM::RefId& getDefaultClass(const MWWorld::Store<ESM::Class>& classes)
    {
        auto it = classes.begin();
        if (it != classes.end())
            return it->mId;
        throw std::runtime_error("List of NPC classes is empty!");
    }

    const ESM::RefId& getDefaultRace(const MWWorld::Store<ESM::Race>& races)
    {
        auto it = races.begin();
        if (it != races.end())
            return it->mId;
        throw std::runtime_error("List of NPC races is empty!");
    }

    std::vector<ESM::NPC> getNPCsToReplace(const MWWorld::Store<ESM::Faction>& factions,
        const MWWorld::Store<ESM::Class>& classes, const MWWorld::Store<ESM::Race>& races,
        const MWWorld::Store<ESM::Script>& scripts, const std::unordered_map<ESM::RefId, ESM::NPC>& npcs)
    {
        // Cache first class from store - we will use it if current class is not found
        const ESM::RefId& defaultCls = getDefaultClass(classes);
        // Same for races
        const ESM::RefId& defaultRace = getDefaultRace(races);

        // Validate NPCs for non-existing class and faction.
        // We will replace invalid entries by fixed ones
        std::vector<ESM::NPC> npcsToReplace;

        for (const auto& npcIter : npcs)
        {
            ESM::NPC npc = npcIter.second;
            bool changed = false;

            const ESM::RefId& npcFaction = npc.mFaction;
            if (!npcFaction.empty())
            {
                const ESM::Faction* fact = factions.search(npcFaction);
                if (!fact)
                {
                    Log(Debug::Verbose) << "NPC " << npc.mId << " (" << npc.mName << ") has nonexistent faction "
                                        << npc.mFaction << ", ignoring it.";
                    npc.mFaction = ESM::RefId();
                    npc.mNpdt.mRank = 0;
                    changed = true;
                }
            }

            const ESM::Class* cls = classes.search(npc.mClass);
            if (!cls)
            {
                Log(Debug::Verbose) << "NPC " << npc.mId << " (" << npc.mName << ") has nonexistent class "
                                    << npc.mClass << ", using " << defaultCls << " class as replacement.";
                npc.mClass = defaultCls;
                changed = true;
            }

            const ESM::Race* race = races.search(npc.mRace);
            if (!race)
            {
                Log(Debug::Verbose) << "NPC " << npc.mId << " (" << npc.mName << ") has nonexistent race " << npc.mRace
                                    << ", using " << defaultRace << " race as replacement.";
                npc.mRace = defaultRace;
                changed = true;
            }

            if (!npc.mScript.empty() && !scripts.search(npc.mScript))
            {
                Log(Debug::Verbose) << "NPC " << npc.mId << " (" << npc.mName << ") has nonexistent script "
                                    << npc.mScript << ", ignoring it.";
                npc.mScript = ESM::RefId();
                changed = true;
            }

            if (changed)
                npcsToReplace.push_back(std::move(npc));
        }

        return npcsToReplace;
    }

    template <class RecordType>
    std::vector<RecordType> getSpellsToReplace(
        const MWWorld::Store<RecordType>& spells, const MWWorld::Store<ESM::MagicEffect>& magicEffects)
    {
        std::vector<RecordType> spellsToReplace;

        for (RecordType spell : spells)
        {
            if (spell.mEffects.mList.empty())
                continue;

            bool changed = false;
            auto iter = spell.mEffects.mList.begin();
            while (iter != spell.mEffects.mList.end())
            {
                const ESM::MagicEffect* mgef = magicEffects.search(iter->mData.mEffectID);
                if (!mgef)
                {
                    Log(Debug::Verbose) << RecordType::getRecordType() << " " << spell.mId
                                        << ": dropping invalid effect (" << iter->mData.mEffectID << ")";
                    iter = spell.mEffects.mList.erase(iter);
                    changed = true;
                    continue;
                }

                if (!(mgef->mData.mFlags & ESM::MagicEffect::TargetAttribute) && !iter->mData.mAttribute.empty())
                {
                    iter->mData.mAttribute = ESM::RefId();
                    Log(Debug::Verbose) << RecordType::getRecordType() << " " << spell.mId
                                        << ": dropping unexpected attribute argument of " << iter->mData.mEffectID
                                        << " effect";
                    changed = true;
                }

                if (!(mgef->mData.mFlags & ESM::MagicEffect::TargetSkill) && !iter->mData.mSkill.empty())
                {
                    iter->mData.mSkill = ESM::RefId();
                    Log(Debug::Verbose) << RecordType::getRecordType() << " " << spell.mId
                                        << ": dropping unexpected skill argument of " << iter->mData.mEffectID
                                        << " effect";
                    changed = true;
                }

                ++iter;
            }

            if (changed)
                spellsToReplace.emplace_back(spell);
        }

        return spellsToReplace;
    }

    // Custom enchanted items can reference scripts that no longer exist, this doesn't necessarily mean the base item no
    // longer exists however. So instead of removing the item altogether, we're only removing the script.
    template <class MapT>
    void removeMissingScripts(const MWWorld::Store<ESM::Script>& scripts, MapT& items)
    {
        for (auto& [id, item] : items)
        {
            if (!item.mScript.empty() && !scripts.search(item.mScript))
            {
                Log(Debug::Verbose) << MapT::mapped_type::getRecordType() << ' ' << id << " (" << item.mName
                                    << ") has nonexistent script " << item.mScript << ", ignoring it.";
                item.mScript = ESM::RefId();
            }
        }
    }
}

namespace MWWorld
{
    using IDMap = std::unordered_map<ESM::RefId, int>;

    struct ESMStoreImp
    {
        ESMStore::StoreTuple mStores;

        std::map<ESM::RecNameInts, DynamicStore*> mRecNameToStore;

        // Lookup of all IDs. Makes looking up references faster. Just
        // maps the id name to the record type.
        IDMap mIds;
        IDMap mStaticIds;

        template <typename T>
        static void assignStoreToIndex(ESMStore& stores, Store<T>& store)
        {
            const std::size_t storeIndex = ESMStore::getTypeIndex<T>();
            if (stores.mStores.size() <= storeIndex)
                stores.mStores.resize(storeIndex + 1);

            assert(&store == &std::get<Store<T>>(stores.mStoreImp->mStores));

            stores.mStores[storeIndex] = &store;
            if constexpr (std::is_convertible_v<Store<T>*, DynamicStore*>)
            {
                stores.mDynamicStores.push_back(&store);
                constexpr ESM::RecNameInts recName = T::sRecordId;
                if constexpr (recName != ESM::REC_INTERNAL_PLAYER)
                {
                    stores.mStoreImp->mRecNameToStore[recName] = &store;
                }
            }
        }

        template <typename T>
        static bool typedReadRecordESM4(ESM4::Reader& reader, Store<T>& store)
        {
            auto recordType = static_cast<ESM4::RecordTypes>(reader.hdr().record.typeId);

            ESM::RecNameInts esm4RecName = static_cast<ESM::RecNameInts>(ESM::esm4Recname(recordType));
            if constexpr (HasRecordId<T>)
            {
                if constexpr (ESM::isESM4Rec(T::sRecordId))
                {
                    if (T::sRecordId == esm4RecName)
                    {
                        reader.getRecordData();
                        T value;
                        value.load(reader);
                        store.insertStatic(value);
                        return true;
                    }
                }
            }
            return false;
        }

        static bool readRecord(ESM4::Reader& reader, ESMStore& store)
        {
            return std::apply(
                [&reader](auto&... x) { return (typedReadRecordESM4(reader, x) || ...); }, store.mStoreImp->mStores);
        }
    };

    int ESMStore::find(const ESM::RefId& id) const
    {
        IDMap::const_iterator it = mStoreImp->mIds.find(id);
        if (it == mStoreImp->mIds.end())
        {
#ifdef OPENMW_PLATFORM_3DS
            return findStatic(id);
#else
            return 0;
#endif
        }
        return it->second;
    }

    int ESMStore::findStatic(const ESM::RefId& id) const
    {
        IDMap::const_iterator it = mStoreImp->mStaticIds.find(id);
        if (it == mStoreImp->mStaticIds.end())
        {
            return 0;
        }
        return it->second;
    }

    ESMStore::ESMStore()
    {
        mStoreImp = std::make_unique<ESMStoreImp>();
        std::apply([this](auto&... x) { (ESMStoreImp::assignStoreToIndex(*this, x), ...); }, mStoreImp->mStores);
        mDynamicCount = 0;
        getWritable<ESM::Pathgrid>().setCells(getWritable<ESM::Cell>());
    }

    ESMStore::~ESMStore() = default;

    void ESMStore::clearDynamic()
    {
        for (const auto& store : mDynamicStores)
            store->clearDynamic();
#ifdef OPENMW_PLATFORM_3DS
        mStoreImp->mIds.clear();
#else
        mStoreImp->mIds = mStoreImp->mStaticIds;
#endif

        movePlayerRecord();
    }

    static bool isCacheableRecord(int id)
    {
        switch (id)
        {
            case ESM::REC_ACTI:
            case ESM::REC_ALCH:
            case ESM::REC_APPA:
            case ESM::REC_ARMO:
            case ESM::REC_BOOK:
            case ESM::REC_CLOT:
            case ESM::REC_CONT:
            case ESM::REC_CREA:
            case ESM::REC_DOOR:
            case ESM::REC_INGR:
            case ESM::REC_LEVC:
            case ESM::REC_LEVI:
            case ESM::REC_LIGH:
            case ESM::REC_LOCK:
            case ESM::REC_MISC:
            case ESM::REC_NPC_:
            case ESM::REC_PROB:
            case ESM::REC_REPA:
            case ESM::REC_STAT:
            case ESM::REC_WEAP:
            case ESM::REC_BODY:
            case ESM::REC_ACTI4:
            case ESM::REC_ALCH4:
            case ESM::REC_AMMO4:
            case ESM::REC_ARMO4:
            case ESM::REC_BOOK4:
            case ESM::REC_CONT4:
            case ESM::REC_CREA4:
            case ESM::REC_DOOR4:
            case ESM::REC_FLOR4:
            case ESM::REC_FURN4:
            case ESM::REC_IMOD4:
            case ESM::REC_INGR4:
            case ESM::REC_LIGH4:
            case ESM::REC_LVLI4:
            case ESM::REC_LVLC4:
            case ESM::REC_LVLN4:
            case ESM::REC_MISC4:
            case ESM::REC_MSTT4:
            case ESM::REC_NPC_4:
            case ESM::REC_SCOL4:
            case ESM::REC_STAT4:
            case ESM::REC_TERM4:
            case ESM::REC_TREE4:
            case ESM::REC_WEAP4:
                return true;
                break;
        }
        return false;
    }

    void ESMStore::load(ESM::ESMReader& esm, Loading::Listener* listener, ESM::Dialogue*& dialogue)
    {
        if (listener != nullptr)
            listener->setProgressRange(::EsmLoader::fileProgress);

        // Loop through all records
        while (esm.hasMoreRecs())
        {
            ESM::NAME n = esm.getRecName();
#ifdef OPENMW_PLATFORM_3DS
            p3ds::memprobe::Scope memScope(n.toInt()); // heap growth per record type (diagnostic)
            p3ds::crash::kick("loading ESM"); // loader thread progress (the loading screen may not redraw)
#endif
            esm.getRecHeader();
            if (esm.getRecordFlags() & ESM::FLAG_Ignored)
            {
                esm.skipRecord();
                continue;
            }

            // Look up the record type.
            ESM::RecNameInts recName = static_cast<ESM::RecNameInts>(n.toInt());
            const auto& it = mStoreImp->mRecNameToStore.find(recName);

            if (it == mStoreImp->mRecNameToStore.end())
            {
                if (recName == ESM::REC_INFO)
                {
                    if (dialogue)
                    {
                        dialogue->readInfo(esm);
                    }
                    else
                    {
                        Log(Debug::Error) << "Error: info record without dialog";
                        esm.skipRecord();
                    }
                }
                else if (n.toInt() == ESM::REC_MGEF)
                {
                    getWritable<ESM::MagicEffect>().load(esm);
                }
                else if (n.toInt() == ESM::REC_SKIL)
                {
                    getWritable<ESM::Skill>().load(esm);
                }
                else if (n.toInt() == ESM::REC_FILT || n.toInt() == ESM::REC_DBGP)
                {
                    // ignore project file only records
                    esm.skipRecord();
                }
                else if (n.toInt() == ESM::REC_LUAL)
                {
                    ESM::LuaScriptsCfg cfg;
                    cfg.load(esm);
                    cfg.adjustRefNums(esm);
                    mLuaContent.push_back(std::move(cfg));
                }
                else
                {
                    throw std::runtime_error("Unknown record: " + n.toString());
                }
            }
            else
            {
                RecordId id = it->second->load(esm);
                if (id.mIsDeleted)
                {
                    it->second->eraseStatic(id.mId);
                    continue;
                }

                if (n.toInt() == ESM::REC_DIAL)
                {
                    dialogue = const_cast<ESM::Dialogue*>(getWritable<ESM::Dialogue>().find(id.mId));
                }
                else
                {
                    dialogue = nullptr;
                }
            }
            if (listener != nullptr)
                listener->setProgress(::EsmLoader::fileProgress * esm.getFileOffset() / esm.getFileSize());
        }
#ifdef OPENMW_PLATFORM_3DS
        p3ds::memprobe::report(esm.getName().string().c_str());
#endif
    }

    void ESMStore::loadESM4(ESM4::Reader& reader, Loading::Listener* listener)
    {
        if (listener != nullptr)
            listener->setProgressRange(::EsmLoader::fileProgress);
        auto visitorRec = [this, listener](ESM4::Reader& r) {
            bool result = ESMStoreImp::readRecord(r, *this);
            if (listener != nullptr)
                listener->setProgress(::EsmLoader::fileProgress * r.getFileOffset() / r.getFileSize());
            return result;
        };
        ESM4::ReaderUtils::readAll(reader, visitorRec, [](ESM4::Reader&) {});
    }

    void ESMStore::setIdType(const ESM::RefId& id, ESM::RecNameInts type)
    {
        mStoreImp->mIds[id] = type;
    }

    ESM::LuaScriptsCfg ESMStore::getLuaScriptsCfg() const
    {
        ESM::LuaScriptsCfg cfg;
        for (const LuaContent& c : mLuaContent)
        {
            if (std::holds_alternative<std::filesystem::path>(c))
            {
                // *.omwscripts are intentionally reloaded every time when `getLuaScriptsCfg` is called.
                // It is important for the `reloadlua` console command.
                try
                {
                    auto file = std::ifstream(std::get<std::filesystem::path>(c));
                    std::string fileContent(std::istreambuf_iterator<char>(file), {});
                    LuaUtil::parseOMWScripts(cfg, fileContent);
                }
                catch (std::exception& e)
                {
                    Log(Debug::Error) << e.what();
                }
            }
            else
            {
                const ESM::LuaScriptsCfg& addition = std::get<ESM::LuaScriptsCfg>(c);
                cfg.mScripts.insert(cfg.mScripts.end(), addition.mScripts.begin(), addition.mScripts.end());
            }
        }
        return cfg;
    }

    void ESMStore::setUp()
    {
        if (mIsSetUpDone)
            throw std::logic_error("ESMStore::setUp() is called twice");
        mIsSetUpDone = true;

        for (const auto& [_, store] : mStoreImp->mRecNameToStore)
            store->setUp();

        getWritable<ESM4::Land>().updateLandPositions(get<ESM4::Cell>());
        getWritable<ESM4::Reference>().preprocessReferences(get<ESM4::Cell>());
        getWritable<ESM4::ActorCharacter>().preprocessReferences(get<ESM4::Cell>());
        getWritable<ESM4::ActorCreature>().preprocessReferences(get<ESM4::Cell>());

        rebuildIdsIndex();
#ifdef OPENMW_PLATFORM_3DS
        // M10: one copy of the index of every record (~2 MB): mIds holds only what changed since (records created
        // or overridden in play), find() falls back to mStaticIds.
        mStoreImp->mStaticIds = std::move(mStoreImp->mIds);
        mStoreImp->mIds = IDMap();
#else
        mStoreImp->mStaticIds = mStoreImp->mIds;
#endif
    }

    void ESMStore::rebuildIdsIndex()
    {
        mStoreImp->mIds.clear();
        for (const auto& [recordType, store] : mStoreImp->mRecNameToStore)
        {
            if (isCacheableRecord(recordType))
            {
                std::vector<ESM::RefId> identifiers;
                store->listIdentifier(identifiers);
                for (auto& record : identifiers)
                    mStoreImp->mIds[record] = recordType;
            }
        }
    }

    void ESMStore::validateRecords(ESM::ReadersCache& readers)
    {
        validate();
        countAllCellRefsAndMarkKeys(readers);
    }

    void ESMStore::countAllCellRefsAndMarkKeys(ESM::ReadersCache& readers)
    {
        // TODO: We currently need to read entire files here again.
        // We should consider consolidating or deferring this reading.
        if (!mRefInfo.empty())
            return;
#ifdef OPENMW_PLATFORM_3DS
        const std::string cacheKey = refInfoCacheKey();
        if (loadRefInfoCache(cacheKey))
            return;
#endif
        std::vector<Ref> refs;
        std::set<ESM::RefId> keyIDs;
        std::vector<ESM::RefId> refIDs;
        const Store<ESM::Cell>& cells = get<ESM::Cell>();
        const std::size_t cellCount = cells.getSize();
        const std::size_t interiorCellCount = cells.getIntSize();
        const std::size_t exteriorCellCount = cells.getExtSize();
        for (std::size_t i = 0; i < cellCount; ++i)
        {
#ifdef OPENMW_PLATFORM_3DS
            p3ds::crash::kick("counting refs"); // re-reads every cell's references: minutes on SD
            p3ds::perf::detail::Scope readRefsScope("readRefs");
#endif
            readRefs(*cells.at(i), i, refs, refIDs, keyIDs, readers);
        }
        const auto lessByRefNum = [](const Ref& l, const Ref& r) { return l.mRefNum < r.mRefNum; };
        std::stable_sort(refs.begin(), refs.end(), lessByRefNum);
        const auto equalByRefNum = [](const Ref& l, const Ref& r) { return l.mRefNum == r.mRefNum; };
        const auto forEachEffectiveRef = [&](auto&& fn) {
            Misc::forEachUnique(refs.rbegin(), refs.rend(), equalByRefNum, [&](const Ref& value) {
                if (value.mRefID != deletedRefID)
                    fn(value, refIDs[value.mRefID]);
            });
        };

        std::vector<std::vector<RefInfo*>> refsPerCell(cellCount);
        forEachEffectiveRef([&](const Ref& value, const ESM::RefId& refId) {
            RefInfo& info = mRefInfo[refId];
            ++info.mCount;
            refsPerCell[value.mCellIndex].push_back(&info);
        });
        // Exteriors first, matching world lookup order.
        for (std::size_t i = 0; i < cellCount; ++i)
        {
            const std::size_t cellIndex = i < exteriorCellCount ? interiorCellCount + i : i - exteriorCellCount;
            const ESM::Cell* const cell = cells.at(cellIndex);
            for (RefInfo* info : refsPerCell[cellIndex])
                if (info->mCells.empty() || info->mCells.back() != cell)
                    info->mCells.push_back(cell);
        }
        auto& store = getWritable<ESM::Miscellaneous>().mStatic;
        for (const auto& id : keyIDs)
        {
            auto it = store.find(id);
            if (it != store.end())
                it->second.mData.mFlags |= ESM::Miscellaneous::Key;
        }
#ifdef OPENMW_PLATFORM_3DS
        saveRefInfoCache(cacheKey, keyIDs);
#endif
    }

#ifdef OPENMW_PLATFORM_3DS
    std::string ESMStore::refInfoCacheKey() const
    {
        const Store<ESM::Cell>& cells = get<ESM::Cell>();
        std::set<std::filesystem::path> files;
        for (std::size_t i = 0; i < cells.getSize(); ++i)
            for (const ESM::ESM_Context& context : cells.at(i)->mContextList)
                files.insert(context.filename);
        std::string key = std::to_string(sRefInfoCacheVersion) + '|' + std::to_string(cells.getSize());
        for (const std::filesystem::path& file : files)
        {
            std::error_code ec;
            const std::uintmax_t size = std::filesystem::file_size(file, ec);
            key += '|' + file.string() + ':' + std::to_string(ec ? 0 : size);
        }
        return key;
    }

    bool ESMStore::loadRefInfoCache(const std::string& key)
    {
        std::string data;
        if (FILE* file = std::fopen(sRefInfoCachePath, "rb"))
        {
            if (std::fseek(file, 0, SEEK_END) == 0)
            {
                const long size = std::ftell(file);
                if (size > 0 && std::fseek(file, 0, SEEK_SET) == 0)
                {
                    data.resize(static_cast<std::size_t>(size));
                    if (std::fread(data.data(), 1, data.size(), file) != data.size())
                        data.clear();
                }
            }
            std::fclose(file);
        }
        if (data.empty())
            return false;

        const Store<ESM::Cell>& cells = get<ESM::Cell>();
        CacheReader reader{ data.data(), data.data() + data.size() };
        std::vector<ESM::RefId> keyIDs;
        try
        {
            if (!reader.magic() || reader.string() != key)
            {
                Log(Debug::Info) << "Ref counts: " << sRefInfoCachePath << " is for other content files, counting";
                return false;
            }
            // Cells by id (their order in the store needn't be the same from one run to the next).
            std::unordered_map<ESM::RefId, const ESM::Cell*> cellsById;
            cellsById.reserve(cells.getSize());
            for (std::size_t i = 0; i < cells.getSize(); ++i)
                cellsById.emplace(cells.at(i)->mId, cells.at(i));
            std::vector<const ESM::Cell*> cellTable(reader.u32());
            for (std::size_t i = 0; i < cellTable.size() && reader.mOk; ++i)
            {
                const auto found = cellsById.find(ESM::RefId::deserializeText(reader.string()));
                if (found == cellsById.end())
                    reader.mOk = false;
                else
                    cellTable[i] = found->second;
            }
            const std::uint32_t keyCount = reader.u32();
            for (std::uint32_t i = 0; i < keyCount && reader.mOk; ++i)
                keyIDs.push_back(ESM::RefId::deserializeText(reader.string()));
            const std::uint32_t infoCount = reader.u32();
            mRefInfo.reserve(infoCount);
            for (std::uint32_t i = 0; i < infoCount && reader.mOk; ++i)
            {
                RefInfo& info = mRefInfo[ESM::RefId::deserializeText(reader.string())];
                info.mCount = static_cast<int>(reader.u32());
                const std::uint32_t cellCount = reader.u32();
                for (std::uint32_t c = 0; c < cellCount && reader.mOk; ++c)
                {
                    const std::uint32_t index = reader.u32();
                    if (index >= cellTable.size())
                        reader.mOk = false;
                    else
                        info.mCells.push_back(cellTable[index]);
                }
            }
            reader.magic(); // end marker: a truncated file doesn't load
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "Ref counts: " << sRefInfoCachePath << ": " << e.what();
            reader.mOk = false;
        }
        if (!reader.mOk || reader.mPos != reader.mEnd)
        {
            Log(Debug::Warning) << "Ref counts: " << sRefInfoCachePath << " is damaged, counting";
            mRefInfo.clear();
            return false;
        }

        auto& store = getWritable<ESM::Miscellaneous>().mStatic;
        for (const ESM::RefId& id : keyIDs)
        {
            auto it = store.find(id);
            if (it != store.end())
                it->second.mData.mFlags |= ESM::Miscellaneous::Key;
        }
        Log(Debug::Info) << "Ref counts: " << mRefInfo.size() << " objects from " << sRefInfoCachePath;
        return true;
    }

    void ESMStore::saveRefInfoCache(const std::string& key, const std::set<ESM::RefId>& keyIDs) const
    {
        const Store<ESM::Cell>& cells = get<ESM::Cell>();
        std::unordered_map<const ESM::Cell*, std::uint32_t> cellIndices;
        cellIndices.reserve(cells.getSize());
        for (std::size_t i = 0; i < cells.getSize(); ++i)
            cellIndices.emplace(cells.at(i), static_cast<std::uint32_t>(i));

        std::string out;
        out.append(sRefInfoCacheMagic, 4);
        appendString(out, key);
        appendU32(out, static_cast<std::uint32_t>(cells.getSize()));
        for (std::size_t i = 0; i < cells.getSize(); ++i)
            appendString(out, cells.at(i)->mId.serializeText());
        appendU32(out, static_cast<std::uint32_t>(keyIDs.size()));
        for (const ESM::RefId& id : keyIDs)
            appendString(out, id.serializeText());
        appendU32(out, static_cast<std::uint32_t>(mRefInfo.size()));
        for (const auto& [id, info] : mRefInfo)
        {
            appendString(out, id.serializeText());
            appendU32(out, static_cast<std::uint32_t>(info.mCount));
            appendU32(out, static_cast<std::uint32_t>(info.mCells.size()));
            for (const ESM::Cell* cell : info.mCells)
                appendU32(out, cellIndices.at(cell));
        }
        out.append(sRefInfoCacheMagic, 4);

        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(sRefInfoCachePath).parent_path(), ec);
        FILE* file = std::fopen(sRefInfoCachePath, "wb");
        bool ok = file != nullptr;
        if (ok)
        {
            ok = std::fwrite(out.data(), 1, out.size(), file) == out.size();
            ok = std::fclose(file) == 0 && ok;
        }
        if (ok)
            Log(Debug::Info) << "Ref counts saved to " << sRefInfoCachePath << " (" << out.size() / 1024 << " KB)";
        else
        {
            Log(Debug::Warning) << "Ref counts: failed to write " << sRefInfoCachePath;
            std::remove(sRefInfoCachePath);
        }
    }
#endif

    int ESMStore::getRefCount(const ESM::RefId& id) const
    {
        auto it = mRefInfo.find(id);
        if (it == mRefInfo.end())
            return 0;
        return it->second.mCount;
    }

    std::span<const ESM::Cell* const> ESMStore::getRefCells(const ESM::RefId& id) const
    {
        const auto it = mRefInfo.find(id);
        if (it == mRefInfo.end())
            return {};
        return it->second.mCells;
    }

    void ESMStore::validate()
    {
        auto& npcs = getWritable<ESM::NPC>();
        std::vector<ESM::NPC> npcsToReplace = getNPCsToReplace(getWritable<ESM::Faction>(), getWritable<ESM::Class>(),
            getWritable<ESM::Race>(), getWritable<ESM::Script>(), npcs.mStatic);

        for (const ESM::NPC& npc : npcsToReplace)
        {
            npcs.eraseStatic(npc.mId);
            npcs.insertStatic(npc);
        }

        removeMissingScripts(getWritable<ESM::Script>(), getWritable<ESM::Creature>().mStatic);

        // Validate spell effects and enchantments for invalid arguments
        auto& spells = getWritable<ESM::Spell>();
        auto& enchantments = getWritable<ESM::Enchantment>();
        auto& magicEffects = getWritable<ESM::MagicEffect>();

        std::vector<ESM::Spell> spellsToReplace = getSpellsToReplace(spells, magicEffects);
        for (const ESM::Spell& spell : spellsToReplace)
        {
            spells.eraseStatic(spell.mId);
            spells.insertStatic(spell);
        }

        std::vector<ESM::Enchantment> enchantmentsToReplace = getSpellsToReplace(enchantments, magicEffects);
        for (const ESM::Enchantment& enchantment : enchantmentsToReplace)
        {
            enchantments.eraseStatic(enchantment.mId);
            enchantments.insertStatic(enchantment);
        }
    }

    void ESMStore::movePlayerRecord()
    {
        auto& npcs = getWritable<ESM::NPC>();
        auto player = npcs.find(ESM::RefId::stringRefId("Player"));
        npcs.insert(*player);
    }

    void ESMStore::validateDynamic()
    {
        auto& npcs = getWritable<ESM::NPC>();
        auto& scripts = getWritable<ESM::Script>();

        std::vector<ESM::NPC> npcsToReplace = getNPCsToReplace(getWritable<ESM::Faction>(), getWritable<ESM::Class>(),
            getWritable<ESM::Race>(), getWritable<ESM::Script>(), npcs.mDynamic);

        for (const ESM::NPC& npc : npcsToReplace)
            npcs.insert(npc);

        removeMissingScripts(scripts, getWritable<ESM::Armor>().mDynamic);
        removeMissingScripts(scripts, getWritable<ESM::Book>().mDynamic);
        removeMissingScripts(scripts, getWritable<ESM::Clothing>().mDynamic);
        removeMissingScripts(scripts, getWritable<ESM::Creature>().mDynamic);
        removeMissingScripts(scripts, getWritable<ESM::Weapon>().mDynamic);

        removeMissingObjects(getWritable<ESM::CreatureLevList>());
        removeMissingObjects(getWritable<ESM::ItemLevList>());
    }

    // Leveled lists can be modified by scripts. This removes items that no longer exist (presumably because the
    // plugin was removed) from modified lists
    template <class T>
    void ESMStore::removeMissingObjects(Store<T>& store)
    {
        for (auto& entry : store.mDynamic)
        {
            auto first = std::remove_if(entry.second.mList.begin(), entry.second.mList.end(), [&](const auto& item) {
                if (!find(item.mId))
                {
                    Log(Debug::Verbose) << "Leveled list " << entry.first << " has nonexistent object " << item.mId
                                        << ", ignoring it.";
                    return true;
                }
                return false;
            });
            entry.second.mList.erase(first, entry.second.mList.end());
        }
    }

    size_t ESMStore::countSavedGameRecords() const
    {
        return 1 // DYNA (dynamic name counter)
            + get<ESM::Potion>().getDynamicSize() + get<ESM::Armor>().getDynamicSize()
            + get<ESM::Book>().getDynamicSize() + get<ESM::Class>().getDynamicSize()
            + get<ESM::Clothing>().getDynamicSize() + get<ESM::Enchantment>().getDynamicSize()
            + get<ESM::NPC>().getDynamicSize() + get<ESM::Spell>().getDynamicSize()
            + get<ESM::Activator>().getDynamicSize() + get<ESM::Miscellaneous>().getDynamicSize()
            + get<ESM::Weapon>().getDynamicSize() + get<ESM::CreatureLevList>().getDynamicSize()
            + get<ESM::ItemLevList>().getDynamicSize() + get<ESM::Creature>().getDynamicSize()
            + get<ESM::Container>().getDynamicSize() + get<ESM::Light>().getDynamicSize()
            + get<ESM::Static>().getDynamicSize() + get<ESM::Door>().getDynamicSize()
            + get<ESM::Probe>().getDynamicSize() + get<ESM::Ingredient>().getDynamicSize();
    }

    void ESMStore::write(ESM::ESMWriter& writer, Loading::Listener& progress) const
    {
        writer.startRecord(ESM::REC_DYNA);
        writer.startSubRecord("COUN");
        writer.writeT(mDynamicCount);
        writer.endRecord("COUN");
        writer.endRecord(ESM::REC_DYNA);

        get<ESM::Potion>().write(writer, progress);
        get<ESM::Armor>().write(writer, progress);
        get<ESM::Book>().write(writer, progress);
        get<ESM::Class>().write(writer, progress);
        get<ESM::Clothing>().write(writer, progress);
        get<ESM::Enchantment>().write(writer, progress);
        get<ESM::NPC>().write(writer, progress);
        get<ESM::Miscellaneous>().write(writer, progress);
        get<ESM::Activator>().write(writer, progress);
        get<ESM::Spell>().write(writer, progress);
        get<ESM::Weapon>().write(writer, progress);
        get<ESM::CreatureLevList>().write(writer, progress);
        get<ESM::ItemLevList>().write(writer, progress);
        get<ESM::Creature>().write(writer, progress);
        get<ESM::Container>().write(writer, progress);
        get<ESM::Light>().write(writer, progress);
        get<ESM::Static>().write(writer, progress);
        get<ESM::Door>().write(writer, progress);
        get<ESM::Probe>().write(writer, progress);
        get<ESM::Ingredient>().write(writer, progress);
    }

    bool ESMStore::readRecord(ESM::ESMReader& reader, uint32_t typeId)
    {
        ESM::RecNameInts type = static_cast<ESM::RecNameInts>(typeId);
        switch (type)
        {
            case ESM::REC_ALCH:
            case ESM::REC_ARMO:
            case ESM::REC_BOOK:
            case ESM::REC_CLAS:
            case ESM::REC_CLOT:
            case ESM::REC_ENCH:
            case ESM::REC_SPEL:
            case ESM::REC_WEAP:
                mStoreImp->mRecNameToStore[type]->read(reader);
                return true;
            case ESM::REC_NPC_:
            case ESM::REC_CREA:
            case ESM::REC_CONT:
            case ESM::REC_MISC:
            case ESM::REC_ACTI:
            case ESM::REC_LEVI:
            case ESM::REC_LEVC:
            case ESM::REC_LIGH:
            case ESM::REC_STAT:
            case ESM::REC_DOOR:
            case ESM::REC_PROB:
            case ESM::REC_INGR:
                mStoreImp->mRecNameToStore[type]->read(reader, true);
                return true;

            case ESM::REC_DYNA:
                reader.getSubNameIs("COUN");
                if (reader.getFormatVersion() <= ESM::MaxActiveSpellTypeVersion)
                {
                    uint32_t dynamicCount32 = 0;
                    reader.getHT(dynamicCount32);
                    mDynamicCount = dynamicCount32;
                }
                else
                {
                    reader.getHT(mDynamicCount);
                }
                return true;

            default:

                return false;
        }
    }

    void ESMStore::checkPlayer()
    {
        const ESM::NPC* player = get<ESM::NPC>().find(ESM::RefId::stringRefId("Player"));

        if (!get<ESM::Race>().find(player->mRace) || !get<ESM::Class>().find(player->mClass))
            throw std::runtime_error("Invalid player record (race or class unavailable");
    }

    std::pair<std::shared_ptr<MWMechanics::SpellList>, bool> ESMStore::getSpellList(ESM::RefId id, bool autoCalc) const
    {
        auto result = mSpellListCache.find(id);
        std::shared_ptr<MWMechanics::SpellList> ptr;
        if (result != mSpellListCache.end())
            ptr = result->second.lock();
        if (!ptr)
        {
            int type = find(id);
            ptr = std::make_shared<MWMechanics::SpellList>(id, type, autoCalc);
            if (result != mSpellListCache.end())
                result->second = ptr;
            else
                mSpellListCache.insert({ id, ptr });
            return { ptr, false };
        }
        return { ptr, true };
    }

    template <>
    const ESM::Cell* ESMStore::insert<ESM::Cell>(const ESM::Cell& cell)
    {
        return getWritable<ESM::Cell>().insert(cell);
    }

    template <>
    const ESM::NPC* ESMStore::insert<ESM::NPC>(const ESM::NPC& npc)
    {

        auto& npcs = getWritable<ESM::NPC>();
        if (npc.mId == "Player")
        {
            return npcs.insert(npc);
        }
        const ESM::RefId id = ESM::RefId::generated(mDynamicCount++);
        if (npcs.search(id) != nullptr)
            throw std::runtime_error("Try to override existing record: " + id.toDebugString());
        ESM::NPC record = npc;

        record.mId = id;

        ESM::NPC* ptr = npcs.insert(record);
        mStoreImp->mIds[ptr->mId] = ESM::REC_NPC_;
        return ptr;
    }

} // end namespace
