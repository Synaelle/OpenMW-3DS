#include "benchmark3ds.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <components/esm/refid.hpp>
#include <components/esm/util.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadregn.hpp>
#include <components/render3ds/render3ds.hpp>

#include <platform3ds/log.hpp>
#include <platform3ds/memprobe.hpp>
#include <platform3ds/memstats.hpp>
#include <platform3ds/platform.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwworld/actionteleport.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/globals.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/refdata.hpp"
#include "../mwworld/store.hpp"

namespace omw3ds::benchmark
{
    namespace
    {
        // Morrowind's weathers by index (ESM::Weather::indexToRefId).
        enum Weather
        {
            Clear = 0,
            Rain = 4,
            Thunder = 5,
            Ash = 6,
            Snow = 8,
            Blizzard = 9,
        };
        const char* weatherName(int weather)
        {
            switch (weather)
            {
                case Clear:
                    return "clear";
                case Rain:
                    return "rain";
                case Thunder:
                    return "thunder";
                case Ash:
                    return "ash storm";
                case Snow:
                    return "snow";
                case Blizzard:
                    return "blizzard";
                default:
                    return "-";
            }
        }

        struct Stage
        {
            const char* mLabel;
            const char* mInterior; // nullptr: an exterior, at mPos facing mYaw (or found by mExterior's name)
            float mPos[3];
            float mYaw; // degrees
            int mWeather; // exteriors. -1: unchanged
            float mHour;
            const char* mExterior = nullptr; // an exterior found by its name (MWScript's COC), instead of mPos
        };
        // The places that weigh most on the frame rate (test #92: towns with people and rain, Vivec's Foreign
        // Quarter Plaza, ~730 draws). Weather only changes at once when the player enters a region, so the stages
        // alternate regions and interiors (WeatherManager forces a region's weather on entering it).
        constexpr Stage sStages[] = {
            { "Seyda Neen", nullptr, { -11615, -70115, 316 }, 91.7f, Clear, 12.f },
            { "Balmora, Council Club", "Balmora, Council Club", {}, 0.f, -1, 12.f },
            { "Balmora", nullptr, { -21792, -12087, 882 }, 270.f, Clear, 12.f },
            { "Vivec, Foreign Quarter Plaza", "Vivec, Foreign Quarter Plaza", {}, 0.f, -1, 12.f },
            { "Seyda Neen", nullptr, { -11615, -70115, 316 }, 91.7f, Rain, 19.f },
            { "Ald'ruhn", nullptr, { -13071, 51975, 2182 }, 292.9f, Ash, 12.f },
            { "Balmora", nullptr, { -21792, -12087, 882 }, 270.f, Thunder, 22.f },
            { "Pelagiad", nullptr, { 2507, -56048, 1570 }, 134.6f, Clear, 18.f },
        };
        // The long tour: towns (found by name, as MWScript's COC: a door marker or the cell's centre, never the
        // void) alternating with interiors, so each town's region weather is forced on arrival. Every name checked
        // against Morrowind.esm.
        constexpr Stage sTourStages[] = {
            { "Seyda Neen", nullptr, {}, 0.f, Clear, 9.f, "Seyda Neen" },
            // Not the Census and Excise Office: its first door destination is outside the room's walls, so the
            // player fell into the void again and again there (the tester, test #113).
            { "Arrille's Tradehouse", "Seyda Neen, Arrille's Tradehouse", {}, 0.f, -1, 9.f },
            { "Balmora", nullptr, {}, 0.f, Clear, 12.f, "Balmora" },
            { "Balmora, Eight Plates", "Balmora, Eight Plates", {}, 0.f, -1, 12.f },
            { "Pelagiad", nullptr, {}, 0.f, Rain, 18.f, "Pelagiad" },
            { "Pelagiad, Halfway Tavern", "Pelagiad, Halfway Tavern", {}, 0.f, -1, 18.f },
            { "Caldera", nullptr, {}, 0.f, Clear, 15.f, "Caldera" },
            { "Caldera Mine", "Caldera Mine", {}, 0.f, -1, 15.f },
            { "Ald-ruhn", nullptr, {}, 0.f, Ash, 12.f, "Ald-ruhn" },
            { "Ald-ruhn, Manor District", "Ald-ruhn, Manor District", {}, 0.f, -1, 12.f },
            { "Maar Gan", nullptr, {}, 0.f, Ash, 10.f, "Maar Gan" },
            { "Ghostgate", nullptr, {}, 0.f, Thunder, 21.f, "Ghostgate" },
            { "Ghostgate, Tower of Dusk", "Ghostgate, Tower of Dusk", {}, 0.f, -1, 21.f },
            { "Gnisis", nullptr, {}, 0.f, Clear, 8.f, "Gnisis" },
            { "Gnisis, Arvs-Drelen", "Gnisis, Arvs-Drelen", {}, 0.f, -1, 8.f },
            { "Khuul", nullptr, {}, 0.f, Rain, 16.f, "Khuul" },
            { "Ald Velothi", nullptr, {}, 0.f, Clear, 19.f, "Ald Velothi" },
            { "Ald Daedroth, Outer Shrine", "Ald Daedroth, Outer Shrine", {}, 0.f, -1, 19.f },
            { "Dagon Fel", nullptr, {}, 0.f, Clear, 7.f, "Dagon Fel" },
            { "Tel Mora", nullptr, {}, 0.f, Rain, 13.f, "Tel Mora" },
            { "Sadrith Mora", nullptr, {}, 0.f, Clear, 14.f, "Sadrith Mora" },
            { "Sadrith Mora, Gateway Inn", "Sadrith Mora, Gateway Inn", {}, 0.f, -1, 14.f },
            { "Molag Mar", nullptr, {}, 0.f, Ash, 17.f, "Molag Mar" },
            { "Arkngthand, Hall of Centrifuge", "Arkngthand, Hall of Centrifuge", {}, 0.f, -1, 17.f },
            { "Suran", nullptr, {}, 0.f, Clear, 20.f, "Suran" },
            { "Suran, Desele's House", "Suran, Desele's House of Earthly Delights", {}, 0.f, -1, 20.f },
            { "Vivec", nullptr, {}, 0.f, Clear, 11.f, "Vivec" },
            { "Vivec, High Fane", "Vivec, High Fane", {}, 0.f, -1, 11.f },
            { "Vivec, Arena Pit", "Vivec, Arena Pit", {}, 0.f, -1, 11.f },
            { "Ebonheart", nullptr, {}, 0.f, Thunder, 23.f, "Ebonheart" },
            { "Andrano Ancestral Tomb", "Andrano Ancestral Tomb", {}, 0.f, -1, 23.f },
            { "Hla Oad", nullptr, {}, 0.f, Clear, 6.f, "Hla Oad" },
        };

