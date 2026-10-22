#ifndef MWGUI_BOOKWINDOW_H
#define MWGUI_BOOKWINDOW_H

#include "windowbase.hpp"

#include "../mwworld/ptr.hpp"

#include <components/widgets/imagebutton.hpp>

#include "../n3ds/menufriend.hpp"

namespace MWGui
{
    class BookWindow : public BookWindowBase
    {
        OMW3DS_MENU_FRIEND
    public:
        BookWindow();

        void setPtr(const MWWorld::Ptr& book) override;
        void setInventoryAllowed(bool allowed);

        void onResChange(int, int) override { center(); }
        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;

        std::string_view getWindowIdForLua() const override { return "Book"; }
        ControllerButtons* getControllerButtons() override;

    protected:
        void onNextPageButtonClicked(MyGUI::Widget* sender);
        void onPrevPageButtonClicked(MyGUI::Widget* sender);
        void onCloseButtonClicked(MyGUI::Widget* sender);
        void onTakeButtonClicked(MyGUI::Widget* sender);
        void onMouseWheel(MyGUI::Widget* sender, int rel);
        void setTakeButtonShow(bool show);

        void onKeyButtonPressed(MyGUI::Widget* sender, MyGUI::KeyCode key, MyGUI::Char character);

        void nextPage();
        void prevPage();

        void updatePages();
        void clearPages();

    private:
        typedef std::pair<int, int> Page;
        typedef std::vector<Page> Pages;

        Gui::ImageButton* mCloseButton;
        Gui::ImageButton* mTakeButton;
        Gui::ImageButton* mNextPageButton;
        Gui::ImageButton* mPrevPageButton;

        MyGUI::TextBox* mLeftPageNumber;
        MyGUI::TextBox* mRightPageNumber;
        MyGUI::Widget* mLeftPage;
        MyGUI::Widget* mRightPage;

        unsigned int mCurrentPage; // 0 is first page
#ifdef OPENMW_PLATFORM_3DS
        // M19: one page at a time on the top screen. The bottom screen's title, page number and text buttons
        // (packaging/ui3ds/mygui/openmw_book.layout). The image buttons stay hidden, these show instead.
        MyGUI::TextBox* mTitle3DS = nullptr;
        MyGUI::Button* mPrev3DS = nullptr;
        MyGUI::Button* mNext3DS = nullptr;
        MyGUI::Button* mTake3DS = nullptr;
        MyGUI::Button* mClose3DS = nullptr;
#endif
        Pages mPages;

        MWWorld::Ptr mBook;

        bool mTakeButtonShow;
        bool mTakeButtonAllowed;
    };

}

#endif
