#pragma once

#include <3ds/types.h>

// Scripted input for automated emulator runs (autorun.txt "btn:", "move:", "look:", "touch:"). The openmw link wraps
// libctru's hidKeysDown/Up/Held, hidCircleRead, irrstCstickRead (what hidCstickRead calls) and hidTouchRead with -Wl,--wrap
// (apps/openmw/CMakeLists.txt), so SDL's 3DS joystick driver sees injected buttons and stick positions as if
// they came from the hardware.
namespace omw3ds::hidinject
{
    struct Stick
    {
        bool mActive = false;
        float mX = 0, mY = 0; // -1..1, +y = up (libctru convention)
    };

    struct Touch
    {
        bool mActive = false; // KEY_TOUCH goes with it in `held`
        u16 mX = 0, mY = 0; // bottom-screen pixels
    };

    // Call once per frame, right after hidScanInput(): the state to add on top of the real one until the next
    // call. Button edges (down/up) are derived from the previous call.
    void set(u32 held, const Stick& circle, const Stick& cstick, const Touch& touch = {});
}