        struct StageSet
        {
            const char* mName;
            const Stage* mStages;
            int mCount;
            int mLaps; // times round the places
            double mSettle; // seconds after arriving: weather particles, actors appearing
            double mMeasure; // seconds measured, while the camera turns once round
            bool mScreenshots; // 3 top-screen screenshots per place (Zeste only: they weigh on the frame times)
        };
        // In the order of benchmark3ds.hpp's Set. The endurance run: 32 places x 2 laps x ~30 s with the loads,
        // about 30-35 minutes on the console.
        constexpr StageSet sSets[] = {
            { "Morrowind", sStages, int(std::size(sStages)), 1, 6.0, 30.0, false },
            { "Tour", sTourStages, int(std::size(sTourStages)), 1, 6.0, 30.0, true },
            { "Endurance", sTourStages, int(std::size(sTourStages)), 2, 5.0, 20.0, false },
        };
        const StageSet* gSet = &sSets[0];

        constexpr double kStartDelay = 3.0; // seconds of play before the first stage

        int stageCount()
        {
            return gSet->mCount * gSet->mLaps;
        }

        struct Result
        {
            double mLoadMs = 0;
            std::vector<float> mFrameMs;
            double mDraws = 0, mGpuMs = 0;
            float mHeapMB = 0, mArenaMB = 0, mLinearFreeMB = 0;
        };

        bool gEnabled = false;
        bool gDone = false;
        int gStage = -1; // -1: waiting for the game
        int gFirstStage = 0;
        double gStageTime = 0; // seconds since the current phase began
        bool gMeasuring = false;
        bool gJustTeleported = false;
        float gStartYaw = 0;
        uint64_t gLastTick = 0;
        std::vector<Result> gResults;

        double toSeconds(uint64_t ticks)
        {
            return double(ticks) / 268111856.0; // SYSCLOCK_ARM11
        }

        MWBase::World& world()
        {
            return *MWBase::Environment::get().getWorld();
        }

