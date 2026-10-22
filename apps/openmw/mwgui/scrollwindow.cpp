#include "scrollwindow.hpp"

#include <MyGUI_Button.h>
#include <MyGUI_ScrollView.h>
#include <MyGUI_TextBox.h>

#include <components/esm3/loadbook.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/widgets/imagebutton.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/windowmanager.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "../mwworld/actiontake.hpp"
#include "../mwworld/class.hpp"

#include "formatting.hpp"

namespace MWGui
{

    ScrollWindow::ScrollWindow()
        : BookWindowBase("openmw_scroll.layout")
        , mTakeButtonShow(true)
        , mTakeButtonAllowed(true)
    {
        getWidget(mTextView, "TextView");

        getWidget(mCloseButton, "CloseButton");
        mCloseButton->eventMouseButtonClick += MyGUI::newDelegate(this, &ScrollWindow::onCloseButtonClicked);

        getWidget(mTakeButton, "TakeButton");
        mTakeButton->eventMouseButtonClick += MyGUI::newDelegate(this, &ScrollWindow::onTakeButtonClicked);

        adjustButton("CloseButton");
        adjustButton("TakeButton");

        mCloseButton->eventKeyButtonPressed += MyGUI::newDelegate(this, &ScrollWindow::onKeyButtonPressed);
        mTakeButton->eventKeyButtonPressed += MyGUI::newDelegate(this, &ScrollWindow::onKeyButtonPressed);

        mControllerScrollWidget = mTextView;
        mControllerButtons.mB = "#{Interface:Close}";
        mControllerButtons.mDpad = "#{Interface:ScrollDown}";

#ifdef OPENMW_PLATFORM_3DS
        MyGUI::Button* button = nullptr;
        getWidget(mTitle3DS, "Title3DS");
        getWidget(mTake3DS, "Take3DS");
        mTake3DS->eventMouseButtonClick += MyGUI::newDelegate(this, &ScrollWindow::onTakeButtonClicked);
        getWidget(button, "Close3DS");
        button->eventMouseButtonClick += MyGUI::newDelegate(this, &ScrollWindow::onCloseButtonClicked);
        getWidget(button, "Up3DS");
        button->eventMouseButtonClick += MyGUI::newDelegate(this, &ScrollWindow::onScrollButton3DS);
        getWidget(button, "Down3DS");
        button->eventMouseButtonClick += MyGUI::newDelegate(this, &ScrollWindow::onScrollButton3DS);
#endif

        center();
    }

