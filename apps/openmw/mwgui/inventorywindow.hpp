#ifndef MGUI_Inventory_H
#define MGUI_Inventory_H

#include "mode.hpp"
#include "windowpinnablebase.hpp"

#include "../mwworld/ptr.hpp"

#include <components/misc/notnullptr.hpp>

#include "../n3ds/menufriend.hpp"
#include <osg/Vec2i>

namespace osg
{
    class Group;
}

#include <memory>

namespace MWRender
{
    class InventoryPreview;
}

namespace MWGui
{
    namespace Widgets
    {
        class MWDynamicStat;
    }

    class ItemView;
    class SortFilterItemModel;
    class TradeItemModel;
    class DragAndDrop;
    class ItemModel;
    class ItemTransfer;

    class InventoryWindow : public WindowPinnableBase
    {
        OMW3DS_MENU_FRIEND
        OMW3DS_HUD_FRIEND
    public:
        explicit InventoryWindow(DragAndDrop& dragAndDrop, ItemTransfer& itemTransfer);
        ~InventoryWindow() override;

        void onOpen() override;

        void onClose() override;

        /// start trading, disables item drag&drop
        void setTrading(bool trading);

        void onFrame(float dt) override;

        void pickUpObject(MWWorld::Ptr object);

        MWWorld::Ptr getAvatarSelectedItem(int x, int y);

        void rebuildAvatar();

        SortFilterItemModel* getSortFilterModel();
        TradeItemModel* getTradeModel();
        ItemModel* getModel();

        void updateItemView();

        void updatePlayer();

        void clear() override;

        void useItem(const MWWorld::Ptr& ptr, bool force = false);

        void setGuiMode(GuiMode mode);

        void onInventoryUpdate(const MWWorld::Ptr& ptr) override;

        /// Cycle to previous/next weapon
        void cycle(bool next);

        std::string_view getWindowIdForLua() const override { return "Inventory"; }

        ControllerButtons* getControllerButtons() override;

    protected:
        void onTitleDoubleClicked() override;
        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;
        void setActiveControllerWindow(bool active) override;

    private:
        Misc::NotNullPtr<DragAndDrop> mDragAndDrop;
        Misc::NotNullPtr<ItemTransfer> mItemTransfer;

        int mSelectedItem;

        MWWorld::Ptr mPtr;

        MWGui::ItemView* mItemView;
        SortFilterItemModel* mSortModel;
        TradeItemModel* mTradeModel;

        MyGUI::Widget* mAvatar;
        MyGUI::ImageBox* mAvatarImage;
        MyGUI::TextBox* mArmorRating;
        Widgets::MWDynamicStat* mEncumbranceBar;

        MyGUI::Widget* mLeftPane;
        MyGUI::Widget* mRightPane;

        MyGUI::Button* mFilterAll;
        MyGUI::Button* mFilterWeapon;
        MyGUI::Button* mFilterApparel;
        MyGUI::Button* mFilterMagic;
        MyGUI::Button* mFilterMisc;

        MyGUI::EditBox* mFilterEdit;

        GuiMode mGuiMode;

        int mLastXSize;
        int mLastYSize;

        std::unique_ptr<MyGUI::ITexture> mPreviewTexture;
        std::unique_ptr<MWRender::InventoryPreview> mPreview;

        bool mTrading;
        bool mUpdateNextFrame;
#ifdef OPENMW_PLATFORM_3DS
        // The paper doll (the player's model in its own render-to-texture scene, ~6 MB and ~1.3 s to build) is
        // built when this window opens: the 3DS shows the inventory as the bottom screen's Items tab, without it.
        bool mPreviewStale3DS = true;
        void rebuildPreview3DS();
#endif

        void toggleMaximized();

        void onItemSelected(int index);
        void onItemSelectedFromSourceModel(int index);

        void onBackgroundSelected();

        enum class ControllerAction
        {
            None,
            Use,
            Transfer,
            Sell,
            Drop,
        };
        ControllerAction mPendingControllerAction;

        void sellItem(MyGUI::Widget* sender, std::size_t count);
        void dragItem(MyGUI::Widget* sender, std::size_t count);
        void transferItem(MyGUI::Widget* sender, std::size_t count);
        void dropItem(MyGUI::Widget* sender, std::size_t count);
        void equipItem(std::size_t count);

        void onWindowResize(MyGUI::Window* sender);
        void onFilterChanged(MyGUI::Widget* sender);
        void onNameFilterChanged(MyGUI::EditBox* sender);
        void onAvatarClicked(MyGUI::Widget* sender);
        void onPinToggled() override;

        void updateEncumbranceBar();
        void notifyContentChanged();
        void dirtyPreview();
        void updatePreviewSize();
        void updateArmorRating();

        MyGUI::IntSize getPreviewViewportSize() const;
        osg::Vec2i mapPreviewWindowToViewport(int x, int y) const;

        void adjustPanes();

        /// Unequips count items from mSelectedItem, if it is equipped, and then updates mSelectedItem in case the items
        /// were re-stacked
        void ensureSelectedItemUnequipped(int count);
    };
}

#endif // Inventory_H
