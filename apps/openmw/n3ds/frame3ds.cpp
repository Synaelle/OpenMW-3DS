#include "frame3ds.hpp"

#include <platform3ds/debugscreen.hpp>
#include <platform3ds/log.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwworld/cell.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/refdata.hpp"

#include <components/esm/position.hpp>

#include <3ds.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace omw3ds
{
    namespace
    {
        p3ds::DebugBottomScreen gBottom;
        p3ds::FrameTimer gTimer;
        p3ds::FrameStats gStats;
        bool gBottomReady = false;

        // Cell name / position are refreshed a few times a second only (string work).
        std::string gCellName = "-";
        float gPos[3] = {};
        unsigned gFramesSinceInfo = 0;

        // Frame-time log every 5 s.
        double gLogAccum = 0;
        unsigned gLogFrames = 0;
        float gLogWorst = 0;

        void refreshWorldInfo()
        {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            if (world == nullptr)
                return;
            const MWWorld::Ptr player = world->getPlayerPtr();
            if (player.isEmpty() || !player.isInCell())
                return;
            gCellName = player.getCell()->getCell()->getDescription();
            const ESM::Position& pos = player.getRefData().getPosition();
            for (int i = 0; i < 3; ++i)
                gPos[i] = pos.pos[i];
        }
    }

    void initBottomScreen(const char* status)
    {
        gBottom.init();
        gBottomReady = true;
        p3ds::GameSnapshot snap;
        snap.cellName = status;
        gBottom.update(snap);
        gBottom.forceRedraw();
    }

    void onEngineFrame()
    {
        static uint64_t lastTick = 0;
        const uint64_t now = svcGetSystemTick();
        if (lastTick == 0)
        {
            lastTick = now;
            return;
        }
        const float frameSeconds = float(double(now - lastTick) / SYSCLOCK_ARM11);
        lastTick = now;
        const float ms = frameSeconds * 1000.f;
        gTimer.addFrame(ms);
        ++gStats.frameIndex;

        gLogAccum += frameSeconds;
        ++gLogFrames;
        if (ms > gLogWorst)
            gLogWorst = ms;
        if (gLogAccum >= 5.0)
        {
            P3DS_LOGI("perf: %u frames in %.1f s, %.1f fps, avg %.1f ms, worst %.1f ms [%s]", gLogFrames, gLogAccum,
                gLogFrames / gLogAccum, gLogAccum * 1000.0 / gLogFrames, gLogWorst, gCellName.c_str());
            gLogAccum = 0;
            gLogFrames = 0;
            gLogWorst = 0;
        }

        if (!gBottomReady)
            return;
        if (++gFramesSinceInfo >= 8)
        {
            gFramesSinceInfo = 0;
            refreshWorldInfo();
        }
        gTimer.fill(gStats);
        p3ds::GameSnapshot snap;
        snap.frame = &gStats;
        snap.cellName = gCellName.c_str();
        for (int i = 0; i < 3; ++i)
            snap.playerPos[i] = gPos[i];
        snap.status = "headless (no renderer yet)";
        gBottom.update(snap);
        gBottom.render(); // rate-limited internally
    }

    void shutdownBottomScreen()
    {
        if (gBottomReady)
            gBottom.shutdown();
        gBottomReady = false;
    }
}