        void startStage(int index)
        {
            const Stage& stage = gSet->mStages[index % gSet->mCount];
            MWBase::World& w = world();
            w.setGlobalFloat(MWWorld::Globals::sGameHour, stage.mHour);
            ESM::Position pos{};
            if (stage.mInterior == nullptr)
            {
                if (stage.mExterior != nullptr)
                {
                    if (w.findExteriorPosition(stage.mExterior, pos).empty())
                        P3DS_LOGW("benchmark: no exterior %s", stage.mExterior);
                }
                else
                {
                    for (int i = 0; i < 3; ++i)
                        pos.pos[i] = stage.mPos[i];
                    pos.rot[2] = osg::DegreesToRadians(stage.mYaw);
                }
                const ESM::ExteriorCellLocation cell = ESM::positionToExteriorCellLocation(pos.pos[0], pos.pos[1]);
                // The region's weather first: it is forced on entering the region (the stage before is elsewhere).
                if (stage.mWeather >= 0)
                    if (const ESM::Cell* record
                        = MWBase::Environment::get().getESMStore()->get<ESM::Cell>().search(cell.mX, cell.mY))
                        if (!record->mRegion.empty())
                            w.changeWeather(record->mRegion, ESM::Weather::indexToRefId(stage.mWeather));
                w.changeToCell(ESM::RefId::esm3ExteriorCell(cell.mX, cell.mY), pos, true);
            }
            else if (const ESM::RefId refId = w.findInteriorPosition(stage.mInterior, pos); !refId.empty())
                MWWorld::ActionTeleport(refId, pos, false).execute(w.getPlayerPtr());
            else
                P3DS_LOGW("benchmark: no cell %s", stage.mInterior);
            P3DS_LOGI("benchmark: stage %d/%d, %s, %s, %02.0f:00", index + 1, stageCount(), stage.mLabel,
                weatherName(stage.mWeather), double(stage.mHour));
            gResults.emplace_back();
            gStageTime = 0;
            gMeasuring = false;
            gJustTeleported = true;
        }

        void setYaw(float yaw)
        {
            MWBase::World& w = world();
            const MWWorld::Ptr player = w.getPlayerPtr();
            osg::Vec3f rot = player.getRefData().getPosition().asRotationVec3();
            rot.x() = 0;
            rot.z() = yaw;
            w.rotateObject(player, rot, MWBase::RotationFlag_none);
        }

        std::string line(int index, const Result& r)
        {
            const Stage& stage = gSet->mStages[index % gSet->mCount];
            std::vector<float> sorted = r.mFrameMs;
            std::sort(sorted.begin(), sorted.end());
            const size_t n = sorted.size();
            double sum = 0;
            for (float ms : sorted)
                sum += ms;
            const double avg = n ? sum / n : 0;
            const float low1 = n ? sorted[std::min(n - 1, n * 99 / 100)] : 0; // the 1% slowest frames start here
            const float worst = n ? sorted.back() : 0;
            char buf[288];
            std::snprintf(buf, sizeof(buf),
                "%-3d %-30s %-9s %5.0f %8.0f %7.1f %7.1f %8.1f %8.1f %6.0f %7.1f %7.1f %8.1f %9.1f", index + 1,
                stage.mLabel, weatherName(stage.mWeather), double(stage.mHour), r.mLoadMs, avg > 0 ? 1000.0 / avg : 0.0,
                avg, double(low1), double(worst), n ? r.mDraws / n : 0.0, n ? r.mGpuMs / n : 0.0, double(r.mHeapMB),
                double(r.mArenaMB), double(r.mLinearFreeMB));
            return buf;
        }

