#include "credits3ds.hpp"
#ifdef OPENMW_PLATFORM_3DS
#include <openmw3ds_version.h>
#endif

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_Gui.h>
#include <SDL_gamecontroller.h>

#include <components/debug/debuglog.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include "../mwbase/environment.hpp"
#include "../mwgui/windowbase.hpp"

namespace omw3ds
{
    namespace
    {
        class CreditsWindow : public MWGui::WindowModal
        {
        public:
            CreditsWindow()
                : WindowModal("openmw_credits3ds.layout")
            {
                getWidget(mText, "CreditsText");
                getWidget(mClose, "CloseButton");
                mClose->eventMouseButtonClick += MyGUI::newDelegate(this, &CreditsWindow::onCloseClicked);
                mText->setCaption(readText());
                // The right stick (C stick) scrolls the text (the tester, 20/10). Scrolled here: OpenMW's own
                // right-stick scroll goes through the GUI cursor, which the 3DS menus don't use.
                MyGUI::Gui::getInstance().eventFrameStart += MyGUI::newDelegate(this, &CreditsWindow::onFrameStart);
            }

            bool onControllerThumbstickEvent(const SDL_ControllerAxisEvent& arg) override
            {
                if (arg.axis != SDL_CONTROLLER_AXIS_RIGHTY)
                    return false;
                mStickY = std::abs(arg.value) < 4000 ? 0.f : arg.value / 32767.f;
                return true;
            }

            // B closes (the tester, 20/10).
            bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override
            {
                if (arg.button == SDL_CONTROLLER_BUTTON_B)
                {
                    setVisible(false);
                    return true;
                }
                return false;
            }

        private:
            static std::string readText()
            {
                constexpr VFS::Path::NormalizedView path("credits3ds.txt");
                const VFS::Manager& vfs = *MWBase::Environment::get().getResourceSystem()->getVFS();
                if (!vfs.exists(path))
                {
                    Log(Debug::Warning) << "credits3ds.txt not found";
                    return "OpenMW 3DS " OPENMW3DS_VERSION;
                }
                std::ostringstream text;
                text << vfs.get(path)->rdbuf();
                return text.str();
            }

            void onCloseClicked(MyGUI::Widget*) { setVisible(false); }

            void onFrameStart(float dt)
            {
                if (mStickY == 0.f || !isVisible())
                    return;
                const size_t range = mText->getVScrollRange();
                mScroll = std::clamp(mScroll + mStickY * dt * 300.f, 0.f, static_cast<float>(range));
                mText->setVScrollPosition(static_cast<size_t>(mScroll));
            }

        public:
            // Opens at the start of the text (an EditBox's caption leaves it scrolled to the end).
            void toTop()
            {
                mText->setVScrollPosition(0);
                mScroll = 0.f;
                mStickY = 0.f;
            }

        private:

            MyGUI::EditBox* mText = nullptr;
            MyGUI::Button* mClose = nullptr;
            float mStickY = 0.f; // -1 (up) to 1 (down)
            float mScroll = 0.f; // pixels from the top
        };

        // Kept until the program ends (destroying it after MyGUI's shutdown, as a static would, crashes); the
        // layout puts it on the top screen.
        CreditsWindow* sWindow = nullptr;
    }

    void showCredits()
    {
        if (sWindow == nullptr)
            sWindow = new CreditsWindow;
        sWindow->setVisible(true);
        sWindow->toTop();
    }
}