    void ScrollWindow::setPtr(const MWWorld::Ptr& scroll)
    {
        if (scroll.isEmpty() || (scroll.getType() != ESM::REC_BOOK && scroll.getType() != ESM::REC_BOOK4))
            throw std::runtime_error("Invalid argument in ScrollWindow::setPtr");
        mScroll = scroll;

        MWWorld::Ptr player = MWMechanics::getPlayer();
        bool showTakeButton = scroll.getContainerStore() != &player.getClass().getContainerStore(player);

        const std::string* text;
        if (scroll.getType() == ESM::REC_BOOK)
            text = &static_cast<const std::string&>(scroll.get<ESM::Book>()->mBase->mText); // (ESM::LazyString on 3DS)
        else
            text = &scroll.get<ESM4::Book>()->mBase->mText;
        bool shrinkTextAtLastTag = scroll.getType() == ESM::REC_BOOK;

        Formatting::BookFormatter formatter;
#ifdef OPENMW_PLATFORM_3DS
        // The 3DS layout's text area, less the scroll bar (the stock layout's 390 of 410).
        formatter.markupToWidget(
            mTextView, *text, mTextView->getWidth() - 20, mTextView->getHeight(), shrinkTextAtLastTag);
        mTitle3DS->setCaption(std::string(scroll.getClass().getName(scroll)));
#else
        formatter.markupToWidget(mTextView, *text, 390, mTextView->getHeight(), shrinkTextAtLastTag);
#endif
        MyGUI::IntSize size = mTextView->getChildAt(0)->getSize();

        // Canvas size must be expressed with VScroll disabled, otherwise MyGUI would expand the scroll area when the
        // scrollbar is hidden
        mTextView->setVisibleVScroll(false);
        if (size.height > mTextView->getSize().height)
            mTextView->setCanvasSize(mTextView->getWidth(), size.height);
        else
            mTextView->setCanvasSize(mTextView->getWidth(), mTextView->getSize().height);
        mTextView->setVisibleVScroll(true);

        mTextView->setViewOffset(MyGUI::IntPoint(0, 0));

        setTakeButtonShow(showTakeButton);

        MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mCloseButton);
    }

    void ScrollWindow::onKeyButtonPressed(MyGUI::Widget* /*sender*/, MyGUI::KeyCode key, MyGUI::Char character)
    {
        int scroll = 0;
        if (key == MyGUI::KeyCode::ArrowUp)
            scroll = 40;
        else if (key == MyGUI::KeyCode::ArrowDown)
            scroll = -40;

        if (scroll != 0)
            mTextView->setViewOffset(mTextView->getViewOffset() + MyGUI::IntPoint(0, scroll));
    }

    void ScrollWindow::setTakeButtonShow(bool show)
    {
        mTakeButtonShow = show;
        mTakeButton->setVisible(mTakeButtonShow && mTakeButtonAllowed);
#ifdef OPENMW_PLATFORM_3DS
        mTake3DS->setVisible(mTakeButtonShow && mTakeButtonAllowed);
        mTakeButton->setVisible(false);
#endif
    }

#ifdef OPENMW_PLATFORM_3DS
    void ScrollWindow::onScrollButton3DS(MyGUI::Widget* sender)
    {
        // As the D-pad does (onKeyButtonPressed): 40 px a press.
        onKeyButtonPressed(sender,
            sender->getName().find("Up3DS") != std::string::npos ? MyGUI::KeyCode::ArrowUp : MyGUI::KeyCode::ArrowDown,
            0);
    }
#endif

    void ScrollWindow::setInventoryAllowed(bool allowed)
    {
        mTakeButtonAllowed = allowed;
        mTakeButton->setVisible(mTakeButtonShow && mTakeButtonAllowed);
#ifdef OPENMW_PLATFORM_3DS
        mTake3DS->setVisible(mTakeButtonShow && mTakeButtonAllowed);
        mTakeButton->setVisible(false);
#endif
    }

    void ScrollWindow::onCloseButtonClicked(MyGUI::Widget* /*sender*/)
    {
        MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_Scroll);
    }

    void ScrollWindow::onTakeButtonClicked(MyGUI::Widget* /*sender*/)
    {
        MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("Item Book Up"));

        MWWorld::ActionTake take(mScroll);
        take.execute(MWMechanics::getPlayer());

        MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_Scroll);
    }

    void ScrollWindow::onClose()
    {
        if (Settings::gui().mControllerMenus)
            MWBase::Environment::get().getInputManager()->setGamepadGuiCursorEnabled(true);
        BookWindowBase::onClose();
    }

    ControllerButtons* ScrollWindow::getControllerButtons()
    {
#ifdef OPENMW_PLATFORM_3DS
        if (mTake3DS->getVisible()) // the image button stays hidden there
#else
        if (mTakeButton->getVisible())
#endif
            mControllerButtons.mA = "#{Interface:Take}";
        else
            mControllerButtons.mA.clear();
        return &mControllerButtons;
    }

    bool ScrollWindow::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
#ifdef OPENMW_PLATFORM_3DS
            if (mTake3DS->getVisible())
#else
            if (mTakeButton->getVisible())
#endif
                onTakeButtonClicked(mTakeButton);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_B)
            onCloseButtonClicked(mCloseButton);
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP || arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
            return false; // Fall through to keyboard

        return true;
    }
}