        void finish()
        {
            gDone = true;
            const std::string header
                = "#   place                          weather    hour  load ms  avg fps  avg ms  1% low ms  worst ms"
                  "  draws  PICA ms heap MB arena MB linear MB";
            std::string file = std::string("benchmark-") + gSet->mName + ".txt";
            std::transform(file.begin(), file.end(), file.begin(), [](unsigned char c) { return std::tolower(c); });
            const std::string path = std::string(p3ds::logDir()) + "/" + file;
            std::ofstream out(path);
            out << "OpenMW 3DS benchmark, " << gSet->mName << ": " << gSet->mCount << " places";
            if (gSet->mLaps > 1)
                out << ", " << gSet->mLaps << " laps";
            out << ", " << gSet->mMeasure << " s measured at each while the camera turns once round (after "
                << gSet->mSettle
                << " s to settle). Frame times in ms; 1% low = the frame time the slowest 1% of frames reach. heap:"
                   " in use after the place; arena: what the heap has taken from the system; linear: GPU memory"
                   " free.\n\n"
                << header << "\n";
            P3DS_LOGI("benchmark: done");
            P3DS_LOGI("benchmark: %s", header.c_str());
            for (size_t i = 0; i < gResults.size(); ++i)
            {
                const std::string text = line(int(i), gResults[i]);
                out << text << "\n";
                P3DS_LOGI("benchmark: %s", text.c_str());
            }
            // Each place against itself one lap later: memory still in use when the same place is shown again.
            if (gSet->mLaps > 1 && int(gResults.size()) == stageCount())
            {
                float sum = 0, most = 0;
                int worst = 0;
                for (int i = 0; i < gSet->mCount; ++i)
                {
                    const float grew = gResults[i + gSet->mCount * (gSet->mLaps - 1)].mHeapMB - gResults[i].mHeapMB;
                    sum += grew;
                    if (grew > most)
                    {
                        most = grew;
                        worst = i;
                    }
                }
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                    "heap, last lap against the first, place by place: %+.1f MB on average, most %+.1f MB (%s)",
                    double(sum / gSet->mCount), double(most), gSet->mStages[worst].mLabel);
                out << "\n" << buf << "\n";
                P3DS_LOGI("benchmark: %s", buf);
            }
            P3DS_LOGI("benchmark: results in %s; quitting", path.c_str());
            MWBase::Environment::get().getStateManager()->requestQuit();
        }
    }

    void enable(Set set, int firstStage)
    {
        gEnabled = true;
        gSet = &sSets[static_cast<int>(set)];
        gFirstStage = std::clamp(firstStage, 0, stageCount() - 1);
    }

    bool enabled()
    {
        return gEnabled;
    }

    void onFrame()
    {
        if (!gEnabled || gDone)
            return;
        const uint64_t now = p3ds::memprobe::ticks();
        const double dt = gLastTick != 0 ? toSeconds(now - gLastTick) : 0.0;
        gLastTick = now;
        if (MWBase::Environment::get().getStateManager()->getState() != MWBase::StateManager::State_Running)
            return;
        const MWWorld::Ptr player = world().getPlayerPtr();
        if (player.isEmpty() || !player.isInCell())
            return;

        if (gStage < 0)
        {
            gStageTime += dt;
            if (gStageTime < kStartDelay)
                return;
            if (!world().getGodModeState())
                world().toggleGodMode(); // nothing interrupts the run
            gStage = gFirstStage;
            startStage(gStage);
            return;
        }

        Result& result = gResults.back();
        if (gJustTeleported)
        {
            // This frame followed the load.
            result.mLoadMs = dt * 1000.0;
            gJustTeleported = false;
            gStartYaw = player.getRefData().getPosition().rot[2];
            return;
        }
        gStageTime += dt;
        if (!gMeasuring)
        {
            if (gStageTime >= gSet->mSettle)
            {
                gMeasuring = true;
                gStageTime = 0;
            }
            return;
        }
        // The tour (Zeste only): the top screen at a quarter, a half and three quarters of the turn.
        if (gSet->mScreenshots)
            for (int shot = 0; shot < 3; ++shot)
            {
                const double at = gSet->mMeasure * (0.25 + 0.25 * shot);
                if (gStageTime >= at && gStageTime - dt < at)
                {
                    char path[160];
                    std::snprintf(path, sizeof(path), "%s/tour-%02d-%d.bmp", p3ds::logDir(), gStage + 1, shot + 1);
                    Render3DS::requestCapture(path);
                }
            }
        result.mFrameMs.push_back(float(dt * 1000.0));
        const Render3DS::FrameStats& stats = Render3DS::getLastFrameStats();
        result.mDraws += stats.mDraws;
        result.mGpuMs += stats.mGpuMs;
        setYaw(gStartYaw + float(osg::PI * 2.0 * std::min(1.0, gStageTime / gSet->mMeasure)));
        if (gStageTime < gSet->mMeasure)
            return;
        const p3ds::mem::Stats mem = p3ds::mem::queryExact(); // a heap walk: after the measured frames
        result.mHeapMB = p3ds::mem::toMB(mem.heapUsed);
        result.mArenaMB = p3ds::mem::toMB(mem.heapArena);
        result.mLinearFreeMB = p3ds::mem::toMB(mem.linearFree);
        P3DS_LOGI("benchmark: %s", line(gStage, result).c_str());
        if (++gStage < stageCount())
            startStage(gStage);
        else
            finish();
    }
}
