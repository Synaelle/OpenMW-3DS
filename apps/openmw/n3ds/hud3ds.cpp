// Nintendo 3DS HUD and bottom-screen tabs (see hud3ds.hpp).

#include "hud3ds.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <memory>
#include <string>

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_Gui.h>
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

        Tab sTab = Tab::None;
        std::unique_ptr<MWGui::Layout> sTabBar;
        std::array<MyGUI::Button*, 3> sTabButtons{};
        std::unique_ptr<ItemsPage> sItems;
        QuickKeyPicker sMagicPicker;
        const MWGui::SpellWindow* sMagicBoundTo = nullptr;
        Tab sShown = Tab::None;

        void onTabClicked(MyGUI::Widget* sender)
        {
            const Tab tab = *sender->getUserData<Tab>();
            sTab = sTab == tab ? Tab::None : tab;
        }

        bool bindTabs()
        {
            if (sTabBar != nullptr)
                return true;
            if (!has3DSLayouts())
                return false;
            sTabBar = std::make_unique<MWGui::Layout>("openmw_3ds_tabs.layout");
            const std::pair<const char*, Tab> tabs[] = {
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

    bool toggleInventoryTab3DS()
    {
        if (sTabBar == nullptr)
            return false;
        sTab = sTab == Tab::Items ? Tab::None : Tab::Items;
        return true;
    }
}
