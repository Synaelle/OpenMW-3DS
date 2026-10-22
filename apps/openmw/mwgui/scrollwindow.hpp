#ifndef MWGUI_SCROLLWINDOW_H
#define MWGUI_SCROLLWINDOW_H

#include "windowbase.hpp"

#include "../mwworld/ptr.hpp"

#include "../n3ds/menufriend.hpp"

namespace Gui
{
    class ImageButton;
}

namespace MWGui
{
    class ScrollWindow : public BookWindowBase
    {
        OMW3DS_MENU_FRIEND
    public:
        ScrollWindow();

        void setPtr(const MWWorld::Ptr& scroll) override;
        void setInventoryAllowed(bool allowed);

        void onClose() override;
        void onResChange(int, int) override { center(); }

        std::string_view getWindowIdForLua() const override { return "Scroll"; }

        ControllerButtons* getControllerButtons() override;

    protected:
        void onCloseButtonClicked(MyGUI::Widget* sender);
        void onTakeButtonClicked(MyGUI::Widget* sender);
        void setTakeButtonShow(bool show);
        void onKeyButtonPressed(MyGUI::Widget* sender, MyGUI::KeyCode key, MyGUI::Char character);
        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;

    private:
#ifdef OPENMW_PLATFORM_3DS
        // M19: the bottom screen's title and text buttons (packaging/ui3ds/mygui/openmw_scroll.layout); the image
        // buttons stay hidden.
        MyGUI::TextBox* mTitle3DS = nullptr;
        MyGUI::Button* mTake3DS = nullptr;
        void onScrollButton3DS(MyGUI::Widget* sender);
#endif
        Gui::ImageButton* mCloseButton;
        Gui::ImageButton* mTakeButton;
        MyGUI::ScrollView* mTextView;

        MWWorld::Ptr mScroll;

        bool mTakeButtonShow;
        bool mTakeButtonAllowed;
    };

}

#endif
