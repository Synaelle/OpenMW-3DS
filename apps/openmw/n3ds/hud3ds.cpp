// Nintendo 3DS HUD and bottom-screen tabs (see hud3ds.hpp).

#include "hud3ds.hpp"
#include "frame3ds.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <functional>
#include <memory>
#include <string>

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_Gui.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_ProgressBar.h>
#include <MyGUI_LanguageManager.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_ResourceManager.h>
#include <MyGUI_TextBox.h>

#include <3ds.h>
#include <platform3ds/platform.hpp>
#include <MyGUI_Window.h>


#include <components/esm3/loadench.hpp>
#include <components/esm3/loadmgef.hpp>
#include <components/esm3/loadspel.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/render3ds/render3ds.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwgui/hud.hpp"
#include "../mwgui/inventoryitemmodel.hpp"
#include "../mwgui/inventorywindow.hpp"
#include "../mwgui/itemview.hpp"
#include "../mwgui/itemwidget.hpp"
#include "../mwgui/layout.hpp"
#include "../mwgui/quickkeysmenu.hpp"
#include "../mwgui/sortfilteritemmodel.hpp"
#include "../mwgui/spellwindow.hpp"
#include "../mwgui/statswindow.hpp"
#include "../mwgui/tooltips.hpp"
#include "../mwgui/widgets.hpp"
#include "../mwgui/windowmanagerimp.hpp"
#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwmechanics/activespells.hpp"
#include "../mwmechanics/spellutil.hpp"
#include "../mwworld/cellref.hpp"
#include <components/esm3/loadweap.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadrace.hpp>
#include <components/esm3/loadclas.hpp>
#include <components/esm3/loadskil.hpp>
#include <components/esm/attr.hpp>
#include "../mwmechanics/npcstats.hpp"
#include "../mwmechanics/spells.hpp"
#include "../mwworld/containerstore.hpp"
#include <cmath>
#include <tuple>
#include <vector>
#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/inventorystore.hpp"

namespace omw3ds
{
    namespace
    {
        // Bottom screen (canvas): the tab bar takes the top 22 rows, the HUD panel and the pages the rest.
        constexpr int kBottomX = 40;
        constexpr int kBottomY = 240;
        constexpr int kPanelY = kBottomY + 22;
        constexpr int kPageWidth = 320;
        constexpr int kPageHeight = 218;
        constexpr int kSlots = 9;
        // Top screen: the target label's top edge (the screen's width is the canvas's).
        constexpr int kLabelTop = 2;

        enum class Tab
        {
            None, // the HUD
            Items,
            Magic,
            Stats,
        };
    }

    // OpenMW's windows and quick keys, through OMW3DS_HUD_FRIEND (WindowManager, QuickKeysMenu).
    struct Hud3DS
    {
        static MWGui::WindowManager& windows()
        {
            return static_cast<MWGui::WindowManager&>(*MWBase::Environment::get().getWindowManager());
        }

        // An item on quick key `slot` (0-8), as QuickKeysMenu assigns it: items with a "cast when used" or "cast
        // once" enchantment cast it, the others are used or equipped.
        // Offers the player's item to the merchant, as a click on it in OpenMW's inventory window while bartering
        // does (a count dialog for a stack); with a companion or a container open, moves it to them (M17).
        static void sell(const MWWorld::Ptr& item)
        {
            MWGui::InventoryWindow* inv = inventory();
            if (inv == nullptr)
                return;
            for (size_t i = 0; i < inv->mSortModel->getItemCount(); ++i)
                if (inv->mSortModel->getItem(i).mBase == item)
                {
                    if (windows().getMode() == MWGui::GM_Companion || windows().getMode() == MWGui::GM_Container)
                        inv->mPendingControllerAction = MWGui::InventoryWindow::ControllerAction::Transfer;
                    inv->onItemSelectedFromSourceModel(static_cast<int>(inv->mSortModel->mapToSource(i)));
                    return;
                }
        }

        static void assignItem(int slot, const MWWorld::Ptr& item)
        {
            MWGui::QuickKeysMenu* menu = windows().mQuickKeysMenu;
            menu->mSelected = &menu->mKey[slot];
            const ESM::RefId& enchantmentId = item.getClass().getEnchantment(item);
            const ESM::Enchantment* enchantment = enchantmentId.empty()
                ? nullptr
                : MWBase::Environment::get().getESMStore()->get<ESM::Enchantment>().search(enchantmentId);
            if (enchantment != nullptr
                && (enchantment->mData.mType == ESM::Enchantment::WhenUsed
                    || enchantment->mData.mType == ESM::Enchantment::CastOnce))
                menu->onAssignMagicItem(item);
            else
                menu->assignItem(item);
        }

        // The spell or magic item Y readies, on quick key `slot`.
        static void assignSelectedMagic(int slot)
        {
            MWGui::QuickKeysMenu* menu = windows().mQuickKeysMenu;
            const ESM::RefId& spell = windows().getSelectedSpell();
            const MWWorld::Ptr& item = windows().getSelectedEnchantItem();
            menu->mSelected = &menu->mKey[slot];
            if (!spell.empty())
                menu->onAssignMagic(spell);
            else if (!item.isEmpty())
                menu->onAssignMagicItem(item);
        }

