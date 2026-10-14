#include "frame3ds.hpp"

#include <platform3ds/debugscreen.hpp>
#include <platform3ds/log.hpp>
#include <platform3ds/platform.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwworld/cell.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/refdata.hpp"

#include "../profile.hpp"

#include <components/esm/position.hpp>

#include <osg/Stats>
#include <osgViewer/Viewer>
#include <components/esm3/loadbook.hpp>
#include <components/esm3/loaddial.hpp>

#include <3ds.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

        // Per-subsystem frame timings from osg::Stats (previous frame), summed between perf lines.
        struct Timing
        {
            std::string mLabel;
            std::string mAttribute;
            bool mCamera; // camera stats (cull/draw) instead of viewer stats
            double mSum = 0;
        };
        osgViewer::Viewer* gViewer = nullptr;
        std::vector<Timing> gTimings;
        unsigned gTimingFrames = 0;

        void accumulateTimings()
        {
            if (gViewer == nullptr)
                return;
            // Resource::StatsHandler turns "engine" off on every event while its overlay is hidden; turn it
            // back on before Engine::frame's ScopedProfiles run (the handler runs later, in eventTraversal).
            gViewer->getViewerStats()->collectStats("engine", true);
            const unsigned frame = gViewer->getFrameStamp()->getFrameNumber();
            if (frame == 0)
                return;
            osg::Stats* viewerStats = gViewer->getViewerStats();
            osg::Stats* cameraStats = gViewer->getCamera()->getStats();
            for (Timing& t : gTimings)
            {
                osg::Stats* stats = t.mCamera ? cameraStats : viewerStats;
                double value = 0;
                if (stats != nullptr && stats->getAttribute(frame - 1, t.mAttribute, value))
                    t.mSum += value;
            }
            ++gTimingFrames;
        }

        // PhaseTimer sums (keyed by label pointer).
        struct Phase
        {
            const char* mLabel;
            unsigned long long mTicks;
        };
        Phase gPhases[16];
        int gPhaseCount = 0;

        void logPhases(unsigned frames)
        {
            if (frames == 0 || gPhaseCount == 0)
                return;
            std::string line = "perf/more:";
            char item[48];
            for (int i = 0; i < gPhaseCount; ++i)
            {
                const double ms = double(gPhases[i].mTicks) * 1000.0 / SYSCLOCK_ARM11 / frames;
                std::snprintf(item, sizeof(item), " %s %.1f", gPhases[i].mLabel, ms);
                line += item;
                gPhases[i].mTicks = 0;
            }
            P3DS_LOGI("%s", line.c_str());
        }

        void logTimings()
        {
            logPhases(gTimingFrames);
            if (gTimingFrames == 0)
                return;
            std::string line = "perf/ms:";
            char item[48];
            for (Timing& t : gTimings)
            {
                const double ms = t.mSum * 1000.0 / gTimingFrames;
                if (ms >= 0.05)
                {
                    std::snprintf(item, sizeof(item), " %s %.1f", t.mLabel.c_str(), ms);
                    line += item;
                }
                t.mSum = 0;
            }
            gTimingFrames = 0;
            P3DS_LOGI("%s", line.c_str());
        }

        // Once: read a few texts that ESM::LazyString left in the content file, to check offsets and the
        // code page conversion in a headless run (books and dialogue can't be opened yet).
        void lazyStringSelfCheck()
        {
            const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
            int shown = 0;
            for (const ESM::Book& book : store.get<ESM::Book>())
            {
                const std::string& text = book.mText.get();
                P3DS_LOGI("lazy text: book %s: %zu bytes \"%.70s\"", book.mId.toDebugString().c_str(), text.size(),
                    text.c_str());
                if (++shown == 2)
                    break;
            }
            shown = 0;
            for (const ESM::Dialogue& dialogue : store.get<ESM::Dialogue>())
            {
                for (const ESM::DialInfo& info : dialogue.mInfo)
                {
                    if (info.mResponse.empty())
                        continue;
                    P3DS_LOGI("lazy text: %s: \"%.70s\"", dialogue.mStringId.c_str(), info.mResponse.get().c_str());
                    if (++shown == 2)
                        return;
                }
            }
        }

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

    namespace
    {
        // HOME menu / power button: SDL only services APT in its 3DS video driver, which we don't use.
        // aptMainLoop() blocks while the app is suspended and returns false when it must close.
        void serviceApt(bool loading)
        {
            if (aptMainLoop())
                return;
            static bool quitRequested = false;
            if (quitRequested)
                return;
            quitRequested = true;
            if (!loading)
            {
                P3DS_LOGI("APT: close requested, quitting");
                MWBase::Environment::get().getStateManager()->requestQuit();
                return;
            }
            // OpenMW can't abort a load; the user wants out now.
            P3DS_LOGI("APT: close requested while loading, exiting");
            p3ds::shutdown();
            svcExitProcess();
        }
    }

    unsigned long long phaseTicks()
    {
        return svcGetSystemTick();
    }

    void addPhaseTime(const char* label, unsigned long long ticks)
    {
        for (int i = 0; i < gPhaseCount; ++i)
            if (gPhases[i].mLabel == label)
            {
                gPhases[i].mTicks += ticks;
                return;
            }
        if (gPhaseCount < int(sizeof(gPhases) / sizeof(gPhases[0])))
            gPhases[gPhaseCount++] = Phase{ label, ticks };
    }

    void setViewer(osgViewer::Viewer* viewer)
    {
        gViewer = viewer;
        osg::Stats* stats = viewer->getViewerStats();
        stats->collectStats("engine", true); // OMW::ScopedProfile in Engine::frame
        stats->collectStats("event", true);
        stats->collectStats("update", true);
        if (osg::Stats* cameraStats = viewer->getCamera()->getStats())
            cameraStats->collectStats("rendering", true); // cull/draw
        gTimings.clear();
        OMW::forEachUserStatsValue([](const OMW::UserStats& value) {
            gTimings.push_back(Timing{ value.mLabel, value.mTaken, false });
        });
        gTimings.push_back(Timing{ "event", "Event traversal time taken", false });
        gTimings.push_back(Timing{ "update", "Update traversal time taken", false });
        gTimings.push_back(Timing{ "cull", "Cull traversal time taken", true });
        gTimings.push_back(Timing{ "draw", "Draw traversal time taken", true });
    }

    void onLoadingTick()
    {
        serviceApt(true);
        if (!gBottomReady)
            return;
        p3ds::GameSnapshot snap;
        snap.frame = &gStats;
        snap.cellName = gCellName == "-" ? "(loading)" : gCellName.c_str();
        snap.status = "loading";
        gBottom.update(snap);
        gBottom.render(); // rate-limited internally
    }

    void onEngineFrame()
    {
        serviceApt(false);
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
        accumulateTimings();
        if (++gStats.frameIndex == 100)
            lazyStringSelfCheck();

        // Automated runs: "quit:N" in sdmc:/openmw/autorun.txt quits after N game frames (tests shutdown).
        static unsigned quitAtFrame = [] {
            unsigned frames = 0;
            if (std::FILE* f = std::fopen("sdmc:/openmw/autorun.txt", "r"))
            {
                char buf[128] = {};
                std::fread(buf, 1, sizeof(buf) - 1, f);
                std::fclose(f);
                if (const char* q = std::strstr(buf, "quit:"))
                    frames = unsigned(std::strtoul(q + 5, nullptr, 10));
            }
            return frames;
        }();
        if (quitAtFrame != 0 && gStats.frameIndex == quitAtFrame)
        {
            P3DS_LOGI("autorun: quitting after %u frames", quitAtFrame);
            MWBase::Environment::get().getStateManager()->requestQuit();
        }

        gLogAccum += frameSeconds;
        ++gLogFrames;
        if (ms > gLogWorst)
            gLogWorst = ms;
        if (gLogAccum >= 5.0)
        {
            P3DS_LOGI("perf: %u frames in %.1f s, %.1f fps, avg %.1f ms, worst %.1f ms [%s]", gLogFrames, gLogAccum,
                gLogFrames / gLogAccum, gLogAccum * 1000.0 / gLogFrames, gLogWorst, gCellName.c_str());
            logTimings();
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
        PhaseTimer phase("bottomScreen");
        gBottom.render(); // rate-limited internally
    }

    void shutdownBottomScreen()
    {
        if (gBottomReady)
            gBottom.shutdown();
        gBottomReady = false;
    }
}
