#include "hidinject.hpp"

#include <3ds.h>

namespace
{
    u32 gHeld = 0;
    u32 gDown = 0;
    u32 gUp = 0;
    omw3ds::hidinject::Stick gCircle;
    omw3ds::hidinject::Stick gCStick;
    omw3ds::hidinject::Touch gTouch;

    void applyStick(circlePosition* pos, const omw3ds::hidinject::Stick& stick)
    {
        if (!stick.mActive)
            return;
        // SDL's driver scales +-160 to the full axis range.
        pos->dx = s16(stick.mX * 160.f);
        pos->dy = s16(stick.mY * 160.f);
    }
}

namespace omw3ds::hidinject
{
    void set(u32 held, const Stick& circle, const Stick& cstick, const Touch& touch)
    {
        gDown = held & ~gHeld;
        gUp = gHeld & ~held;
        gHeld = held;
        gCircle = circle;
        gCStick = cstick;
        gTouch = touch;
    }
}

extern "C"
{
    u32 __real_hidKeysDown(void);
    u32 __real_hidKeysUp(void);
    u32 __real_hidKeysHeld(void);
    void __real_hidCircleRead(circlePosition* pos);
    void __real_irrstCstickRead(circlePosition* pos);
    void __real_hidTouchRead(touchPosition* pos);

    u32 __wrap_hidKeysDown(void)
    {
        return __real_hidKeysDown() | gDown;
    }

    u32 __wrap_hidKeysUp(void)
    {
        return __real_hidKeysUp() | gUp;
    }

    u32 __wrap_hidKeysHeld(void)
    {
        return __real_hidKeysHeld() | gHeld;
    }

    void __wrap_hidCircleRead(circlePosition* pos)
    {
        __real_hidCircleRead(pos);
        applyStick(pos, gCircle);
    }

    void __wrap_irrstCstickRead(circlePosition* pos)
    {
        __real_irrstCstickRead(pos);
        applyStick(pos, gCStick);
    }

    void __wrap_hidTouchRead(touchPosition* pos)
    {
        __real_hidTouchRead(pos);
        if (gTouch.mActive)
        {
            pos->px = gTouch.mX;
            pos->py = gTouch.mY;
        }
    }
}
