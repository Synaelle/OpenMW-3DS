#include <platform3ds/debugscreen.hpp>
#include <platform3ds/platform.hpp>
#include <platform3ds/textfb.hpp>
#include <platform3ds/timing.hpp>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace p3ds
{
    void FrameTimer::addFrame(float frameMs)
    {
        mLast = frameMs;
        mSamples[mNext] = frameMs;
        mNext = (mNext + 1) % kWindow;
        if (mCount < kWindow)
            ++mCount;
    }

    void FrameTimer::fill(FrameStats& out) const
    {
        out.frameMs = mLast;
        if (mCount == 0)
            return;
        float sum = 0.f, worst = 0.f;
        for (int i = 0; i < mCount; ++i)
        {
            sum += mSamples[i];
            if (mSamples[i] > worst)
                worst = mSamples[i];
        }
        out.fps = sum > 0.f ? 1000.f * float(mCount) / sum : 0.f;
        out.fpsLow = worst > 0.f ? 1000.f / worst : 0.f;
    }

    namespace
    {
        // Rows (8 px each, 30 in all; the last one is the watchdog's alive line, crash.cpp).
        constexpr int kTeleportLabelRow = 7;
        constexpr int kTeleportFirstRow = 8; // buttons: 3 rows tall, every 4 rows, two columns
        constexpr int kTeleportPitch = 4;
        constexpr int kTeleportNoteRow = 24;
        // Screenshot button above the alive line: rows 26-28, 24 px tall.
        constexpr int kButtonRow = textfb::kRows - 4;
        constexpr int kButtonRows = 3;

        // Shadow of what is on screen, so a redraw only touches characters that changed.
        char gShadow[textfb::kRows][textfb::kCols];
        uint16_t gShadowFg[textfb::kRows][textfb::kCols];
        bool gStaticDrawn = false;

        void invalidateShadow()
        {
            memset(gShadow, 0, sizeof(gShadow));
            gStaticDrawn = false;
        }

        void row(int r, uint16_t fg, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
        void row(int r, uint16_t fg, const char* fmt, ...)
        {
            char buf[textfb::kCols + 1];
            va_list args;
            va_start(args, fmt);
            vsnprintf(buf, sizeof(buf), fmt, args);
            va_end(args);
            // Pad to the full width so stale characters are overwritten without clearing.
            size_t n = strlen(buf);
            while (n < size_t(textfb::kCols))
                buf[n++] = ' ';
            buf[textfb::kCols] = '\0';
            for (int c = 0; c < textfb::kCols; ++c)
            {
                if (gShadow[r][c] == buf[c] && gShadowFg[r][c] == fg)
                    continue;
                gShadow[r][c] = buf[c];
                gShadowFg[r][c] = fg;
                textfb::drawText(c, r, buf + c, fg, textfb::color::black, 1);
            }
        }

        // A filled box of `rows` rows from column col, with the label centered on its middle row.
        void button(int col, int firstRow, int width, int rows, const char* label)
        {
            char line[textfb::kCols + 1];
            const int n = std::min<int>(width, int(sizeof(line)) - 1);
            memset(line, ' ', size_t(n));
            line[n] = '\0';
            for (int r = 0; r < rows; ++r)
                textfb::drawText(col, firstRow + r, line, textfb::color::white, textfb::color::header);
            const int len = std::min<int>(int(strlen(label)), width);
            textfb::drawText(col + (width - len) / 2, firstRow + rows / 2, label, textfb::color::white,
                textfb::color::header, len);
        }
    }

    void DebugBottomScreen::init()
    {
        mNeedsClear = true;
    }

    void DebugBottomScreen::shutdown()
    {
        textfb::clear(textfb::color::black);
        textfb::present();
    }

    void DebugBottomScreen::setVisible(bool visible)
    {
        if (visible == mVisible)
            return;
        mVisible = visible;
        mNeedsClear = true;
        mLastRedrawTick = 0;
    }

    void DebugBottomScreen::update(const GameSnapshot& snapshot)
    {
        if (!mVisible)
            return;
        // Copy what we need: the pointers in the snapshot are only valid during this call.
        if (snapshot.frame != nullptr)
            mFrameCopy = *snapshot.frame;
        mSnapshot = snapshot;
        mSnapshot.frame = &mFrameCopy;
        strncpy(mCell, snapshot.cellName ? snapshot.cellName : "-", sizeof(mCell) - 1);
        strncpy(mStatus, snapshot.status ? snapshot.status : "", sizeof(mStatus) - 1);
        strncpy(mLooking, snapshot.lookingAt ? snapshot.lookingAt : "", sizeof(mLooking) - 1);
        strncpy(mState, snapshot.state ? snapshot.state : "", sizeof(mState) - 1);
        mSnapshot.cellName = mCell;
        mSnapshot.status = mStatus;
        mSnapshot.lookingAt = mLooking;
        mSnapshot.state = mState;
    }

    void DebugBottomScreen::setTeleportTargets(const char* const* labels, int count)
    {
        mTeleportLabels = labels;
        mTeleportCount = std::min(count, kMaxTeleports);
        mNeedsClear = true;
    }

    void DebugBottomScreen::onTouch(const TouchEvent& event)
    {
        if (!mVisible || !event.down)
            return;
        const int r = event.y / 8;
        if (r >= kButtonRow && r < kButtonRow + kButtonRows)
            mScreenshotRequested = true;
        else if (mTeleportCount > 0 && r >= kTeleportFirstRow && r < kTeleportFirstRow + kMaxTeleports / 2 * kTeleportPitch)
        {
            const int index = (r - kTeleportFirstRow) / kTeleportPitch * 2 + (event.x < textfb::kWidth / 2 ? 0 : 1);
            if (index < mTeleportCount)
            {
                mTeleportRequest = index;
                snprintf(mTeleportNote, sizeof(mTeleportNote), "-> %s", mTeleportLabels[index]);
            }
        }
    }

    bool DebugBottomScreen::consumeScreenshotRequest()
    {
        const bool requested = mScreenshotRequested;
        mScreenshotRequested = false;
        return requested;
    }

    int DebugBottomScreen::consumeTeleportRequest()
    {
        const int request = mTeleportRequest;
        mTeleportRequest = -1;
        return request;
    }

    void DebugBottomScreen::render()
    {
        const uint64_t now = ticks();
        if (mNeedsClear)
        {
            textfb::clear(textfb::color::black);
            invalidateShadow();
            if (!mVisible)
            {
                textfb::present();
                mNeedsClear = false;
                return;
            }
            mNeedsClear = false;
        }
        if (!mVisible)
            return;
        const uint64_t interval = uint64_t(double(SYSCLOCK_ARM11) / kRedrawHz);
        if (mLastRedrawTick != 0 && now - mLastRedrawTick < interval)
            return;
        mLastRedrawTick = now;

        const uint64_t t0 = ticks();
        redraw();
        textfb::present();
        mLastRedrawMs = float(ticksToMs(ticks() - t0));
    }

    void DebugBottomScreen::forceRedraw()
    {
        mLastRedrawTick = 0;
        render();
    }

    // Header and buttons: drawn once per clear.
    void DebugBottomScreen::drawStatic()
    {
        using namespace textfb;
        const PlatformInfo& pi = info();
        char title[kCols + 1];
        // The build stamp ("YYYYMMDD-HHMMSS rev", UTC): "build 1014-2112" shows at a glance which CIA is installed
        // (the package README names the one it contains).
        char stamp[10] = "?";
        if (strlen(p3ds_build_id) >= 13)
            snprintf(stamp, sizeof(stamp), "%.4s-%.4s", p3ds_build_id + 4, p3ds_build_id + 9);
        snprintf(title, sizeof(title), " OpenMW3DS  %s %s  build %s", pi.isNew3DS ? "N3DS" : "O3DS",
            pi.speedupEnabled ? "804MHz" : "268MHz", stamp);
        fillRows(0, 1, color::header);
        drawText(0, 0, title, color::cyan, color::header);
        fillRows(kButtonRow, kButtonRows, color::header);
        static const char kButton[] = "[ tap: SCREENSHOT ]";
        drawText((kCols - int(sizeof(kButton) - 1)) / 2, kButtonRow + 1, kButton, color::white, color::header);
        if (mTeleportCount > 0)
        {
            drawText(1, kTeleportLabelRow, "teleport (coc):", color::grey, color::black);
            for (int i = 0; i < mTeleportCount; ++i)
                button(i % 2 == 0 ? 1 : kCols / 2 + 1, kTeleportFirstRow + i / 2 * kTeleportPitch, kCols / 2 - 2, 3,
                    mTeleportLabels[i]);
        }
        gStaticDrawn = true;
    }

    void DebugBottomScreen::redraw()
    {
        using namespace textfb;
        if (!gStaticDrawn)
            drawStatic();

#ifdef OPENMW3DS_DEBUG
        const FrameStats& f = mFrameCopy;
        const uint16_t fpsColor = f.fps >= 25.f ? color::green : (f.fps >= 15.f ? color::yellow : color::red);
        row(1, fpsColor, "FPS %5.1f  low %5.1f  frame %6.2f ms", f.fps, f.fpsLow, f.frameMs);
        row(2, color::cyan, "cell %.34s", mSnapshot.cellName);
        row(3, color::cyan, "pos %.0f %.0f %.0f  yaw %.0f pitch %.0f", mSnapshot.playerPos[0],
            mSnapshot.playerPos[1], mSnapshot.playerPos[2], mSnapshot.yaw, mSnapshot.pitch);
        row(4, color::cyan, "look %.34s", mLooking[0] ? mLooking : "-");
#endif
        row(5, mSnapshot.stateAlert ? color::yellow : color::cyan, "%s", mState);
        row(6, color::grey, "%s", mStatus);
        if (mTeleportCount > 0)
            row(kTeleportNoteRow, color::cyan, " %s", mTeleportNote);
    }
}