        static void quickKeyState(int slot, ESM::QuickKeys::Type& type, ESM::RefId& id)
        {
            const auto& key = windows().mQuickKeysMenu->mKey[slot];
            type = key.type;
            id = key.id;
        }

        static MWGui::HUD* hud() { return windows().mHud; }

        // The weapon's and the spell's names as OpenMW's HUD last showed them (may hold #{} tags).
        static std::pair<std::string, std::string> selectedNames()
        {
            return { hud()->mWeaponName, hud()->mSpellName };
        }

        static std::pair<bool, bool> boxesVisible()
        {
            return { hud()->mWeapBox->getVisible(), hud()->mSpellBox->getVisible() };
        }
        static MWGui::InventoryWindow* inventory() { return windows().mInventoryWindow; }
        static MWGui::SpellWindow* magic() { return windows().mSpellWindow; }
        static MWGui::StatsWindow* stats() { return windows().mStatsWindow; }
        static bool ready() { return windows().mQuickKeysMenu != nullptr && windows().mHud != nullptr; }
    };

    namespace
    {
        MWGui::WindowManager& windows()
        {
            return Hud3DS::windows();
        }

        // The ui3ds data directory's layouts (packaging/ui3ds), or OpenMW's own.
        bool has3DSLayouts()
        {
            return MyGUI::ResourceManager::getInstance().isExist("MW_Page3DS");
        }

        // ------------------------------------------------------------------ quick-key strip (HUD)

        struct Slot
        {
            MyGUI::Button* mButton = nullptr;
            MWGui::ItemWidget* mIcon = nullptr;
            ESM::QuickKeys::Type mType = ESM::QuickKeys::Type::Unassigned;
            ESM::RefId mId;
            bool mShown = false; // mType/mId are on screen
            int mCount = -1; // the count shown (items: how many the player carries, all stacks)
        };

        std::array<Slot, kSlots> sSlots;
        const MWGui::HUD* sStripBoundTo = nullptr;
        MyGUI::TextBox* sFps = nullptr; // the frame rate under the minimap

        void onStripSlotClicked(MyGUI::Widget* sender)
        {
            for (int i = 0; i < kSlots; ++i)
                if (sSlots[i].mButton == sender)
                    windows().activateQuickKey(i + 1);
        }

        bool bindStrip()
        {
            MWGui::HUD* hud = Hud3DS::hud();
            if (hud == sStripBoundTo)
                return true;
            if (!has3DSLayouts())
                return false;
            for (int i = 0; i < kSlots; ++i)
            {
                Slot& slot = sSlots[i];
                slot = Slot{};
                const std::string n = std::to_string(i + 1);
                hud->getWidget(slot.mButton, "QuickKey3DS" + n);
                hud->getWidget(slot.mIcon, "QuickKey3DSIcon" + n);
                slot.mButton->eventMouseButtonClick += MyGUI::newDelegate(&onStripSlotClicked);
            }
            hud->getWidget(sFps, "Fps3DS");
            // A debug readout: only after the start menu's Debug choices (M15).
            sFps->setVisible(p3ds::debugMode());
            sStripBoundTo = hud;
            return true;
        }

        // The frame rate once a second under the minimap (the bottom screen redraws it as a small rect; a caption
        // changing on the top overlay broke its redraw in test #61).
        void updateFps()
        {
            if (sFps == nullptr || !sFps->getVisible())
                return;
            static uint64_t lastTick = 0;
            static unsigned frames = 0;
            ++frames;
            const uint64_t now = svcGetSystemTick();
            if (lastTick == 0)
                lastTick = now;
            const uint64_t elapsed = now - lastTick;
            if (elapsed < SYSCLOCK_ARM11)
                return;
            const double fps = double(frames) * double(SYSCLOCK_ARM11) / double(elapsed);
            frames = 0;
            lastTick = now;
            if (sFps != nullptr)
                sFps->setCaption(std::to_string(int(fps + 0.5))); // the number alone (test #69)
        }

        // How many of an item the player carries, every stack (a quick key shows it, as the inventory does).
        int countItem(const ESM::RefId& id)
        {
            const MWWorld::Ptr player = MWMechanics::getPlayer();
            MWWorld::InventoryStore& store = player.getClass().getInventoryStore(player);
            int count = 0;
            for (MWWorld::ContainerStoreIterator it = store.begin(); it != store.end(); ++it)
                if (it->getCellRef().getRefId() == id)
                    count += it->getCellRef().getCount();
            return count;
        }

        MWWorld::Ptr findItem(const ESM::RefId& id)
        {
            const MWWorld::Ptr player = MWMechanics::getPlayer();
            MWWorld::InventoryStore& store = player.getClass().getInventoryStore(player);
            for (MWWorld::ContainerStoreIterator it = store.begin(); it != store.end(); ++it)
                if (it->getCellRef().getRefId() == id)
                    return *it;
            return MWWorld::Ptr();
        }

