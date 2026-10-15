#pragma once

#include <platform3ds/bottomscreen.hpp>

namespace p3ds
{
    // Bottom-screen debug display: build, then (debug builds) FPS, cell, position and view, the object in reach,
    // then the game state, the teleport buttons (debug builds), the screenshot button, and the watchdog's alive
    // line at the very bottom (crash.cpp). Everything else is in the log. Redraws at most kRedrawHz, only the
    // characters that changed: a few hundred microseconds every 250 ms, and zero when hidden.
    class DebugBottomScreen final : public IBottomScreen
    {
    public:
        static constexpr float kRedrawHz = 4.f;
        static constexpr int kMaxTeleports = 8;

        void init() override;
        void shutdown() override;
        void update(const GameSnapshot& snapshot) override;
        void render() override;
        void onTouch(const TouchEvent& event) override;
        void setVisible(bool visible) override;
        bool isVisible() const override { return mVisible; }

        // True once after the SCREENSHOT button was tapped.
        bool consumeScreenshotRequest();

        // Teleport buttons (debug builds): two columns of up to kMaxTeleports labels (the strings must outlive
        // the screen). consumeTeleportRequest returns the tapped button's index once, else -1.
        void setTeleportTargets(const char* const* labels, int count);
        int consumeTeleportRequest();

        // Redraws immediately, ignoring the rate limit (progress display during long blocking steps).
        void forceRedraw();

        // Cost of the last redraw, for self-profiling.
        float lastRedrawMs() const { return mLastRedrawMs; }

    private:
        void redraw();
        void drawStatic();

        GameSnapshot mSnapshot;
        FrameStats mFrameCopy;
        char mCell[64] = {};
        char mStatus[64] = {};
        char mLooking[64] = {};
        char mState[64] = {};
        char mTeleportNote[48] = {};
        const char* const* mTeleportLabels = nullptr;
        int mTeleportCount = 0;
        int mTeleportRequest = -1;
        bool mScreenshotRequested = false;
        bool mVisible = true;
        bool mNeedsClear = true;
        uint64_t mLastRedrawTick = 0;
        float mLastRedrawMs = 0.f;
    };
}
