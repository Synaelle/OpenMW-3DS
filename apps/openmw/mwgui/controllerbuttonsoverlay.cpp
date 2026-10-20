#include "controllerbuttonsoverlay.hpp"

#include <algorithm>
#include <vector>

#include <MyGUI_Window.h>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/windowmanager.hpp"

namespace MWGui
{
    static constexpr ControllerButtonsOverlay::ButtonDefinition sButtonDefs[] = {
        { ControllerButtonsOverlay::Button::Button_A, "A", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_A }, &ControllerButtons::mA },
        { ControllerButtonsOverlay::Button::Button_B, "B", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_B }, &ControllerButtons::mB },
        { ControllerButtonsOverlay::Button::Button_Dpad, "Dpad", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_DPAD_UP }, &ControllerButtons::mDpad },
        { ControllerButtonsOverlay::Button::Button_L1, "L1", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_LEFTSHOULDER }, &ControllerButtons::mL1 },
        { ControllerButtonsOverlay::Button::Button_L2, "L2", ControllerButtonsOverlay::InputType_Axis,
            { .mAxis = SDL_CONTROLLER_AXIS_TRIGGERLEFT }, &ControllerButtons::mL2 },
        { ControllerButtonsOverlay::Button::Button_L3, "L3", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_LEFTSTICK }, &ControllerButtons::mL3 },
        { ControllerButtonsOverlay::Button::Button_LStick, "LStick", ControllerButtonsOverlay::InputType_Axis,
            { .mAxis = SDL_CONTROLLER_AXIS_LEFTY }, &ControllerButtons::mLStick },
        { ControllerButtonsOverlay::Button::Button_Menu, "Menu", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_BACK }, &ControllerButtons::mMenu },
        { ControllerButtonsOverlay::Button::Button_R1, "R1", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER }, &ControllerButtons::mR1 },
        { ControllerButtonsOverlay::Button::Button_R2, "R2", ControllerButtonsOverlay::InputType_Axis,
            { .mAxis = SDL_CONTROLLER_AXIS_TRIGGERRIGHT }, &ControllerButtons::mR2 },
        { ControllerButtonsOverlay::Button::Button_R3, "R3", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_RIGHTSTICK }, &ControllerButtons::mR3 },
        { ControllerButtonsOverlay::Button::Button_RStick, "RStick", ControllerButtonsOverlay::InputType_Axis,
            { .mAxis = SDL_CONTROLLER_AXIS_RIGHTY }, &ControllerButtons::mRStick },
        { ControllerButtonsOverlay::Button::Button_View, "View", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_START }, &ControllerButtons::mView },
        { ControllerButtonsOverlay::Button::Button_X, "X", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_X }, &ControllerButtons::mX },
        { ControllerButtonsOverlay::Button::Button_Y, "Y", ControllerButtonsOverlay::InputType_Button,
            { .mButton = SDL_CONTROLLER_BUTTON_Y }, &ControllerButtons::mY },
    };

    ControllerButtonsOverlay::ControllerButtonsOverlay()
        : WindowBase("openmw_controllerbuttons.layout")
    {
        MWBase::InputManager* inputMgr = MWBase::Environment::get().getInputManager();

        for (size_t i = 0; i < mButtons.size(); i++)
        {
            getWidget(mButtons[i].mImage, "Btn" + std::string(sButtonDefs[i].mName) + "Image");
            getWidget(mButtons[i].mText, "Btn" + std::string(sButtonDefs[i].mName) + "Text");
            getWidget(mButtons[i].mHBox, "Btn" + std::string(sButtonDefs[i].mName));

            if (sButtonDefs[i].mInputType == InputType_Axis)
                setIcon(mButtons[i].mImage, inputMgr->getControllerAxisIcon(sButtonDefs[i].mId.mAxis));
            else
                setIcon(mButtons[i].mImage, inputMgr->getControllerButtonIcon(sButtonDefs[i].mId.mButton));
        }

        getWidget(mHBox, "ButtonBox");
    }

    int ControllerButtonsOverlay::getHeight()
    {
        MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>();
        return window->getHeight();
    }

    void ControllerButtonsOverlay::setButtons(ControllerButtons* buttons)
    {
        int buttonCount = 0;
        if (buttons != nullptr)
        {
            for (const auto& row : sButtonDefs)
            {
#ifdef OPENMW_PLATFORM_3DS
                // No stick clicks on the 3DS (R3: "Info"), and ZL/ZR's other window (the inventory) is the touch
                // screen's page: not shown (the tester, test #97).
                if (row.mButton == Button_R3 || row.mButton == Button_L3 || row.mButton == Button_L2
                    || row.mButton == Button_R2)
                {
                    updateButton(row.mButton, {});
                    continue;
                }
#endif
                buttonCount += updateButton(row.mButton, buttons->*(row.mField));
            }

#ifdef OPENMW_PLATFORM_3DS
            // M17: the strip is the bottom screen's width. When a window's hints don't fit, the least needed go
            // first: the sticks (their use shows on screen), B (back, on every 3DS window), START and SELECT, then
            // the page and category switches (the tab bars show them).
            constexpr int kWidth = 312;
            constexpr int kSpacing = 6; // openmw_controllerbuttons.layout
            auto width = [this] {
                int total = 0;
                int shown = 0;
                for (const ButtonWidgets& button : mButtons)
                    if (button.mHBox->getVisible())
                    {
                        total += button.mImage->getWidth() + 2 + button.mText->getTextSize().width;
                        ++shown;
                    }
                return total + std::max(0, shown - 1) * kSpacing;
            };
            // L and R (ZL and ZR) go together: "L Prev" without "R Next" read wrong (the journal).
            const std::vector<std::vector<Button>> drops = { { Button_RStick }, { Button_LStick }, { Button_B },
                { Button_View }, { Button_Menu }, { Button_L2, Button_R2 }, { Button_L1, Button_R1 } };
            for (const std::vector<Button>& group : drops)
            {
                if (width() <= kWidth)
                    break;
                for (Button drop : group)
                    if (mButtons[drop].mHBox->getVisible())
                    {
                        updateButton(drop, {});
                        --buttonCount;
                    }
            }
#endif
            mHBox->notifyChildrenSizeChanged();
        }

        setVisible(buttonCount > 0);
    }

    void ControllerButtonsOverlay::setIcon(MyGUI::ImageBox* image, const std::string& imagePath)
    {
        if (!imagePath.empty())
            image->setImageTexture(imagePath);
    }

    int ControllerButtonsOverlay::updateButton(ControllerButtonsOverlay::Button button, const std::string& buttonStr)
    {
        if (buttonStr.empty())
        {
            mButtons[button].mHBox->setVisible(false);
            mButtons[button].mHBox->setUserString("Hidden", "true");
            return 0;
        }
        else
        {
            mButtons[button].mHBox->setVisible(true);
            mButtons[button].mHBox->setUserString("Hidden", "false");
            mButtons[button].mText->setCaptionWithReplacing(buttonStr);
            return 1;
        }
    }
}
