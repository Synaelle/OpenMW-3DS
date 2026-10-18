// Nintendo 3DS HUD and bottom-screen tabs (see hud3ds.hpp).

#include "hud3ds.hpp"
#include "frame3ds.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <memory>
#include <string>

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_Gui.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_LanguageManager.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_ResourceManager.h>
#include <MyGUI_TextBox.h>
#include <MyGUI_Window.h>


#include <components/esm3/loadench.hpp>
#include <components/esm3/loadmgef.hpp>
#include <components/esm3/loadspel.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/render3ds/render3ds.hpp>
#include <components/resource/resourcesystem.hpp>

#include "../mwbase/environment.hpp"
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
            Debug, // debug builds
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
        };

        std::array<Slot, kSlots> sSlots;
        const MWGui::HUD* sStripBoundTo = nullptr;

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
            sStripBoundTo = hud;
            return true;
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
            for (int i = 0; i < kSlots; ++i)
            {
                Slot& slot = sSlots[i];
                ESM::QuickKeys::Type type;
                ESM::RefId id;
                Hud3DS::quickKeyState(i, type, id);
                if (slot.mShown && slot.mType == type && slot.mId == id)
                    continue;
                slot.mType = type;
                slot.mId = id;
                slot.mShown = true;
                slot.mIcon->setItem(MWWorld::Ptr());
                switch (type)
                {
                    case ESM::QuickKeys::Type::Item:
                    case ESM::QuickKeys::Type::MagicItem:
                        slot.mIcon->setItem(findItem(id));
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
                mInfo->setCaptionWithReplacing("#{sWeight}: "
                    + MWGui::ToolTips::toString(mSelected.getClass().getWeight(mSelected)) + "   #{sValue}: "
                    + MWGui::ToolTips::toString(mSelected.getClass().getValue(mSelected)));
                const bool wearable = !mSelected.getClass().getEquipmentSlots(mSelected).first.empty();
                mUse->setCaptionWithReplacing(
                    equipped ? "#{OMW3DS:Unequip}" : (wearable ? "#{sEquip}" : "#{sUse}"));
            }

            void onItemClicked(MWGui::ItemModel::ModelIndex index)
            {
                const MWWorld::Ptr item = mModel->getItem(index).mBase;
                select(item == mSelected ? MWWorld::Ptr() : item); // a second touch clears the selection
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
            void onUse(MyGUI::Widget*)
            {
                if (mSelected.isEmpty())
                    return;
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
        };

        // ------------------------------------------------------------------ tabs

        // ------------------------------------------------------------------ Debug page

        // The debug screen's lines (frame3ds.cpp) while the GUI draws the bottom screen.
        class DebugPage : public MWGui::Layout
        {
        public:
            DebugPage()
                : Layout("openmw_3ds_debug.layout")
            {
                getWidget(mText, "DebugText3DS");
                MyGUI::Button* screenshot = nullptr;
                getWidget(screenshot, "ScreenshotButton3DS");
                screenshot->eventMouseButtonClick += MyGUI::newDelegate(this, &DebugPage::onScreenshot);
                setVisible(false);
            }

            void show()
            {
                mShown.clear();
                tick();
                setVisible(true);
            }

            void hide() { setVisible(false); }

            // The debug screen redraws at 4 Hz.
            void tick()
            {
                if (mFrames++ % 15 != 0)
                    return;
                std::string text = debugScreenText3DS();
                if (text == mShown)
                    return;
                mShown = std::move(text);
                mText->setCaption(mShown);
            }

        private:
            void onScreenshot(MyGUI::Widget*) { requestScreenshot3DS(); }

            MyGUI::TextBox* mText = nullptr;
            std::string mShown;
            unsigned mFrames = 0;
        };

        // ------------------------------------------------------------------ tabs

        Tab sTab = Tab::None;
        std::unique_ptr<MWGui::Layout> sTabBar;
        std::array<MyGUI::Button*, 5> sTabButtons{}; // Home, then the tabs
        MyGUI::ImageBox* sHomeIcon = nullptr;
        std::array<MyGUI::Colour, 2> sHomeColours; // as MW_Button's caption: normal, selected
        int sHomeColour = -1; // which one the icon has
        std::unique_ptr<ItemsPage> sItems;
        std::unique_ptr<DebugPage> sDebug;
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
                { "TabDebug3DS", Tab::Debug },
            };
            for (size_t i = 0; i < std::size(tabs); ++i)
            {
                sTabBar->getWidget(sTabButtons[i], tabs[i].first);
                sTabButtons[i]->setUserData(tabs[i].second);
                sTabButtons[i]->eventMouseButtonClick += MyGUI::newDelegate(&onTabClicked);
            }
            sTabBar->getWidget(sHomeIcon, "TabHomeIcon3DS");
            sHomeColours = { fontColour("#{fontcolour=normal}"), fontColour("#{fontcolour=active}") };
#ifdef OPENMW3DS_DEBUG
            sDebug = std::make_unique<DebugPage>();
#else
            // No Debug tab: the other three share the bar, after Home.
            sTabButtons[4]->setVisible(false);
            for (int i = 1; i < 4; ++i)
                sTabButtons[i]->setCoord(28 + (i - 1) * 97, 1, 95, 21);
#endif
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
            const bool active = Render3DS::guiOwnsBottomScreen() && !windows().isGuiMode();
            const Tab tab = active ? sTab : Tab::None;
            sTabBar->setVisible(active);
            for (MyGUI::Button* button : sTabButtons)
                button->setStateSelected(*button->getUserData<Tab>() == sTab);
            const int homeColour = sTab == Tab::None ? 1 : 0;
            if (homeColour != sHomeColour)
            {
                sHomeIcon->setColour(sHomeColours[homeColour]);
                sHomeColour = homeColour;
            }

            if (tab != sShown)
            {
                if (tab == Tab::Items)
                    sItems->show();
                else
                    sItems->hide();
                if (sDebug != nullptr)
                {
                    if (tab == Tab::Debug)
                        sDebug->show();
                    else
                        sDebug->hide();
                }
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
            const bool hideHud = active && tab != Tab::None;
            if (hideHud != hudHidden)
            {
                windows().setHudVisibility(!hideHud);
                hudHidden = hideHud;
            }
            if (tab == Tab::Items)
                sItems->tick();
            else if (tab == Tab::Debug && sDebug != nullptr)
                sDebug->tick();
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
            };
            for (const auto& [name, handler] : touches)
            {
                MyGUI::Widget* widget = hud.getWidget(name);
                widget->eventMouseButtonClick.clear();
                if (handler != nullptr)
                    widget->eventMouseButtonClick += MyGUI::newDelegate(handler);
            }
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

#ifdef OPENMW3DS_LUMIERE
    // Deprecated 18/10/2026 (see backend.cpp): compiled only with OPENMW3DS_LUMIERE.
    // ------------------------------------------------------------------ the port's own interface (Lumiere)
    //
    // Hardware tests #34-#44: MyGUI's interface drawn through the GPU flashes nearby meshes (its texture changes),
    // drawn by the CPU it is too slow; the port's own interface (components/render3ds, Render3DS::Lumiere) draws
    // everything from one texture, as one draw per screen. This is its game side: the same HUD as the M8 layout
    // (packaging/ui3ds/mygui/openmw_hud.layout: the same places and sizes, Morrowind's frames and fonts), the tab
    // bar (openmw_3ds_tabs.layout) and the crosshair's target label on the top screen. The screens are rebuilt
    // only when what they show changes (a snapshot compared each frame); touches hit rectangles recorded while
    // the bottom screen was built. The tabs' pages come in phase 3b; until then a tab shows its title.
    namespace
    {
        namespace ui = Render3DS::Lumiere;

        constexpr ui::Rgba kGold{ 202, 165, 96, 255 }; // MW_Button's normal caption (fontcolour=normal)
        constexpr ui::Rgba kActive{ 223, 201, 159, 255 }; // fontcolour=active
        constexpr ui::Rgba kWhite{ 255, 255, 255, 255 };
        constexpr ui::Rgba kBarBack{ 45, 40, 36, 255 };
        constexpr ui::Rgba kHealth{ 141, 15, 15, 255 }; // MW_Progress_Red/Blue/Green's colours
        constexpr ui::Rgba kMagicka{ 15, 15, 141, 255 };
        constexpr ui::Rgba kFatigue{ 15, 141, 15, 255 };
        constexpr ui::Rgba kWeaponBar{ 148, 128, 56, 255 }; // MW_EnergyBar_Weapon
        constexpr ui::Rgba kMagicBar{ 56, 56, 200, 255 }; // MW_EnergyBar_Magic
        constexpr ui::Rgba kLabelBack{ 0, 0, 0, 150 };
        constexpr ui::Rgba kBarFrame{ 202, 165, 96, 255 };
        constexpr ui::Rgba kSelected{ 120, 100, 50, 110 };
        constexpr ui::Rgba kEquipped{ 90, 90, 90, 255 };
        constexpr int kFontText = 0; // 16 px: tab captions, names
        constexpr int kFontValue = 1; // 11 px: the bars' values
        constexpr int kFontSmall = 2; // 10 px: the target label

        enum class Action
        {
            None,
            Home,
            TabItems,
            TabMagic,
            TabStats,
            TabDebug,
            OpenStats,
            OpenItems,
            OpenMagic,
            QuickKey1, // + slot
            // the pages (phase 3b)
            Filter, // arg: SortFilterItemModel category
            Scroll, // arg: +1 / -1 (the open tab's list)
            SelectItem, // arg: model index
            UseItem,
            DropItem,
            AssignQuick, // arg: quick key slot 0-8 (the selected item, or the selected spell on the Magic tab)
            SelectSpell, // arg: index in the spell list
            QuickPick, // the "Quick key" button: show the 1-9 row
            Screenshot,
        };

        struct Zone
        {
            float mX, mY, mW, mH;
            Action mAction;
            int mArg;
        };

        struct UiState
        {
            Tab mTab = Tab::None;
            bool mActive = false; // game mode, the GUI owns the bottom screen
            std::array<int, 3> mCurrent{}, mMax{};
            bool mWeaponShown = false, mSpellShown = false, mSneaking = false;
            std::string mWeaponName, mSpellName, mWeaponIcon, mSpellIcon;
            int mWeaponHealth = -1, mSpellChance = -1; // percent
            std::vector<std::string> mEffectIcons;
            std::array<std::string, kSlots> mQuickIcons;
            float mYaw = 0;
            std::string mPage; // what the open tab shows (its own signature), "" on the HUD
            bool operator==(const UiState& o) const
            {
                if (mPage != o.mPage)
                    return false;
                return mTab == o.mTab && mActive == o.mActive && mCurrent == o.mCurrent && mMax == o.mMax
                    && mWeaponShown == o.mWeaponShown && mSpellShown == o.mSpellShown && mSneaking == o.mSneaking
                    && mWeaponName == o.mWeaponName && mSpellName == o.mSpellName && mWeaponIcon == o.mWeaponIcon
                    && mSpellIcon == o.mSpellIcon && mWeaponHealth == o.mWeaponHealth
                    && mSpellChance == o.mSpellChance && mEffectIcons == o.mEffectIcons
                    && mQuickIcons == o.mQuickIcons && std::abs(mYaw - o.mYaw) < 0.02f;
            }
        };

        UiState sUiShown;
        bool sUiBuilt = false;
        // the pages' own state
        int sInvCategory = MWGui::SortFilterItemModel::Category_All;
        int sInvScroll = 0; // first visible row
        MWWorld::Ptr sInvSelected;
        std::unique_ptr<MWGui::SortFilterItemModel> sInvModel;
        int sMagicScroll = 0;
        int sStatsScroll = 0;
        std::string sDebugText;
        unsigned sDebugFrame = 0;
        constexpr int kInvColumns = 7, kInvRows = 2; // 14 icons: the 20 slots are free while a tab is open
        constexpr int kMagicRows = 7, kSkillRows = 7;
        bool sQuickPick = false; // the 1-9 row shown instead of the buttons (Inventory, Magic)
        std::vector<Zone> sZones;
        std::string sTopLabel, sTopShown;
        unsigned sTopLabelFrame = 0, sUiFrame = 0;
        bool sTopBuilt = false;

        std::string iconKey(VFS::Path::NormalizedView path)
        {
            std::string key(path.value());
            const size_t dot = key.rfind('.');
            if (dot != std::string::npos)
                key.erase(dot);
            return key;
        }

        std::string itemIconKey(const MWWorld::ConstPtr& item)
        {
            if (item.isEmpty())
                return {};
            const VFS::Manager& vfs = *MWBase::Environment::get().getResourceSystem()->getVFS();
            return iconKey(Misc::ResourceHelpers::correctIconPath(item.getClass().getInventoryIcon(item), vfs));
        }

        std::string spellIconKey(const ESM::RefId& spellId, bool big)
        {
            const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
            const ESM::Spell* spell = store.get<ESM::Spell>().search(spellId);
            if (spell == nullptr || spell->mEffects.mList.empty())
                return {};
            const ESM::MagicEffect* effect
                = store.get<ESM::MagicEffect>().search(spell->mEffects.mList.front().mData.mEffectID);
            if (effect == nullptr)
                return {};
            const VFS::Manager& vfs = *MWBase::Environment::get().getResourceSystem()->getVFS();
            const auto icon = effect->mIcon.getNormalized();
            return iconKey(big ? Misc::ResourceHelpers::correctBigIconPath(icon, vfs)
                               : Misc::ResourceHelpers::correctIconPath(icon, vfs));
        }

        std::string effectIconKey(const ESM::RefId& effectId)
        {
            const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
            const ESM::MagicEffect* effect = store.get<ESM::MagicEffect>().search(effectId);
            if (effect == nullptr)
                return {};
            const VFS::Manager& vfs = *MWBase::Environment::get().getResourceSystem()->getVFS();
            return iconKey(Misc::ResourceHelpers::correctIconPath(effect->mIcon.getNormalized(), vfs));
        }

        std::string plainText(const std::string& tagged)
        {
            return MyGUI::LanguageManager::getInstance().replaceTags(tagged);
        }

        std::string pageSignature(Tab tab); // the pages, below

        UiState readUiState()
        {
            UiState s;
            s.mActive = Render3DS::guiOwnsBottomScreen() && !windows().isGuiMode();
            s.mTab = s.mActive ? sTab : Tab::None;
            const MWWorld::Ptr player = MWMechanics::getPlayer();
            const MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
            const MWMechanics::DynamicStat<float> values[]
                = { stats.getHealth(), stats.getMagicka(), stats.getFatigue() };
            for (size_t i = 0; i < 3; ++i)
            {
                int current = static_cast<int>(values[i].getCurrent());
                if (i != 2)
                    current = std::max(0, current);
                s.mCurrent[i] = current;
                s.mMax[i] = std::max(1, static_cast<int>(values[i].getModified()));
            }
            std::tie(s.mWeaponShown, s.mSpellShown) = Hud3DS::boxesVisible();
            const auto [weaponName, spellName] = Hud3DS::selectedNames();
            s.mWeaponName = plainText(weaponName);
            s.mSpellName = plainText(spellName);
            s.mSneaking = MWBase::Environment::get().getMechanicsManager()->isSneaking(player);
            s.mYaw = player.getRefData().getPosition().rot[2];

            MWWorld::InventoryStore& inventory = player.getClass().getInventoryStore(player);
            MWWorld::ContainerStoreIterator weapon = inventory.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
            if (weapon != inventory.end() && weapon->getType() == ESM::Weapon::sRecordId)
            {
                s.mWeaponIcon = itemIconKey(*weapon);
                if (weapon->getClass().hasItemHealth(*weapon))
                    s.mWeaponHealth = static_cast<int>(weapon->getClass().getItemNormalizedHealth(*weapon) * 100.f);
            }
            else
                s.mWeaponIcon = "icons/k/stealth_handtohand";

            const ESM::RefId& spell = windows().getSelectedSpell();
            const MWWorld::Ptr& enchanted = windows().getSelectedEnchantItem();
            if (!spell.empty())
            {
                s.mSpellIcon = spellIconKey(spell, true);
                if (const ESM::Spell* record
                    = MWBase::Environment::get().getESMStore()->get<ESM::Spell>().search(spell))
                    s.mSpellChance = static_cast<int>(MWMechanics::getSpellSuccessChance(record, player));
            }
            else if (!enchanted.isEmpty())
            {
                s.mSpellIcon = itemIconKey(enchanted);
                const ESM::RefId& enchantmentId = enchanted.getClass().getEnchantment(enchanted);
                const ESM::Enchantment* enchantment = enchantmentId.empty()
                    ? nullptr
                    : MWBase::Environment::get().getESMStore()->get<ESM::Enchantment>().search(enchantmentId);
                if (enchantment != nullptr && enchantment->mData.mCharge > 0)
                    s.mSpellChance = static_cast<int>(
                        100.f * enchanted.getCellRef().getEnchantmentCharge() / enchantment->mData.mCharge);
            }

            for (const auto& params : stats.getActiveSpells())
                for (const auto& effect : params.getEffects())
                {
                    if (!(effect.mFlags & ESM::ActiveEffect::Flag_Applied) || effect.mMagnitude <= 0)
                        continue;
                    std::string key = effectIconKey(effect.mEffectId);
                    if (!key.empty() && std::find(s.mEffectIcons.begin(), s.mEffectIcons.end(), key) == s.mEffectIcons.end())
                        s.mEffectIcons.push_back(std::move(key));
                    if (s.mEffectIcons.size() >= 8)
                        break;
                }

            if (s.mTab != Tab::None)
                s.mPage = pageSignature(s.mTab);
            for (int i = 0; i < kSlots; ++i)
            {
                ESM::QuickKeys::Type type;
                ESM::RefId id;
                Hud3DS::quickKeyState(i, type, id);
                switch (type)
                {
                    case ESM::QuickKeys::Type::Item:
                    case ESM::QuickKeys::Type::MagicItem:
                        s.mQuickIcons[i] = itemIconKey(findItem(id));
                        break;
                    case ESM::QuickKeys::Type::Magic:
                        s.mQuickIcons[i] = spellIconKey(id, true);
                        break;
                    case ESM::QuickKeys::Type::HandToHand:
                        s.mQuickIcons[i] = "icons/k/stealth_handtohand";
                        break;
                    default:
                        break;
                }
            }
            return s;
        }

        void zone(float x, float y, float w, float h, Action action, int arg = 0)
        {
            sZones.push_back(Zone{ x, y, w, h, action, arg });
        }

        // the drawing helpers, defined with the HUD below
        void buttonFrame(float x, float y, float w, float h);
        void button(float x, float y, float w, float h, std::string_view caption, bool selected, Action action, int arg = 0);
        void framedBar(float x, float y, float w, float h, float fill, ui::Rgba colour, std::string_view value);
        void bar(float x, float y, float w, float h, float fill, ui::Rgba colour);
        void separator(float x, float y, float w);

        // ---- the pages: what each tab shows, and a signature of it (the screen is rebuilt when it changes)

        MWGui::SortFilterItemModel& invModel()
        {
            if (sInvModel == nullptr)
            {
                const MWWorld::Ptr player = MWMechanics::getPlayer();
                sInvModel = std::make_unique<MWGui::SortFilterItemModel>(std::make_unique<MWGui::InventoryItemModel>(player));
            }
            sInvModel->setCategory(sInvCategory);
            sInvModel->update();
            return *sInvModel;
        }

        int invSelectedIndex(MWGui::SortFilterItemModel& model)
        {
            if (sInvSelected.isEmpty())
                return -1;
            for (size_t i = 0; i < model.getItemCount(); ++i)
                if (model.getItem(i).mBase == sInvSelected)
                    return static_cast<int>(i);
            sInvSelected = MWWorld::Ptr();
            return -1;
        }

        std::vector<const ESM::Spell*> playerSpells()
        {
            const MWWorld::Ptr player = MWMechanics::getPlayer();
            const MWMechanics::Spells& spells = player.getClass().getCreatureStats(player).getSpells();
            std::vector<const ESM::Spell*> list(spells.begin(), spells.end());
            std::sort(list.begin(), list.end(), [](const ESM::Spell* a, const ESM::Spell* b) {
                return a->mData.mType != b->mData.mType ? a->mData.mType < b->mData.mType : a->mName < b->mName;
            });
            return list;
        }

        std::string pageSignature(Tab tab)
        {
            std::string sig = std::to_string(static_cast<int>(tab)) + ":";
            const MWWorld::Ptr player = MWMechanics::getPlayer();
            switch (tab)
            {
                case Tab::Items:
                {
                    MWGui::SortFilterItemModel& model = invModel();
                    sig += std::to_string(sInvCategory) + "/" + std::to_string(sInvScroll) + "/" + (sQuickPick ? "q/" : "/")
                        + std::to_string(invSelectedIndex(model)) + "/" + std::to_string(model.getItemCount()) + "|";
                    const size_t first = static_cast<size_t>(sInvScroll) * kInvColumns;
                    for (size_t i = first; i < model.getItemCount() && i < first + kInvColumns * kInvRows; ++i)
                    {
                        const MWGui::ItemStack stack = model.getItem(i);
                        sig += stack.mBase.getCellRef().getRefId().toDebugString() + "," + std::to_string(stack.mCount)
                            + "," + std::to_string(static_cast<int>(stack.mType)) + ";";
                    }
                    sig += std::to_string(static_cast<int>(player.getClass().getEncumbrance(player)));
                    break;
                }
                case Tab::Magic:
                {
                    sig += std::to_string(sMagicScroll) + "/" + (sQuickPick ? "q/" : "/") + windows().getSelectedSpell().toDebugString() + "|";
                    const auto& spells = playerSpells();
                    for (size_t i = static_cast<size_t>(sMagicScroll); i < spells.size() && i < static_cast<size_t>(sMagicScroll + kMagicRows); ++i)
                        sig += spells[i]->mId.toDebugString() + ";";
                    break;
                }
                case Tab::Stats:
                {
                    const MWMechanics::NpcStats& stats = player.getClass().getNpcStats(player);
                    sig += std::to_string(sStatsScroll) + "/" + std::to_string(stats.getLevel()) + "|";
                    for (int i = 0; i < ESM::Attribute::Length; ++i)
                        sig += std::to_string(static_cast<int>(stats.getAttribute(ESM::Attribute::indexToRefId(i)).getModified())) + ",";
                    for (int i = 0; i < ESM::Skill::Length; ++i)
                        sig += std::to_string(static_cast<int>(stats.getSkill(ESM::Skill::indexToRefId(i)).getModified())) + ",";
                    break;
                }
                case Tab::Debug:
                    if (sUiFrame - sDebugFrame >= 15 || sDebugText.empty())
                    {
                        // without MyGUI's colour tags (#RRGGBB) and its tag references (#{...})
                        const std::string raw = debugScreenText3DS();
                        sDebugText.clear();
                        for (size_t i = 0; i < raw.size(); ++i)
                        {
                            if (raw[i] == '#' && i + 1 < raw.size() && raw[i + 1] == '{')
                            {
                                const size_t close = raw.find('}', i);
                                i = close == std::string::npos ? raw.size() : close;
                            }
                            else if (raw[i] == '#' && i + 6 < raw.size() + 0 && std::isxdigit(static_cast<unsigned char>(raw[i + 1])))
                                i += 6;
                            else
                                sDebugText += raw[i];
                        }
                        sDebugFrame = sUiFrame;
                    }
                    sig += sDebugText;
                    break;
                default:
                    break;
            }
            return sig;
        }

        void scrollArrows(float x, float y, float h, bool canUp, bool canDown)
        {
            // OpenMW's scroll arrows (textures/omw_menu_scroll_up/down)
            ui::image(ui::Bottom, "textures/omw_menu_scroll_up", x, y, 16, 16, canUp ? kWhite : ui::Rgba{ 120, 120, 120, 255 });
            ui::image(ui::Bottom, "textures/omw_menu_scroll_down", x, y + h - 16, 16, 16, canDown ? kWhite : ui::Rgba{ 120, 120, 120, 255 });
            zone(x - 8, y - 8, 32, 32, Action::Scroll, -1);
            zone(x - 8, y + h - 24, 32, 32, Action::Scroll, 1);
        }

        // The 1-9 row the "Quick key" button opens (QuickSlots3DS: nine 32x24 buttons), or the buttons.
        void quickPickRow(float x, float y)
        {
            for (int i = 0; i < kSlots; ++i)
                button(x + i * 33, y, 32, 24, std::to_string(i + 1), false, Action::AssignQuick, i);
        }

        void buildItemsPage(float px, float py)
        {
            // the filters (FilterAll3DS 2 2 52 22 ...), MW_Button
            const std::pair<const char*, int> filters[] = { { "All", MWGui::SortFilterItemModel::Category_All },
                { "Weapon", MWGui::SortFilterItemModel::Category_Weapon },
                { "Apparel", MWGui::SortFilterItemModel::Category_Apparel },
                { "Magic", MWGui::SortFilterItemModel::Category_Magic },
                { "Misc", MWGui::SortFilterItemModel::Category_Misc } };
            const float fx[5] = { 2, 56, 124, 196, 256 }, fw[5] = { 52, 66, 70, 58, 54 };
            for (int i = 0; i < 5; ++i)
                button(px + fx[i], py + 2, fw[i], 22, filters[i].first, sInvCategory == filters[i].second, Action::Filter, filters[i].second);
            // the grid (ItemView3DS 2 26 308 128): 42 px cells like OpenMW's item view, the arrows at the right
            MWGui::SortFilterItemModel& model = invModel();
            const int selected = invSelectedIndex(model);
            const int rows = (static_cast<int>(model.getItemCount()) + kInvColumns - 1) / kInvColumns;
            sInvScroll = std::clamp(sInvScroll, 0, std::max(0, rows - kInvRows));
            const size_t first = static_cast<size_t>(sInvScroll) * kInvColumns;
            for (size_t i = first; i < model.getItemCount() && i < first + kInvColumns * kInvRows; ++i)
            {
                const MWGui::ItemStack stack = model.getItem(i);
                const int cell = static_cast<int>(i - first);
                const float cx = px + 4 + (cell % kInvColumns) * 42, cy = py + 28 + (cell / kInvColumns) * 42;
                if (stack.mType == MWGui::ItemStack::Type_Equipped)
                    ui::rect(ui::Bottom, cx, cy, 40, 40, kEquipped);
                if (static_cast<int>(i) == selected)
                    ui::rect(ui::Bottom, cx, cy, 40, 40, kSelected);
                ui::icon(ui::Bottom, itemIconKey(stack.mBase), cx + 4, cy + 4, 32);
                if (stack.mCount > 1)
                {
                    const std::string count = std::to_string(stack.mCount);
                    ui::text(ui::Bottom, kFontValue, cx + 38 - ui::textWidth(kFontValue, count), cy + 28, count, kWhite);
                }
                zone(cx, cy, 42, 42, Action::SelectItem, static_cast<int>(i));
            }
            scrollArrows(px + 294, py + 28, 120, sInvScroll > 0, sInvScroll + kInvRows < rows);
            // the details (Details3DS 2 156 308 52): the item, then Use / Drop / Quick key (or the 1-9 row);
            // the encumbrance bar and the gold when nothing is selected
            const MWWorld::Ptr player = MWMechanics::getPlayer();
            if (selected >= 0)
            {
                const MWGui::ItemStack stack = model.getItem(selected);
                std::string name(sInvSelected.getClass().getName(sInvSelected));
                if (stack.mCount > 1)
                    name += " (" + std::to_string(stack.mCount) + ")";
                ui::text(ui::Bottom, kFontText, px + 8, py + 158, name, kGold);
                const std::string info = "Weight: " + MWGui::ToolTips::toString(sInvSelected.getClass().getWeight(sInvSelected))
                    + "   Value: " + MWGui::ToolTips::toString(sInvSelected.getClass().getValue(sInvSelected));
                ui::text(ui::Bottom, kFontText, px + 302 - ui::textWidth(kFontText, info), py + 158, info, kWhite);
                if (sQuickPick)
                    quickPickRow(px + 6, py + 180);
                else
                {
                    const bool equipped = stack.mType == MWGui::ItemStack::Type_Equipped;
                    button(px + 6, py + 180, 96, 24, equipped ? "Unequip" : "Use", false, Action::UseItem);
                    button(px + 108, py + 180, 96, 24, "Drop", false, Action::DropItem);
                    button(px + 210, py + 180, 96, 24, "Quick key", false, Action::QuickPick);
                }
            }
            else
            {
                const float encumbrance = player.getClass().getEncumbrance(player), capacity = player.getClass().getCapacity(player);
                framedBar(px + 6, py + 184, 200, 22, capacity > 0 ? encumbrance / capacity : 0.f, kMagicka,
                    std::to_string(static_cast<int>(encumbrance)) + "/" + std::to_string(static_cast<int>(capacity)));
                const std::string gold = std::to_string(player.getClass().getContainerStore(player).count(MWWorld::ContainerStore::sGoldId));
                ui::text(ui::Bottom, kFontText, px + 302 - ui::textWidth(kFontText, gold), py + 187, gold, kGold);
            }
        }

        void buildMagicPage(float px, float py)
        {
            const MWWorld::Ptr player = MWMechanics::getPlayer();
            const auto spells = playerSpells();
            const ESM::RefId& selected = windows().getSelectedSpell();
            // the active effects' icons (EffectsBox 2 2 304 20)
            float ex = px + 6;
            for (const std::string& key : sUiShown.mEffectIcons)
            {
                ui::icon(ui::Bottom, key, ex, py + 4, 16);
                ex += 18;
            }
            // Powers, then Spells with their cost/chance (abilities are not listed, as in OpenMW's spell window)
            std::vector<const ESM::Spell*> powers, normal;
            for (const ESM::Spell* spell : spells)
                if (spell->mData.mType == ESM::Spell::ST_Power)
                    powers.push_back(spell);
                else if (spell->mData.mType == ESM::Spell::ST_Spell)
                    normal.push_back(spell);
            // one flat list of rows: headers, names; scrolled by rows
            struct Row
            {
                const char* mHeader; // non-null: a group header
                const ESM::Spell* mSpell;
            };
            std::vector<Row> rowList;
            if (!powers.empty())
            {
                rowList.push_back({ "Powers", nullptr });
                for (const ESM::Spell* spell : powers)
                    rowList.push_back({ nullptr, spell });
            }
            rowList.push_back({ "Spells", nullptr });
            for (const ESM::Spell* spell : normal)
                rowList.push_back({ nullptr, spell });
            sMagicScroll = std::clamp(sMagicScroll, 0, std::max(0, static_cast<int>(rowList.size()) - kMagicRows));
            float y = py + 28;
            for (int r = sMagicScroll; r < static_cast<int>(rowList.size()) && r < sMagicScroll + kMagicRows; ++r)
            {
                const Row& row = rowList[static_cast<size_t>(r)];
                if (row.mHeader != nullptr)
                {
                    if (r > 0)
                        separator(px + 6, y - 2, 280);
                    ui::text(ui::Bottom, kFontText, px + 6, y, row.mHeader, kGold);
                    if (row.mHeader[0] == 'S')
                        ui::text(ui::Bottom, kFontText, px + 286 - ui::textWidth(kFontText, "Cost/Chance"), y, "Cost/Chance", kGold);
                }
                else
                {
                    const ESM::Spell* spell = row.mSpell;
                    const bool isSelected = spell->mId == selected;
                    if (isSelected)
                        ui::rect(ui::Bottom, px + 4, y - 1, 282, 18, kSelected);
                    ui::text(ui::Bottom, kFontText, px + 8, y, spell->mName, isSelected ? kActive : kGold);
                    if (spell->mData.mType == ESM::Spell::ST_Spell)
                    {
                        const int chance = static_cast<int>(MWMechanics::getSpellSuccessChance(spell, player));
                        const std::string right = std::to_string(spell->mData.mCost) + "/" + std::to_string(chance);
                        ui::text(ui::Bottom, kFontText, px + 286 - ui::textWidth(kFontText, right), y, right, kWhite);
                    }
                    zone(px + 4, y - 1, 282, 18, Action::SelectSpell, static_cast<int>(std::find(spells.begin(), spells.end(), spell) - spells.begin()));
                }
                y += 18;
            }
            scrollArrows(px + 294, py + 28, 150, sMagicScroll > 0, sMagicScroll + kMagicRows < static_cast<int>(rowList.size()));
            // the "Quick key" button (2 184 96 24), or the 1-9 row, for the selected spell
            if (!selected.empty())
            {
                if (sQuickPick)
                    quickPickRow(px + 6, py + 184);
                else
                    button(px + 6, py + 184, 96, 24, "Quick key", false, Action::QuickPick);
            }
        }

        void buildStatsPage(float px, float py)
        {
            const MWWorld::Ptr player = MWMechanics::getPlayer();
            const MWMechanics::NpcStats& stats = player.getClass().getNpcStats(player);
            const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
            const ESM::NPC* npc = player.get<ESM::NPC>()->mBase;
            const ESM::Race* race = store.get<ESM::Race>().search(npc->mRace);
            const ESM::Class* cls = store.get<ESM::Class>().search(npc->mClass);
            // the left pane (0 0 216 218): level, race, class with the values right-aligned, then the attributes
            const std::pair<std::string, std::string> lines[] = { { "Level", std::to_string(stats.getLevel()) },
                { "Race", race ? race->mName : std::string() }, { "Class", cls ? cls->mName : std::string() } };
            for (int i = 0; i < 3; ++i)
            {
                ui::text(ui::Bottom, kFontText, px + 6, py + 4 + i * 18, lines[i].first, kGold);
                ui::text(ui::Bottom, kFontText, px + 140 - ui::textWidth(kFontText, lines[i].second), py + 4 + i * 18, lines[i].second, kWhite);
            }
            for (int i = 0; i < ESM::Attribute::Length; ++i)
            {
                const ESM::RefId id = ESM::Attribute::indexToRefId(i);
                const ESM::Attribute* attribute = store.get<ESM::Attribute>().search(id);
                const float y = py + 64 + i * 18;
                ui::text(ui::Bottom, kFontText, px + 6, y, attribute ? attribute->mName : id.toDebugString(), kGold);
                const std::string value = std::to_string(static_cast<int>(stats.getAttribute(id).getModified()));
                ui::text(ui::Bottom, kFontText, px + 140 - ui::textWidth(kFontText, value), y, value, kWhite);
            }
            // the right pane (148 0 172 218): the three bars (label 64, bar 88x18), then the skills in groups
            const char* labels[3] = { "Health", "Magicka", "Fatigue" };
            const ui::Rgba colours[3] = { kHealth, kMagicka, kFatigue };
            for (int i = 0; i < 3; ++i)
            {
                const float y = py + 4 + i * 18;
                ui::text(ui::Bottom, kFontText, px + 158, y + 1, labels[i], kGold);
                framedBar(px + 222, y, 84, 18, float(sUiShown.mCurrent[i]) / float(sUiShown.mMax[i]), colours[i],
                    std::to_string(sUiShown.mCurrent[i]) + "/" + std::to_string(sUiShown.mMax[i]));
            }
            struct Row
            {
                const char* mHeader;
                ESM::RefId mSkill;
            };
            std::vector<Row> rows;
            std::vector<ESM::RefId> major, minor;
            if (cls != nullptr)
            {
                major.assign(cls->mData.mMajorSkills.begin(), cls->mData.mMajorSkills.end());
                minor.assign(cls->mData.mMinorSkills.begin(), cls->mData.mMinorSkills.end());
            }
            rows.push_back({ "Major Skills", ESM::RefId() });
            for (const ESM::RefId& id : major)
                rows.push_back({ nullptr, id });
            rows.push_back({ "Minor Skills", ESM::RefId() });
            for (const ESM::RefId& id : minor)
                rows.push_back({ nullptr, id });
            rows.push_back({ "Misc Skills", ESM::RefId() });
            for (int i = 0; i < ESM::Skill::Length; ++i)
            {
                const ESM::RefId id = ESM::Skill::indexToRefId(i);
                if (std::find(major.begin(), major.end(), id) == major.end() && std::find(minor.begin(), minor.end(), id) == minor.end())
                    rows.push_back({ nullptr, id });
            }
            sStatsScroll = std::clamp(sStatsScroll, 0, std::max(0, static_cast<int>(rows.size()) - kSkillRows));
            float y = py + 64;
            for (int r = sStatsScroll; r < static_cast<int>(rows.size()) && r < sStatsScroll + kSkillRows; ++r)
            {
                const Row& row = rows[static_cast<size_t>(r)];
                if (row.mHeader != nullptr)
                    ui::text(ui::Bottom, kFontText, px + 158, y, row.mHeader, kGold);
                else
                {
                    const ESM::Skill* skill = store.get<ESM::Skill>().search(row.mSkill);
                    ui::text(ui::Bottom, kFontText, px + 164, y, skill ? skill->mName : row.mSkill.toDebugString(), kGold);
                    const std::string value = std::to_string(static_cast<int>(stats.getSkill(row.mSkill).getModified()));
                    ui::text(ui::Bottom, kFontText, px + 286 - ui::textWidth(kFontText, value), y, value, kWhite);
                }
                y += 18;
            }
            scrollArrows(px + 294, py + 64, 140, sStatsScroll > 0, sStatsScroll + kSkillRows < static_cast<int>(rows.size()));
        }

        void buildDebugPage(float px, float py)
        {
            // DebugText3DS 6 4 296 82: the debug screen's rows; ScreenshotButton3DS 2 96 120 24
            float y = py + 4;
            size_t start = 0;
            while (start < sDebugText.size() && y < py + 96)
            {
                size_t end = sDebugText.find('\n', start);
                if (end == std::string::npos)
                    end = sDebugText.size();
                ui::text(ui::Bottom, kFontSmall, px + 6, y, std::string_view(sDebugText).substr(start, end - start), kWhite);
                y += 11;
                start = end + 1;
            }
            ui::frame(ui::Bottom, px + 2, py + 96, 120, 24, "thin");
            ui::text(ui::Bottom, kFontValue, px + 62 - ui::textWidth(kFontValue, "Screenshot") / 2, py + 102, "Screenshot", kGold);
            zone(px + 2, py + 96, 120, 24, Action::Screenshot);
        }

        // A button in Morrowind's style (MW_Button: the menu_rightbuttonup pieces, 2 px corners and edges, the
        // centre stretched) with a centred 16 px caption, lighter when selected.
        void buttonFrame(float x, float y, float w, float h)
        {
            // (the skin's centre piece is left out: MyGUI's buttons show the black panel through it)
            const char* p = "textures/menu_rightbuttonup_";
            ui::image(ui::Bottom, std::string(p) + "top", x + 2, y, w - 4, 2, kWhite);
            ui::image(ui::Bottom, std::string(p) + "bottom", x + 2, y + h - 2, w - 4, 2, kWhite);
            ui::image(ui::Bottom, std::string(p) + "left", x, y + 2, 2, h - 4, kWhite);
            ui::image(ui::Bottom, std::string(p) + "right", x + w - 2, y + 2, 2, h - 4, kWhite);
            ui::image(ui::Bottom, std::string(p) + "top_left", x, y, 2, 2, kWhite);
            ui::image(ui::Bottom, std::string(p) + "top_right", x + w - 2, y, 2, 2, kWhite);
            ui::image(ui::Bottom, std::string(p) + "bottom_left", x, y + h - 2, 2, 2, kWhite);
            ui::image(ui::Bottom, std::string(p) + "bottom_right", x + w - 2, y + h - 2, 2, 2, kWhite);
        }

        void button(float x, float y, float w, float h, std::string_view caption, bool selected, Action action, int arg)
        {
            buttonFrame(x, y, w, h);
            const float tw = ui::textWidth(kFontText, caption);
            ui::text(ui::Bottom, kFontText, x + (w - tw) / 2, y + (h - 16) / 2, caption, selected ? kActive : kGold);
            zone(x, y, w, h, action, arg);
        }

        // A framed bar (MW_Progress: a gold border, the colour inside) with its value centred.
        void framedBar(float x, float y, float w, float h, float fill, ui::Rgba colour, std::string_view value)
        {
            ui::rect(ui::Bottom, x, y, w, h, kBarFrame);
            ui::rect(ui::Bottom, x + 1, y + 1, w - 2, h - 2, kBarBack);
            ui::rect(ui::Bottom, x + 1, y + 1, (w - 2) * std::clamp(fill, 0.f, 1.f), h - 2, colour);
            if (!value.empty())
                ui::text(ui::Bottom, kFontText, x + (w - ui::textWidth(kFontText, value)) / 2, y + (h - 16) / 2, value, kWhite);
        }

        // A plain bar (the energy bars under the weapon and spell boxes).
        void bar(float x, float y, float w, float h, float fill, ui::Rgba colour)
        {
            ui::rect(ui::Bottom, x, y, w, h, kBarBack);
            ui::rect(ui::Bottom, x, y, w * std::clamp(fill, 0.f, 1.f), h, colour);
        }

        void separator(float x, float y, float w)
        {
            ui::rect(ui::Bottom, x, y, w, 1, kGold);
        }

        void buildBottom(const UiState& s)
        {
            sZones.clear();
            ui::releaseIcons(); // the icon slots are rebuilt with the screen
            ui::begin(ui::Bottom);
            if (!s.mActive)
                return;
            // the tab bar: openmw_3ds_tabs.layout (Home 2 1 24 21, Inventory 28 1 80 21, Magic 110, Stats 180, Debug 250)
            const float bx = kBottomX, by = kBottomY;
            ui::rect(ui::Bottom, bx, by, 320, 22, ui::Rgba{ 0, 0, 0, 255 });
            buttonFrame(bx + 2, by + 1, 24, 21);
            ui::image(ui::Bottom, "textures/omw_3ds_home", bx + 6, by + 3, 16, 16, s.mTab == Tab::None ? kActive : kGold);
            zone(bx + 2, by + 1, 24, 21, Action::Home);
            button(bx + 28, by + 1, 80, 21, "Inventory", s.mTab == Tab::Items, Action::TabItems);
            button(bx + 110, by + 1, 68, 21, "Magic", s.mTab == Tab::Magic, Action::TabMagic);
            button(bx + 180, by + 1, 68, 21, "Stats", s.mTab == Tab::Stats, Action::TabStats);
#ifdef OPENMW3DS_DEBUG
            button(bx + 250, by + 1, 68, 21, "Debug", s.mTab == Tab::Debug, Action::TabDebug);
#endif
            const float px = kBottomX, py = kPanelY; // the panel: openmw_hud.layout's Bottom3DS (MW_DialogNoTransp)
            ui::frame(ui::Bottom, px, py, kPageWidth, kPageHeight, "thick");
            if (s.mTab != Tab::None)
            {
                switch (s.mTab)
                {
                    case Tab::Items:
                        buildItemsPage(px, py);
                        break;
                    case Tab::Magic:
                        buildMagicPage(px, py);
                        break;
                    case Tab::Stats:
                        buildStatsPage(px, py);
                        break;
                    case Tab::Debug:
                        buildDebugPage(px, py);
                        break;
                    default:
                        break;
                }
                return;
            }
            // the bars with their labels and values (HealthFrame 4 4 230 18: the label, the bar at +66 164 wide)
            const char* labels[3] = { "Health", "Magicka", "Fatigue" };
            const ui::Rgba colours[3] = { kHealth, kMagicka, kFatigue };
            for (int i = 0; i < 3; ++i)
            {
                const float y = py + 4 + i * 18;
                ui::text(ui::Bottom, kFontText, px + 6, y + 1, labels[i], kGold);
                framedBar(px + 70, y, 164, 18, float(s.mCurrent[i]) / float(s.mMax[i]), colours[i],
                    std::to_string(s.mCurrent[i]) + "/" + std::to_string(s.mMax[i]));
                zone(px + 4, y, 230, 18, Action::OpenStats);
            }
            // the minimap box (240 4 68 68): black until the map comes; the compass at its bottom-left corner
            ui::rect(ui::Bottom, px + 240, py + 4, 68, 68, ui::Rgba{ 0, 0, 0, 255 });
            ui::frame(ui::Bottom, px + 240, py + 4, 68, 68, "thin");
            ui::imageRotated(ui::Bottom, "textures/compass", px + 242 + 16, py + 70 - 16, 32, 32, -s.mYaw, kWhite);
            // the weapon (WeapBox 4 78 36 42: the icon, its bar under it inside the box) and the spell (158 78)
            if (s.mWeaponShown)
            {
                ui::frame(ui::Bottom, px + 4, py + 78, 36, 42, "thin");
                ui::icon(ui::Bottom, s.mWeaponIcon, px + 6, py + 80, 32);
                if (s.mWeaponHealth >= 0)
                    bar(px + 6, py + 114, 32, 4, s.mWeaponHealth / 100.f, kWeaponBar);
                ui::text(ui::Bottom, kFontText, px + 44, py + 86, s.mWeaponName, kGold);
                zone(px + 4, py + 78, 150, 42, Action::OpenItems);
            }
            if (s.mSpellShown)
            {
                ui::frame(ui::Bottom, px + 158, py + 78, 36, 42, "thin");
                ui::icon(ui::Bottom, s.mSpellIcon, px + 160, py + 80, 32);
                if (s.mSpellChance >= 0)
                    bar(px + 160, py + 114, 32, 4, s.mSpellChance / 100.f, kMagicBar);
                ui::text(ui::Bottom, kFontText, px + 198, py + 86, s.mSpellName, kGold);
                zone(px + 158, py + 78, 150, 42, Action::OpenMagic);
            }
            // sneaking (SneakBox 4 128 36 36) and the active effects (EffectBox 288 128, growing left)
            if (s.mSneaking)
                ui::icon(ui::Bottom, "icons/k/stealth_sneak", px + 6, py + 130, 32);
            float ex = px + 288;
            for (const std::string& key : s.mEffectIcons)
            {
                ui::icon(ui::Bottom, key, ex, py + 130, 16);
                ex -= 18;
            }
            // the quick keys (QuickKeys3DS 3 174: nine 34x34 boxes, HUD_Box)
            for (int i = 0; i < kSlots; ++i)
            {
                const float x = px + 3 + i * 34, y = py + 174;
                ui::frame(ui::Bottom, x, y, 34, 34, "thin");
                if (!s.mQuickIcons[i].empty())
                    ui::icon(ui::Bottom, s.mQuickIcons[i], x + 1, y + 1, 32);
                zone(x, y, 34, 34, Action::QuickKey1, i);
            }
        }

        void buildTop(const std::string& label)
        {
            ui::begin(ui::Top);
            ui::image(ui::Top, "textures/target", 187, 107, 27, 27, kWhite); // HUD_Crosshair's place
            if (!label.empty())
            {
                const float w = ui::textWidth(kFontSmall, label);
                const float x = (Render3DS::kGuiCanvasWidth - w) / 2;
                ui::rect(ui::Top, x - 4, kLabelTop, w + 8, 14, kLabelBack);
                ui::text(ui::Top, kFontSmall, x, kLabelTop + 2, label, kGold);
            }
        }

        void act(const Zone& z)
        {
            if (z.mAction != Action::AssignQuick && z.mAction != Action::QuickPick)
                sQuickPick = false;
            switch (z.mAction)
            {
                case Action::Home:
                    sTab = Tab::None;
                    break;
                case Action::TabItems:
                    sTab = sTab == Tab::Items ? Tab::None : Tab::Items;
                    break;
                case Action::TabMagic:
                    sTab = sTab == Tab::Magic ? Tab::None : Tab::Magic;
                    break;
                case Action::TabStats:
                    sTab = sTab == Tab::Stats ? Tab::None : Tab::Stats;
                    break;
                case Action::TabDebug:
                    sTab = sTab == Tab::Debug ? Tab::None : Tab::Debug;
                    break;
                case Action::OpenStats:
                    sTab = Tab::Stats;
                    break;
                case Action::OpenItems:
                    sTab = Tab::Items;
                    break;
                case Action::OpenMagic:
                    sTab = Tab::Magic;
                    break;
                case Action::QuickKey1:
                    windows().activateQuickKey(z.mArg + 1);
                    break;
                case Action::Filter:
                    sInvCategory = z.mArg;
                    sInvScroll = 0;
                    break;
                case Action::Scroll:
                    if (sTab == Tab::Items)
                        sInvScroll = std::max(0, sInvScroll + z.mArg);
                    else if (sTab == Tab::Magic)
                        sMagicScroll = std::max(0, sMagicScroll + z.mArg);
                    else if (sTab == Tab::Stats)
                        sStatsScroll = std::max(0, sStatsScroll + z.mArg);
                    break;
                case Action::SelectItem:
                {
                    MWGui::SortFilterItemModel& model = invModel();
                    if (z.mArg >= 0 && static_cast<size_t>(z.mArg) < model.getItemCount())
                    {
                        const MWWorld::Ptr item = model.getItem(z.mArg).mBase;
                        sInvSelected = item == sInvSelected ? MWWorld::Ptr() : item;
                    }
                    break;
                }
                case Action::UseItem:
                {
                    if (sInvSelected.isEmpty())
                        break;
                    MWGui::SortFilterItemModel& model = invModel();
                    const int index = invSelectedIndex(model);
                    const MWWorld::Ptr player = MWMechanics::getPlayer();
                    if (index >= 0 && model.getItem(index).mType == MWGui::ItemStack::Type_Equipped)
                        player.getClass().getInventoryStore(player).unequipItem(sInvSelected);
                    else if (MWGui::InventoryWindow* inventory = Hud3DS::inventory())
                        inventory->useItem(sInvSelected);
                    break;
                }
                case Action::DropItem:
                {
                    if (sInvSelected.isEmpty())
                        break;
                    MWGui::SortFilterItemModel& model = invModel();
                    const int index = invSelectedIndex(model);
                    if (index < 0)
                        break;
                    const int count = static_cast<int>(model.getItem(index).mCount);
                    const MWWorld::Ptr player = MWMechanics::getPlayer();
                    MWBase::World* world = MWBase::Environment::get().getWorld();
                    world->breakInvisibility(player);
                    world->dropObjectOnGround(player, sInvSelected, count);
                    windows().playSound(sInvSelected.getClass().getDownSoundId(sInvSelected));
                    player.getClass().getContainerStore(player).remove(sInvSelected, count);
                    sInvSelected = MWWorld::Ptr();
                    break;
                }
                case Action::AssignQuick:
                    sQuickPick = false;
                    if (sTab == Tab::Items && !sInvSelected.isEmpty())
                        Hud3DS::assignItem(z.mArg, sInvSelected);
                    else if (sTab == Tab::Magic)
                        Hud3DS::assignSelectedMagic(z.mArg);
                    break;
                case Action::SelectSpell:
                {
                    const auto& spells = playerSpells();
                    if (z.mArg >= 0 && static_cast<size_t>(z.mArg) < spells.size())
                    {
                        const ESM::Spell* spell = spells[z.mArg];
                        const MWWorld::Ptr player = MWMechanics::getPlayer();
                        windows().setSelectedSpell(spell->mId, static_cast<int>(MWMechanics::getSpellSuccessChance(spell, player)));
                    }
                    break;
                }
                case Action::QuickPick:
                    sQuickPick = true;
                    break;
                case Action::Screenshot:
                    requestScreenshot3DS();
                    break;
                default:
                    break;
            }
        }

        void updateLumiere()
        {
            ++sUiFrame;
            UiState s = readUiState();
            if (!sUiBuilt || !(s == sUiShown))
            {
                buildBottom(s);
                sUiShown = std::move(s);
                sUiBuilt = true;
            }
            // the target label: tooltips.cpp sets it on the frames the crosshair has a target
            const std::string label = sUiFrame - sTopLabelFrame <= 2 ? sTopLabel : std::string();
            if (!sTopBuilt || label != sTopShown)
            {
                buildTop(label);
                sTopShown = label;
                sTopBuilt = true;
            }
        }
    }

    void touchLumiere(int px, int py, bool down)
    {
        static bool sDown = false;
        static int sX = 0, sY = 0;
        if (down)
        {
            sX = px + kBottomX;
            sY = py + kBottomY;
        }
        else if (sDown) // released: the zone under the last touch acts
        {
            for (const Zone& z : sZones)
                if (sX >= z.mX && sX < z.mX + z.mW && sY >= z.mY && sY < z.mY + z.mH)
                {
                    act(z);
                    break;
                }
        }
        sDown = down;
    }
#endif // OPENMW3DS_LUMIERE

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
#ifdef OPENMW3DS_LUMIERE
        sTopLabel = label;
        sTopLabelFrame = sUiFrame;
#endif // OPENMW3DS_LUMIERE
        return label;
    }

    void updateHud3DS()
    {
        if (!Hud3DS::ready())
            return;
#ifdef OPENMW3DS_LUMIERE
        if (Render3DS::Lumiere::active())
        {
            updateLumiere();
            return;
        }
#endif // OPENMW3DS_LUMIERE
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
