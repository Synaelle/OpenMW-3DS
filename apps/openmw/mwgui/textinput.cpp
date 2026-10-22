#include "textinput.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_TextBox.h>
#include <MyGUI_UString.h>

#include <components/esm/refid.hpp>

#ifdef OPENMW_PLATFORM_3DS
#include "../n3ds/frame3ds.hpp"
#endif

namespace MWGui
{

    TextInputDialog::TextInputDialog()
        : WindowModal("openmw_text_input.layout")
    {
        // Centre dialog
        center();
#ifdef OPENMW_PLATFORM_3DS
        // On the top screen: the system keyboard covers the bottom one while the text is typed.
        const MyGUI::IntCoord coord = mMainWidget->getCoord();
        mMainWidget->setPosition((400 - coord.width) / 2, (240 - coord.height) / 2);
#endif

        getWidget(mTextEdit, "TextEdit");
        mTextEdit->eventEditSelectAccept += newDelegate(this, &TextInputDialog::onTextAccepted);

        MyGUI::Button* okButton;
        getWidget(okButton, "OKButton");
        okButton->eventMouseButtonClick += MyGUI::newDelegate(this, &TextInputDialog::onOkClicked);

        // Make sure the edit box has focus
        MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mTextEdit);

        mControllerButtons.mA = "#{Interface:OK}";
    }

    void TextInputDialog::setNextButtonShow(bool shown)
    {
        MyGUI::Button* okButton;
        getWidget(okButton, "OKButton");

        if (shown)
            okButton->setCaption(
                MyGUI::UString(MWBase::Environment::get().getWindowManager()->getGameSettingString("sNext", {})));
        else
            okButton->setCaption(
                MyGUI::UString(MWBase::Environment::get().getWindowManager()->getGameSettingString("sOK", {})));
    }

    void TextInputDialog::setTextLabel(std::string_view label)
    {
        setText("LabelT", label);
    }

    void TextInputDialog::onOpen()
    {
        WindowModal::onOpen();
        // Make sure the edit box has focus
        MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mTextEdit);
#ifdef OPENMW_PLATFORM_3DS
        // The system keyboard types the text (M8): opened at the next frame's start, outside any drawing, and
        // the dialog is accepted as if OK were pressed. Cancelled, the dialog stays for a second try by touch.
        omw3ds::deferToFrame([this] {
            std::string text = getTextInput();
            MyGUI::TextBox* label = nullptr;
            getWidget(label, "LabelT");
            if (omw3ds::systemKeyboard(label != nullptr ? label->getCaption().asUTF8() : std::string(), text, 32))
            {
                mTextEdit->setCaption(MyGUI::UString(text));
                onOkClicked(nullptr);
            }
        });
#endif
    }

    // widget controls

    void TextInputDialog::onOkClicked(MyGUI::Widget* /*sender*/)
    {
        if (mTextEdit->getCaption().empty())
        {
            MWBase::Environment::get().getWindowManager()->messageBox("#{sNotifyMessage37}");
            MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mTextEdit);
        }
        else
            eventDone(this);
    }

    void TextInputDialog::onTextAccepted(MyGUI::EditBox* sender)
    {
        onOkClicked(sender);

        // To do not spam onTextAccepted() again and again
        MWBase::Environment::get().getWindowManager()->injectKeyRelease(MyGUI::KeyCode::None);
    }

    std::string TextInputDialog::getTextInput() const
    {
        return mTextEdit->getCaption();
    }

    void TextInputDialog::setTextInput(const std::string& text)
    {
        mTextEdit->setCaption(text);
    }

    bool TextInputDialog::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            onOkClicked(nullptr);
            MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("Menu Click"));
            return true;
        }

        return false;
    }
}