        // As QuickKeysMenu shows a spell: the first effect's big icon in the magic frame.
        void showSpell(MWGui::ItemWidget& icon, const ESM::RefId& spellId)
        {
            const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
            const ESM::Spell* spell = store.get<ESM::Spell>().search(spellId);
            if (spell == nullptr || spell->mEffects.mList.empty())
                return;
            const ESM::MagicEffect* effect
                = store.get<ESM::MagicEffect>().search(spell->mEffects.mList.front().mData.mEffectID);
            if (effect == nullptr)
                return;
            const VFS::Path::Normalized path = Misc::ResourceHelpers::correctBigIconPath(
                effect->mIcon.getNormalized(), *MWBase::Environment::get().getResourceSystem()->getVFS());
            float scale = 1.f;
            if (MyGUI::ITexture* frame
                = MyGUI::RenderManager::getInstance().getTexture("textures\\menu_icon_select_magic.dds"))
                scale = frame->getHeight() / 64.f;
            const int diameter = static_cast<int>(44 * scale);
            icon.setFrame("textures\\menu_icon_select_magic.dds", MyGUI::IntCoord(0, 0, diameter, diameter));
            icon.setIcon(path);
        }

        void updateStrip()
        {
            if (!bindStrip())
                return;
            updateFps();
            // The items' counts (the tester, 20/10: "10 potions on a quick key"), ~3 times a second.
            static unsigned frames = 0;
            const bool recount = ++frames % 10 == 0;
            for (int i = 0; i < kSlots; ++i)
            {
                Slot& slot = sSlots[i];
                ESM::QuickKeys::Type type;
                ESM::RefId id;
                Hud3DS::quickKeyState(i, type, id);
                const bool item = type == ESM::QuickKeys::Type::Item || type == ESM::QuickKeys::Type::MagicItem;
                if (slot.mShown && slot.mType == type && slot.mId == id)
                {
                    if (item && recount)
                    {
                        const int count = countItem(id);
                        if (count == 0 && slot.mCount != 0)
                            slot.mShown = false; // run out: shown again (empty) next frame
                        else if (count != slot.mCount)
                        {
                            slot.mCount = count;
                            slot.mIcon->setCount(count);
                        }
                    }
                    continue;
                }
                slot.mCount = -1;
                slot.mType = type;
                slot.mId = id;
                slot.mShown = true;
                slot.mIcon->setItem(MWWorld::Ptr());
                switch (type)
                {
                    case ESM::QuickKeys::Type::Item:
                    case ESM::QuickKeys::Type::MagicItem:
                        slot.mIcon->setItem(findItem(id));
                        slot.mCount = countItem(id);
                        slot.mIcon->setCount(std::max(slot.mCount, 1)); // 1 shows nothing (getCountString), 0 too
                        break;
                    case ESM::QuickKeys::Type::Magic:
                        showSpell(*slot.mIcon, id);
                        break;
                    case ESM::QuickKeys::Type::HandToHand:
                        slot.mIcon->setIcon(VFS::Path::Normalized("icons/k/stealth_handtohand.dds"));
                        break;
                    default:
                        break;
                }
            }
        }

        // ------------------------------------------------------------------ quick-key picker (pages)

        // A page's "Quick key" button and the row of nine numbered slots that replaces it once touched.
        class QuickKeyPicker
        {
        public:
            void bind(MWGui::Layout& layout, std::function<void(int)> assign, std::vector<MyGUI::Widget*> hide = {})
            {
                layout.getWidget(mButton, "QuickKeyButton3DS");
                layout.getWidget(mRow, "QuickSlots3DS");
                for (int i = 0; i < kSlots; ++i)
                {
                    layout.getWidget(mSlotButtons[i], "QuickSlot3DS" + std::to_string(i + 1));
                    mSlotButtons[i]->eventMouseButtonClick += MyGUI::newDelegate(this, &QuickKeyPicker::onSlot);
                }
                mButton->eventMouseButtonClick += MyGUI::newDelegate(this, &QuickKeyPicker::onButton);
                mAssign = std::move(assign);
                mHide = std::move(hide);
            }

            // Back to the button (shown if `available`).
            void reset(bool available)
            {
                mRow->setVisible(false);
                mButton->setVisible(available);
                for (MyGUI::Widget* widget : mHide)
                    widget->setVisible(available);
            }

        private:
            void onButton(MyGUI::Widget*)
            {
                mButton->setVisible(false);
                for (MyGUI::Widget* widget : mHide)
                    widget->setVisible(false);
                mRow->setVisible(true);
            }

            void onSlot(MyGUI::Widget* sender)
            {
                for (int i = 0; i < kSlots; ++i)
                    if (mSlotButtons[i] == sender)
                        mAssign(i);
                reset(true);
            }

            MyGUI::Button* mButton = nullptr;
            MyGUI::Widget* mRow = nullptr;
            std::array<MyGUI::Button*, kSlots> mSlotButtons{};
            std::function<void(int)> mAssign;
            std::vector<MyGUI::Widget*> mHide;
        };

        // ------------------------------------------------------------------ Inventory page

