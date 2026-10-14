#include "frame3ds.hpp"

#include <platform3ds/debugscreen.hpp>
#include <platform3ds/log.hpp>
#include <platform3ds/memprobe.hpp>
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
#include <components/render3ds/render3ds.hpp>
#include <components/settings/values.hpp>

#include <osg/Stats>
#include <osgViewer/Viewer>
#include <components/esm3/loadbook.hpp>
#include <components/esm3/loaddial.hpp>

#include <3ds.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>

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

        // Renderer counters summed between perf lines.
        struct RenderSums
        {
            double mDraws = 0, mTriangles = 0, mLeaves = 0, mSkipped = 0, mUploadKb = 0, mEmitMs = 0, mGpuMs = 0;
            float mGpuWorst = 0;
            float mCmdBufPeak = 0;
            unsigned mVerified = 0;
        } gRender;

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

        void logTimings()
        {
            p3ds::perf::logAndReset(gTimingFrames); // perf/more (engine.cpp OMW3DS_PHASE, sceneutil, ...)
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

    void setViewer(osgViewer::Viewer* viewer)
    {
        // The settings that matter for performance, as actually loaded (the SD card may hold an older
        // settings.cfg than the package).
        P3DS_LOGI("settings: physics async threads %d, lua threads %d, viewing distance %.0f, preload %s, "
                  "actors range %.0f",
            int(Settings::physics().mAsyncNumThreads), int(Settings::lua().mLuaNumThreads),
            double(Settings::camera().mViewingDistance), Settings::cells().mPreloadEnabled ? "on" : "off",
            double(Settings::game().mActorsProcessingRange));
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

        // Automated runs, sdmc:/openmw/autorun.txt: "quit:N" quits after N game frames (tests shutdown),
        // "shot:N,M,..." saves the top screen at those frames (sdmc:/openmw/shot_top_N.bmp).
        struct Autorun
        {
            unsigned mQuitAt = 0;
            std::vector<unsigned> mShots;
            float mTurnDegrees = 0; // "turn:D": rotate the player by D degrees at frame 30
        };
        static const Autorun autorun = [] {
            Autorun result;
            if (std::FILE* f = std::fopen("sdmc:/openmw/autorun.txt", "r"))
            {
                char buf[256] = {};
                std::fread(buf, 1, sizeof(buf) - 1, f);
                std::fclose(f);
                if (const char* q = std::strstr(buf, "quit:"))
                    result.mQuitAt = unsigned(std::strtoul(q + 5, nullptr, 10));
                if (const char* q = std::strstr(buf, "turn:"))
                    result.mTurnDegrees = std::strtof(q + 5, nullptr);
                if (const char* q = std::strstr(buf, "shot:"))
                {
                    char* end = const_cast<char*>(q + 5);
                    do
                        result.mShots.push_back(unsigned(std::strtoul(end + (*end == ',' ? 1 : 0), &end, 10)));
                    while (*end == ',');
                }
            }
            return result;
        }();
        if (autorun.mQuitAt != 0 && gStats.frameIndex == autorun.mQuitAt)
        {
            P3DS_LOGI("autorun: quitting after %u frames", autorun.mQuitAt);
            MWBase::Environment::get().getStateManager()->requestQuit();
        }
        for (unsigned shot : autorun.mShots)
            if (shot == gStats.frameIndex)
                Render3DS::requestCapture(("sdmc:/openmw/shot_top_" + std::to_string(shot) + ".bmp").c_str());
        // Buttons: SDL's 3DS joystick driver reads hidKeysDown/Up but only its (unused) video driver scans,
        // so scan once per frame here. SELECT = top-screen screenshot (temporary, until M4 maps controls).
        hidScanInput();
        // D-pad left/right: turn the player 45 degrees (debug camera until M4 brings real controls).
        float turn = 0;
        if (hidKeysDown() & KEY_DLEFT)
            turn = 45;
        if (hidKeysDown() & KEY_DRIGHT)
            turn = -45;
        if (gStats.frameIndex == 30)
            turn += autorun.mTurnDegrees;
        if (turn != 0)
        {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            const MWWorld::Ptr player = world->getPlayerPtr();
            osg::Vec3f rot = player.getRefData().getPosition().asRotationVec3();
            rot.z() += osg::DegreesToRadians(turn);
            world->rotateObject(player, rot, MWBase::RotationFlag_none);
            P3DS_LOGI("debug: player turned to %.0f degrees", double(osg::RadiansToDegrees(rot.z())));
        }
        if (hidKeysDown() & KEY_SELECT)
        {
            static unsigned count = 0;
            if (count == 0)
                mkdir("sdmc:/openmw/screenshots", 0777); // may be missing (empty folders and FTP copies)
            char path[64];
            std::snprintf(path, sizeof(path), "sdmc:/openmw/screenshots/top_%03u.bmp", count++);
            Render3DS::requestCapture(path);
        }

        const Render3DS::FrameStats& render = Render3DS::getLastFrameStats();
        gStats.drawCalls = render.mDraws;
        gStats.triangles = render.mTriangles;
        gStats.cpuMs = render.mEmitMs;
        gStats.gpuMs = render.mGpuMs;
        gRender.mDraws += render.mDraws;
        gRender.mTriangles += render.mTriangles;
        gRender.mLeaves += render.mLeaves;
        gRender.mSkipped += render.mSkipped;
        gRender.mUploadKb += render.mUploadedBytes / 1024.0;
        gRender.mEmitMs += render.mEmitMs;
        gRender.mGpuMs += render.mGpuMs;
        gRender.mGpuWorst = std::max(gRender.mGpuWorst, render.mGpuMs);
        gRender.mCmdBufPeak = std::max(gRender.mCmdBufPeak, render.mCmdBufUsage);
        gRender.mVerified += render.mVerified;

        gLogAccum += frameSeconds;
        ++gLogFrames;
        if (ms > gLogWorst)
            gLogWorst = ms;
        if (gLogAccum >= 5.0)
        {
            P3DS_LOGI("perf: %u frames in %.1f s, %.1f fps, avg %.1f ms, worst %.1f ms [%s] pos %.0f %.0f %.0f",
                gLogFrames, gLogAccum, gLogFrames / gLogAccum, gLogAccum * 1000.0 / gLogFrames, gLogWorst,
                gCellName.c_str(), double(gPos[0]), double(gPos[1]), double(gPos[2]));
            logTimings();
            if (Render3DS::isActive())
            {
                const double n = gLogFrames;
                P3DS_LOGI("gpu: per frame %.0f draws, %.0f tris, %.0f leaves (%.0f skipped), emit %.1f ms, "
                          "PICA %.1f ms (worst %.1f), uploads %.0f KB/frame, %lu geometries, linear free %lu KB, "
                          "cmdbuf peak %.0f%%, %u new objects GPU-checked",
                    gRender.mDraws / n, gRender.mTriangles / n, gRender.mLeaves / n, gRender.mSkipped / n,
                    gRender.mEmitMs / n, gRender.mGpuMs / n, double(gRender.mGpuWorst), gRender.mUploadKb / n,
                    (unsigned long)render.mGeometries, (unsigned long)render.mLinearFreeKb,
                    double(gRender.mCmdBufPeak) * 100.0, gRender.mVerified);
            }
            gRender = RenderSums{};
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
        snap.status = Render3DS::isActive() ? "citro3d" : "headless (renderer failed)";
        gBottom.update(snap);
        p3ds::perf::Phase phase("bottomScreen");
        gBottom.render(); // rate-limited internally
    }

    void shutdownBottomScreen()
    {
        if (gBottomReady)
            gBottom.shutdown();
        gBottomReady = false;
    }
}
