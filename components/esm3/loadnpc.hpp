#ifndef OPENMW_ESM_NPC_H
#define OPENMW_ESM_NPC_H

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <components/esm/attr.hpp>
#include <components/esm/defs.hpp>
#include <components/esm/path.hpp>
#include <components/esm/refid.hpp>

#include "aipackage.hpp"
#include "loadcont.hpp"
#include "loadskil.hpp"
#include "spelllist.hpp"
#include "transport.hpp"

namespace ESM
{

    class ESMReader;
    class ESMWriter;

#ifdef OPENMW_PLATFORM_3DS
    // A sorted vector with the parts of std::map's interface NPC stats use (M9: 2675 NPC records kept 35 map nodes
    // each, ~1 KB more per record than this).
    class StatMap
    {
    public:
        using value_type = std::pair<ESM::RefId, unsigned char>;
        using const_iterator = std::vector<value_type>::const_iterator;

        const_iterator begin() const { return mItems.begin(); }
        const_iterator end() const { return mItems.end(); }
        bool empty() const { return mItems.empty(); }
        std::size_t size() const { return mItems.size(); }
        void clear() { mItems.clear(); }
        void reserve(std::size_t count) { mItems.reserve(count); }
        const_iterator find(const ESM::RefId& key) const
        {
            const auto it = lowerBound(key);
            return it != mItems.end() && it->first == key ? const_iterator(it) : mItems.end();
        }
        void emplace(const ESM::RefId& key, unsigned char value)
        {
            const auto it = lowerBound(key);
            if (it == mItems.end() || !(it->first == key))
                mItems.insert(it, value_type(key, value));
        }
        unsigned char& operator[](const ESM::RefId& key)
        {
            auto it = lowerBound(key);
            if (it == mItems.end() || !(it->first == key))
                it = mItems.insert(it, value_type(key, 0));
            return it->second;
        }
        bool operator==(const StatMap& other) const { return mItems == other.mItems; }

    private:
        std::vector<value_type>::iterator lowerBound(const ESM::RefId& key) const
        {
            auto& items = const_cast<std::vector<value_type>&>(mItems);
            return std::lower_bound(items.begin(), items.end(), key,
                [](const value_type& item, const ESM::RefId& k) { return item.first < k; });
        }

        std::vector<value_type> mItems;
    };
#endif

    /*
     * NPC definition
     */

    struct NPC
    {
        constexpr static RecNameInts sRecordId = REC_NPC_;

        /// Return a string descriptor for this record type. Currently used for debugging / error logs only.
        static std::string_view getRecordType() { return "NPC"; }

        // Services
        enum Services
        {
            // This merchant buys:
            Weapon = 0x00001,
            Armor = 0x00002,
            Clothing = 0x00004,
            Books = 0x00008,
            Ingredients = 0x00010,
            Picks = 0x00020,
            Probes = 0x00040,
            Lights = 0x00080,
            Apparatus = 0x00100,
            RepairItem = 0x00200,
            Misc = 0x00400,
            Potions = 0x02000,

            AllItems = Weapon | Armor | Clothing | Books | Ingredients | Picks | Probes | Lights | Apparatus
                | RepairItem | Misc | Potions,

            // Other services
            Spells = 0x00800,
            MagicItems = 0x01000,
            Training = 0x04000,
            Spellmaking = 0x08000,
            Enchanting = 0x10000,
            Repair = 0x20000
        };

        enum Flags
        {
            Female = 0x01,
            Essential = 0x02,
            Respawn = 0x04,
            Base = 0x08,
            Autocalc = 0x10
        };

        enum NpcType
        {
            NPC_WITH_AUTOCALCULATED_STATS = 12,
            NPC_DEFAULT = 52
        };

        struct NPDTstruct52
        {
#ifdef OPENMW_PLATFORM_3DS
            StatMap mAttributes;
            // mSkill can grow up to 200, it must be unsigned
            StatMap mSkills;
#else
            std::map<ESM::RefId, unsigned char> mAttributes;
            // mSkill can grow up to 200, it must be unsigned
            std::map<ESM::RefId, unsigned char> mSkills;
#endif
            int16_t mLevel;

            uint16_t mHealth, mMana, mFatigue;
            unsigned char mDisposition, mReputation, mRank;
            int32_t mGold;

            unsigned char getAttribute(ESM::RefId) const;
            unsigned char getSkill(ESM::RefId) const;
        }; // 52 bytes

        unsigned char mNpdtType;
        // Worth noting when saving the struct:
        //  Although we might read a NPDTstruct12 in, we use NPDTstruct52 internally
        NPDTstruct52 mNpdt;

        int getFactionRank() const; /// wrapper for mNpdt*, -1 = no rank

        int32_t mBloodType;
        unsigned char mFlags;

        InventoryList mInventory;
        SpellList mSpells;

        AIData mAiData;

        Transport mTransport;

        const std::vector<Transport::Dest>& getTransport() const;

        AIPackageList mAiPackage;

        uint32_t mRecordFlags;
        RefId mId, mRace, mClass, mFaction, mScript;
        std::string mName;
        Path mModel;

        // body parts
        RefId mHair, mHead;

        void load(ESMReader& esm, bool& isDeleted);
        void save(ESMWriter& esm, bool isDeleted = false) const;

        bool isMale() const;

        void setIsMale(bool value);

        void blank();
        ///< Set record to default state (does not touch the ID).

        /// Resets the mNpdt object
        void blankNpdt();
    };
}
#endif