        class ItemsPage : public MWGui::Layout
        {
        public:
            ItemsPage()
                : Layout("openmw_3ds_items.layout")
            {
                getWidget(mItemView, "ItemView3DS");
                getWidget(mName, "ItemName3DS");
                getWidget(mInfo, "ItemInfo3DS");
                getWidget(mUse, "UseButton3DS");
                getWidget(mDrop, "DropButton3DS");
                getWidget(mEncumbrance, "Encumbrance3DS");
                getWidget(mGold, "Gold3DS");
                getWidget(mDetails, "Details3DS");
                mItemView->eventItemClicked += MyGUI::newDelegate(this, &ItemsPage::onItemClicked);
                mUse->eventMouseButtonClick += MyGUI::newDelegate(this, &ItemsPage::onUse);
                mDrop->eventMouseButtonClick += MyGUI::newDelegate(this, &ItemsPage::onDrop);
                const std::pair<const char*, int> filters[] = {
                    { "FilterAll3DS", MWGui::SortFilterItemModel::Category_All },
                    { "FilterWeapon3DS", MWGui::SortFilterItemModel::Category_Weapon },
                    { "FilterApparel3DS", MWGui::SortFilterItemModel::Category_Apparel },
                    { "FilterMagic3DS", MWGui::SortFilterItemModel::Category_Magic },
                    { "FilterMisc3DS", MWGui::SortFilterItemModel::Category_Misc },
                };
                for (const auto& [name, category] : filters)
                {
                    MyGUI::Button* button = nullptr;
                    getWidget(button, name);
                    button->setUserData(category);
                    button->eventMouseButtonClick += MyGUI::newDelegate(this, &ItemsPage::onFilter);
                    mFilters.push_back(button);
                }
                mPicker.bind(*this, [this](int slot) {
                    if (!mSelected.isEmpty())
                        Hud3DS::assignItem(slot, mSelected);
                },
                    { mUse, mDrop });
                setVisible(false);
            }

            void show()
            {
                const MWWorld::Ptr player = MWMechanics::getPlayer();
                if (player != mPlayer) // a new game or a loaded one: another player object
                {
                    auto model = std::make_unique<MWGui::SortFilterItemModel>(
                        std::make_unique<MWGui::InventoryItemModel>(player));
                    mModel = model.get();
                    mItemView->setModel(std::move(model));
                    mModel->setCategory(mCategory);
                    mPlayer = player;
                }
                select(MWWorld::Ptr());
                refresh();
                setVisible(true);
            }

            void hide() { setVisible(false); }

            // While looting or trading, the button hint strip of the window on the top screen takes the bottom 18
            // rows (M17, test #97): the page is that much shorter, its item grid too.
            void setLooting(bool looting)
            {
                if (looting == mLooting)
                    return;
                mLooting = looting;
                const int cut = looting ? 18 : 0;
                mMainWidget->setSize(mMainWidget->getWidth(), 218 - cut);
                static_cast<MyGUI::Widget*>(mItemView)->setSize(mItemView->getWidth(), 128 - cut);
                mDetails->setPosition(mDetails->getLeft(), 156 - cut);
            }

            // While shown: the inventory changes under it (pickups, quick keys, spells). Rebuilding the item
            // widgets loses a touch in progress, so only when something changed.
            void tick()
            {
                if (++mFrames % 10 != 0)
                    return;
                const size_t signature = inventorySignature();
                if (signature != mSignature)
                    refresh();
            }

        private:
            // The items, their counts and what is equipped.
            size_t inventorySignature() const
            {
                const MWWorld::Ptr player = MWMechanics::getPlayer();
                MWWorld::InventoryStore& store = player.getClass().getInventoryStore(player);
                size_t signature = 0;
                for (MWWorld::ContainerStoreIterator it = store.begin(); it != store.end(); ++it)
                    signature = signature * 31 + reinterpret_cast<size_t>(it->getBase())
                        + static_cast<size_t>(it->getCellRef().getCount());
                for (int slot = 0; slot < MWWorld::InventoryStore::Slots; ++slot)
                {
                    const MWWorld::ContainerStoreIterator it = store.getSlot(slot);
                    signature = signature * 31 + (it != store.end() ? reinterpret_cast<size_t>(it->getBase()) : 0);
                }
                return signature;
            }

            void refresh()
            {
                if (mModel == nullptr)
                    return;
                mSignature = inventorySignature();
                mItemView->update();
                if (!mSelected.isEmpty() && findStack() < 0)
                    select(MWWorld::Ptr());
                else
                    showDetails();
                for (MyGUI::Button* button : mFilters)
                    button->setStateSelected(*button->getUserData<int>() == mCategory);
            }

            int findStack() const
            {
                for (size_t i = 0; i < mModel->getItemCount(); ++i)
                    if (mModel->getItem(i).mBase == mSelected)
                        return static_cast<int>(i);
                return -1;
            }

            void select(const MWWorld::Ptr& item)
            {
                mSelected = item;
                mPicker.reset(!item.isEmpty());
                showDetails();
            }

