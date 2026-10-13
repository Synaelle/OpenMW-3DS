// Nintendo 3DS implementation of SDLUtil::GraphicsWindowSDL2 (replaces sdlgraphicswindow.cpp).
//
// There is no OpenGL on the 3DS: SDL runs its "dummy" video driver (events/joystick/touch/timers only),
// and OSG renders through a stub GL. This window owns no GL context; buffer swaps are forwarded to a
// platform hook, which is where the port's citro3d backend presents a frame.

#include "sdlgraphicswindow.hpp"

#include <SDL_video.h>

namespace SDLUtil
{
    // Set by the platform layer (citro3d backend). Called once per OSG frame from swapBuffers().
    void (*gSwapBuffersHook3DS)() = nullptr;

    GraphicsWindowSDL2::~GraphicsWindowSDL2()
    {
        close(true);
    }

    GraphicsWindowSDL2::GraphicsWindowSDL2(osg::GraphicsContext::Traits* traits, VSyncMode vsyncMode)
        : mWindow(nullptr)
        , mContext(nullptr)
        , mValid(false)
        , mRealized(false)
        , mOwnsWindow(false)
        , mVSyncMode(vsyncMode)
    {
        _traits = traits;
        init();
        if (valid())
        {
            setState(new osg::State);
            getState()->setGraphicsContext(this);
            if (_traits.valid() && _traits->sharedContext.valid())
            {
                getState()->setContextID(_traits->sharedContext->getState()->getContextID());
                incrementContextIDUsageCount(getState()->getContextID());
            }
            else
                getState()->setContextID(osg::GraphicsContext::createNewContextID());
        }
    }

    void GraphicsWindowSDL2::init()
    {
        if (mValid)
            return;
        if (!_traits.valid())
            return;
        WindowData* inheritedWindowData = dynamic_cast<WindowData*>(_traits->inheritedWindowData.get());
        mWindow = inheritedWindowData ? inheritedWindowData->mWindow : nullptr;
        mOwnsWindow = false;
        // 3DS top screen. The traits come from the SDL dummy window created by Engine::createWindow.
        if (_traits->width <= 0 || _traits->height <= 0)
        {
            _traits->width = 400;
            _traits->height = 240;
        }
        _traits->samples = 0;
        mValid = true;
        getEventQueue()->syncWindowRectangleWithGraphicsContext();
    }

    bool GraphicsWindowSDL2::realizeImplementation()
    {
        if (mRealized)
            return true;
        if (!mValid)
            init();
        mRealized = mValid;
        return mRealized;
    }

    void GraphicsWindowSDL2::closeImplementation()
    {
        mContext = nullptr;
        mWindow = nullptr;
        mValid = false;
        mRealized = false;
    }

    bool GraphicsWindowSDL2::makeCurrentImplementation()
    {
        return mRealized;
    }

    bool GraphicsWindowSDL2::releaseContextImplementation()
    {
        return true;
    }

    void GraphicsWindowSDL2::swapBuffersImplementation()
    {
        if (gSwapBuffersHook3DS)
            gSwapBuffersHook3DS();
    }

    void GraphicsWindowSDL2::setSyncToVBlank(bool on)
    {
        setSyncToVBlank(on ? VSyncMode::Enabled : VSyncMode::Disabled);
    }

    void GraphicsWindowSDL2::setSyncToVBlank(VSyncMode mode)
    {
        mVSyncMode = mode; // presentation timing is owned by the citro3d backend
    }

    void GraphicsWindowSDL2::setSwapInterval(VSyncMode) {}

    bool GraphicsWindowSDL2::setWindowDecorationImplementation(bool)
    {
        return true;
    }

    void GraphicsWindowSDL2::raiseWindow() {}

    bool GraphicsWindowSDL2::setWindowRectangleImplementation(int, int, int, int)
    {
        return true; // fixed 400x240 screen
    }

    void GraphicsWindowSDL2::setWindowName(const std::string&) {}

    void GraphicsWindowSDL2::setCursor(MouseCursor) {}
}