            void showDetails()
            {
                const MWWorld::Ptr player = MWMechanics::getPlayer();
                const bool selected = !mSelected.isEmpty();
                mEncumbrance->setVisible(!selected);
                mGold->setVisible(!selected);
                if (!selected)
                {
                    mName->setCaption({});
                    mInfo->setCaption({});
                    mUse->setVisible(false);
                    mDrop->setVisible(false);
                    const float capacity = player.getClass().getCapacity(player);
                    const float encumbrance = player.getClass().getEncumbrance(player);
                    mEncumbrance->setValue(static_cast<int>(encumbrance), static_cast<int>(capacity));
                    mGold->setCaption(MyGUI::utility::toString(
                        player.getClass().getContainerStore(player).count(MWWorld::ContainerStore::sGoldId)));
                    return;
                }
                const int index = findStack();
                const size_t count = index >= 0 ? mModel->getItem(index).mCount : 1;
                const bool equipped = index >= 0 && mModel->getItem(index).mType == MWGui::ItemStack::Type_Equipped;
                std::string name(mSelected.getClass().getName(mSelected));
                if (count > 1)
                    name += " (" + std::to_string(count) + ")";
                mName->setCaption(name);
                // As the item's tooltip words them. (Not a char array: MyGUI::UString takes those whole, as literals.)
                mInfo->setCaptionWithReplacing("W: " // "W: / V:" (test #67, the tester)
                    + MWGui::ToolTips::toString(mSelected.getClass().getWeight(mSelected)) + "  V: "
                    + MWGui::ToolTips::toString(mSelected.getClass().getValue(mSelected)));
                const bool wearable = !mSelected.getClass().getEquipmentSlots(mSelected).first.empty();
                if (bartering())
                {
                    // Bartering: the item is offered to the merchant (the barter window's deal on the top screen);
                    // with a companion, given to them.
                    mPicker.reset(false); // no Quick key either (it hides Use and Drop too)
                    mUse->setVisible(true);
                    mUse->setCaptionWithReplacing(windows().getMode() == MWGui::GM_Companion ? "#{OMW3DS:Give}"
                            : windows().getMode() == MWGui::GM_Container                   ? "#{OMW3DS:Put}"
                                                                                           : "#{Interface:Sell}");
                    return;
                }
                mUse->setCaptionWithReplacing(
                    equipped ? "#{OMW3DS:Unequip}" : (wearable ? "#{sEquip}" : "#{sUse}"));
            }

            void onItemClicked(MWGui::ItemModel::ModelIndex index)
            {
                const MWWorld::Ptr item = mModel->getItem(index).mBase;
                // A quick second touch on the selected item uses or equips it (test #67, the tester); a slow one
                // clears the selection.
                static uint64_t lastTick = 0;
                static MWWorld::Ptr lastItem;
                const uint64_t now = svcGetSystemTick();
                const bool quick = item == lastItem && now - lastTick < SYSCLOCK_ARM11 / 2;
                lastTick = now;
                lastItem = item;
                if (item == mSelected && quick)
                {
                    lastTick = 0;
                    onUse(nullptr);
                    return;
                }
                select(item == mSelected ? MWWorld::Ptr() : item);
            }

            void onFilter(MyGUI::Widget* sender)
            {
                mCategory = *sender->getUserData<int>();
                if (mModel != nullptr)
                    mModel->setCategory(mCategory);
                select(MWWorld::Ptr());
                refresh();
            }

            // As in the inventory window: equips, unequips, or uses (potions, books, scrolls, ingredients...).
            static bool bartering() // or trading with a companion, or a container open (M17: the item goes in)
            {
                return windows().isGuiMode()
                    && (windows().getMode() == MWGui::GM_Barter || windows().getMode() == MWGui::GM_Companion
                        || windows().getMode() == MWGui::GM_Container);
            }

            void onUse(MyGUI::Widget*)
            {
                if (mSelected.isEmpty())
                    return;
                if (bartering())
                {
                    Hud3DS::sell(mSelected);
                    refresh();
                    return;
                }
                const int index = findStack();
                const MWWorld::Ptr player = MWMechanics::getPlayer();
                if (index >= 0 && mModel->getItem(index).mType == MWGui::ItemStack::Type_Equipped)
                    player.getClass().getInventoryStore(player).unequipItem(mSelected);
                else if (MWGui::InventoryWindow* inventory = Hud3DS::inventory())
                    inventory->useItem(mSelected);
                refresh();
            }

            // One item (a stack: all of it) dropped at the player's feet, as dropping from the inventory does.
            void onDrop(MyGUI::Widget*)
            {
                const int index = findStack();
                if (mSelected.isEmpty() || index < 0)
                    return;
                const int count = static_cast<int>(mModel->getItem(index).mCount);
                const MWWorld::Ptr player = MWMechanics::getPlayer();
                MWBase::World* world = MWBase::Environment::get().getWorld();
                world->breakInvisibility(player);
                world->dropObjectOnGround(player, mSelected, count);
                windows().playSound(mSelected.getClass().getDownSoundId(mSelected));
                player.getClass().getContainerStore(player).remove(mSelected, count);
                select(MWWorld::Ptr());
                refresh();
            }

            MWGui::ItemView* mItemView = nullptr;
            MyGUI::TextBox* mName = nullptr;
            MyGUI::TextBox* mInfo = nullptr;
            MyGUI::Button* mUse = nullptr;
            MyGUI::Button* mDrop = nullptr;
            MWGui::Widgets::MWDynamicStat* mEncumbrance = nullptr;
            MyGUI::TextBox* mGold = nullptr;
            std::vector<MyGUI::Button*> mFilters;
            QuickKeyPicker mPicker;
            MWGui::SortFilterItemModel* mModel = nullptr; // owned by mItemView
            MWWorld::Ptr mPlayer;
            MWWorld::Ptr mSelected;
            int mCategory = MWGui::SortFilterItemModel::Category_All;
            unsigned mFrames = 0;
            size_t mSignature = 0;
            MyGUI::Widget* mDetails = nullptr;
            bool mLooting = false;
        };

        // ------------------------------------------------------------------ tabs

        // ------------------------------------------------------------------ tabs

        Tab sTab = Tab::None;
        std::unique_ptr<MWGui::Layout> sTabBar;
        std::array<MyGUI::Button*, 4> sTabButtons{}; // Home, then the tabs
        MyGUI::ImageBox* sHomeIcon = nullptr;
        std::array<MyGUI::Colour, 2> sHomeColours; // as MW_Button's caption: normal, selected
        int sHomeColour = -1; // which one the icon has
        std::unique_ptr<ItemsPage> sItems;
        QuickKeyPicker sMagicPicker;
        const MWGui::SpellWindow* sMagicBoundTo = nullptr;
        Tab sShown = Tab::None;

        // Home shows the HUD; a tab shows its page, or the HUD again if it's already shown.
        void onTabClicked(MyGUI::Widget* sender)
        {
            const Tab tab = *sender->getUserData<Tab>();
            sTab = sTab == tab ? Tab::None : tab;
        }

        MyGUI::Colour fontColour(const char* tag)
        {
            return MyGUI::Colour::parse(MyGUI::LanguageManager::getInstance().replaceTags(tag));
        }

        bool bindTabs()
        {
            if (sTabBar != nullptr)
                return true;
            if (!has3DSLayouts())
                return false;
            sTabBar = std::make_unique<MWGui::Layout>("openmw_3ds_tabs.layout");
            const std::pair<const char*, Tab> tabs[] = {
                { "TabHome3DS", Tab::None },
                { "TabItems3DS", Tab::Items },
                { "TabMagic3DS", Tab::Magic },
                { "TabStats3DS", Tab::Stats },
            };
            for (size_t i = 0; i < std::size(tabs); ++i)
            {
                sTabBar->getWidget(sTabButtons[i], tabs[i].first);
                sTabButtons[i]->setUserData(tabs[i].second);
                sTabButtons[i]->eventMouseButtonClick += MyGUI::newDelegate(&onTabClicked);
            }
            sTabBar->getWidget(sHomeIcon, "TabHomeIcon3DS");
            sHomeColours = { fontColour("#{fontcolour=normal}"), fontColour("#{fontcolour=active}") };
            sItems = std::make_unique<ItemsPage>();
            return true;
        }

        void bindMagicPage(MWGui::SpellWindow& window)
        {
            if (&window == sMagicBoundTo)
                return;
            sMagicPicker.bind(window, [](int slot) { Hud3DS::assignSelectedMagic(slot); });
            sMagicBoundTo = &window;
        }

        // OpenMW's window as a page: under the tab bar, whatever its settings say.
        void showWindow(MWGui::WindowBase* window, bool show)
        {
            if (window == nullptr)
                return;
            if (show)
                window->mMainWidget->setCoord(kBottomX, kPanelY, kPageWidth, kPageHeight);
            if (window->isVisible() != show)
                window->setVisible(show);
        }

        void updateTabs()
        {
            if (!bindTabs())
                return;
            // In game mode, while the GUI draws the bottom screen; OpenMW's menus (dialogue, containers...) still go
            // to the M6 text menus.
            // ... and while a container is open (M8: its window on the top screen, the Inventory tab below).
            // ... and while bartering (the merchant's window on the top screen, test #74).
            const bool looting = windows().isGuiMode()
                && (windows().getMode() == MWGui::GM_Container || windows().getMode() == MWGui::GM_Barter
                    || windows().getMode() == MWGui::GM_Companion);
            const bool active = Render3DS::guiOwnsBottomScreen() && (!windows().isGuiMode() || looting);
            const Tab tab = !active ? Tab::None : looting ? Tab::Items : sTab;
            Render3DS::setHudOnly(active && !looting && sTab == Tab::None && !windows().isGuiMode());
            sTabBar->setVisible(active);
            for (MyGUI::Button* button : sTabButtons)
                button->setStateSelected(*button->getUserData<Tab>() == sTab);
            const int homeColour = sTab == Tab::None ? 1 : 0;
            if (homeColour != sHomeColour)
            {
                sHomeIcon->setColour(sHomeColours[homeColour]);
                sHomeColour = homeColour;
            }

            sItems->setLooting(looting);
            if (tab != sShown)
            {
                if (tab == Tab::Items)
                    sItems->show();
                else
                    sItems->hide();
                if (MWGui::SpellWindow* magic = Hud3DS::magic())
                {
                    bindMagicPage(*magic);
                    sMagicPicker.reset(true);
                }
                sShown = tab;
            }
            showWindow(Hud3DS::magic(), tab == Tab::Magic);
            showWindow(Hud3DS::stats(), tab == Tab::Stats);
            // A page replaces the HUD panel: OpenMW's own HUD toggle (the one bound to F12 on the PC), which the
            // window manager applies on every mode change; hiding the HUD window directly broke its layout.
            static bool hudHidden = false;
            const bool noGame = MWBase::Environment::get().getStateManager()->getState()
                == MWBase::StateManager::State_NoGame; // the main menu before a game: nothing of the HUD
            // ... and under every window that pauses the game (the character creation ones, dialogue, containers:
            // Morrowind's HUD is not drawn under them on the small screen).
            const bool hideHud = noGame || windows().isGuiMode();
            if (hideHud != hudHidden)
            {
                windows().setHudVisibility(!hideHud);
                hudHidden = hideHud;
            }
            // A page replaces only the HUD's bottom panel: the top screen keeps the crosshair, the cell name and
            // the target label (test #67, the tester).
            static MyGUI::Widget* bottomPanel = nullptr;
            static const MWGui::HUD* panelBoundTo = nullptr;
            if (MWGui::HUD* hud = Hud3DS::hud(); hud != panelBoundTo)
            {
                bottomPanel = nullptr;
                hud->getWidget(bottomPanel, "Bottom3DS");
                panelBoundTo = hud;
            }
            if (bottomPanel != nullptr)
            {
                const bool showPanel = !(active && tab != Tab::None);
                if (bottomPanel->getVisible() != showPanel)
                    bottomPanel->setVisible(showPanel);
            }
            if (tab == Tab::Items)
                sItems->tick();
        }

        // ------------------------------------------------------------------ HUD panel

        // What the 3DS HUD panel adds to OpenMW's HUD: the values on the health, magicka and fatigue bars, and the
        // names of the weapon and the spell next to their boxes (OpenMW shows a name for a few seconds when it
        // changes). Touching the bars, the weapon or the spell opens their tab.
        struct Panel
        {
            const MWGui::HUD* mBoundTo = nullptr;
            std::array<MyGUI::TextBox*, 3> mValues{};
            std::array<std::string, 3> mValueText;
            MyGUI::EditBox* mWeaponName = nullptr;
            MyGUI::EditBox* mSpellName = nullptr;
            std::string mWeaponText;
            std::string mSpellText;
            // HUD::updatePositions moves the boxes left when one is hidden (PC's single row); here they keep their
            // places, in two columns.
            std::array<std::pair<MyGUI::Widget*, MyGUI::IntPoint>, 3> mBoxes{};
            // The Skyrim-like bars on the top screen ([HUD] top bars): health, magicka, fatigue, and what they show
            // (a change of position redraws the interface, so only real changes reach them).
            MyGUI::Widget* mTopBars = nullptr;
            std::array<MyGUI::ProgressBar*, 3> mTopBar{};
            std::array<std::pair<size_t, size_t>, 3> mTopShown{};
            std::array<MyGUI::Widget*, 3> mBottomBars{}; // the bottom screen's bars, hidden while those show
            MyGUI::Widget* mCellName = nullptr;
            MyGUI::IntPoint mCellNamePosition;
        };

        Panel sPanel;

        void openTab(Tab tab)
        {
            sTab = tab;
        }

        void bindPanel(MWGui::HUD& hud)
        {
            if (&hud == sPanel.mBoundTo)
                return;
            sPanel = Panel{};
            const char* values[] = { "HealthValue3DS", "MagickaValue3DS", "FatigueValue3DS" };
            for (size_t i = 0; i < sPanel.mValues.size(); ++i)
                hud.getWidget(sPanel.mValues[i], values[i]);
            hud.getWidget(sPanel.mTopBars, "TopBars3DS");
            const char* topBars[] = { "TopHealth3DS", "TopMagicka3DS", "TopFatigue3DS" };
            for (size_t i = 0; i < sPanel.mTopBar.size(); ++i)
                hud.getWidget(sPanel.mTopBar[i], topBars[i]);
            const char* bottomBars[] = { "HealthFrame", "MagickaFrame", "FatigueFrame" };
            for (size_t i = 0; i < sPanel.mBottomBars.size(); ++i)
                hud.getWidget(sPanel.mBottomBars[i], bottomBars[i]);
            hud.getWidget(sPanel.mCellName, "CellName");
            sPanel.mCellNamePosition = sPanel.mCellName->getPosition();
            hud.getWidget(sPanel.mWeaponName, "WeaponName3DS");
            hud.getWidget(sPanel.mSpellName, "SpellName3DS");
            const char* boxes[] = { "WeapBox", "SpellBox", "SneakBox" };
            for (size_t i = 0; i < sPanel.mBoxes.size(); ++i)
            {
                MyGUI::Widget* box = hud.getWidget(boxes[i]);
                sPanel.mBoxes[i] = { box, box->getPosition() };
            }

            // OpenMW's handlers toggle the windows shown with the inventory menu (HUD::onWeaponClicked...).
            const std::pair<const char*, void (*)(MyGUI::Widget*)> touches[] = {
                { "HealthFrame", [](MyGUI::Widget*) { openTab(Tab::Stats); } },
                { "MagickaFrame", [](MyGUI::Widget*) { openTab(Tab::Stats); } },
                { "FatigueFrame", [](MyGUI::Widget*) { openTab(Tab::Stats); } },
                { "WeapBox", [](MyGUI::Widget*) { openTab(Tab::Items); } },
                { "SpellBox", [](MyGUI::Widget*) { openTab(Tab::Magic); } },
                { "MiniMapButton", nullptr }, // the Map tab, later
                { "Screenshot3DS", [](MyGUI::Widget*) { requestPlayerScreenshot3DS(); } },
            };
            for (const auto& [name, handler] : touches)
            {
                MyGUI::Widget* widget = hud.getWidget(name);
                widget->eventMouseButtonClick.clear();
                if (handler != nullptr)
                    widget->eventMouseButtonClick += MyGUI::newDelegate(handler);
            }
            // The screenshot button: with the start menu's experimental option only; its camera in the captions'
            // colour, as the Home tab's house.
            hud.getWidget("Screenshot3DS")->setVisible(screenshotButtonEnabled3DS());
            hud.getWidget("ScreenshotIcon3DS")->setColour(fontColour("#{fontcolour=normal}"));
            sPanel.mBoundTo = &hud;
        }

        void setText(MyGUI::Widget& widget, std::string& shown, const std::string& text)
        {
            if (text == shown)
                return;
            shown = text;
            widget.castType<MyGUI::TextBox>()->setCaptionWithReplacing(text);
        }

        void updatePanel()
        {
            if (!has3DSLayouts())
                return;
            bindPanel(*Hud3DS::hud());

            const MWWorld::Ptr player = MWMechanics::getPlayer();
            const MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
            const MWMechanics::DynamicStat<float> values[] = { stats.getHealth(), stats.getMagicka(),
                stats.getFatigue() };
            for (size_t i = 0; i < sPanel.mValues.size(); ++i)
            {
                int current = static_cast<int>(values[i].getCurrent());
                if (i != 2) // fatigue can be negative
                    current = std::max(0, current);
                setText(*sPanel.mValues[i], sPanel.mValueText[i],
                    std::to_string(current) + "/" + std::to_string(static_cast<int>(values[i].getModified())));
            }

            const bool topBars = Settings::hud().mTopBars3DS;
            if (sPanel.mTopBars->getVisible() != topBars)
            {
                sPanel.mTopBars->setVisible(topBars);
                for (MyGUI::Widget* bar : sPanel.mBottomBars) // one set of bars at a time (the tester, test #78)
                    bar->setVisible(!topBars);
                // The cell name, shown at the top screen's lower right when it changes, goes above the bars.
                sPanel.mCellName->setPosition(sPanel.mCellNamePosition - MyGUI::IntPoint(0, topBars ? 14 : 0));
            }
            if (topBars)
                for (size_t i = 0; i < sPanel.mTopBar.size(); ++i)
                {
                    const size_t range = static_cast<size_t>(std::max(0, static_cast<int>(values[i].getModified())));
                    const size_t position = std::min(
                        range, static_cast<size_t>(std::max(0, static_cast<int>(values[i].getCurrent()))));
                    if (sPanel.mTopShown[i] == std::make_pair(range, position))
                        continue;
                    sPanel.mTopShown[i] = { range, position };
                    sPanel.mTopBar[i]->setProgressRange(range);
                    sPanel.mTopBar[i]->setProgressPosition(position);
                }

            const auto [weaponVisible, spellVisible] = Hud3DS::boxesVisible();
            sPanel.mWeaponName->setVisible(weaponVisible);
            sPanel.mSpellName->setVisible(spellVisible);
            const auto [weaponName, spellName] = Hud3DS::selectedNames();
            setText(*sPanel.mWeaponName, sPanel.mWeaponText, weaponName);
            setText(*sPanel.mSpellName, sPanel.mSpellText, spellName);

            for (const auto& [box, position] : sPanel.mBoxes)
                if (box->getPosition() != position)
                    box->setPosition(position);
        }
    }

    MyGUI::IntPoint targetLabel3DS(const MyGUI::IntSize& size)
    {
        return MyGUI::IntPoint((Render3DS::kGuiCanvasWidth - size.width) / 2, kLabelTop);
    }

    std::string targetLabelText3DS(const std::string& caption, const std::string& text)
    {
        // Tags first, to see the lines as shown: a line starting in lower case continues the previous one, and so
        // does the line after it ("to", then the destination); the others are separate details.
        std::string label = caption;
        bool continues = false;
        const std::string lines = MyGUI::LanguageManager::getInstance().replaceTags(text);
        size_t start = 0;
        while (start <= lines.size())
        {
            size_t end = lines.find('\n', start);
            if (end == std::string::npos)
                end = lines.size();
            const std::string line = lines.substr(start, end - start);
            start = end + 1;
            if (line.empty())
                continue;
            const bool lower = std::islower(static_cast<unsigned char>(line[0])) != 0;
            if (!label.empty())
                label += lower || continues ? " " : ", ";
            continues = lower;
            label += line;
        }
        return label;
    }

    void updateHud3DS()
    {
        if (!Hud3DS::ready())
            return;
        updateStrip();
        updatePanel();
        updateTabs();
    }

    void onLoadingTick3DS()
    {
        if (Hud3DS::ready())
            updateTabs();
    }

    bool toggleInventoryTab3DS()
    {
        if (sTabBar == nullptr)
            return false;
        sTab = sTab == Tab::Items ? Tab::None : Tab::Items;
        return true;
    }
}
