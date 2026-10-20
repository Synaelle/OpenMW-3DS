#include <platform3ds/loadscreen.hpp>
#include <platform3ds/menuart.hpp>
#include <platform3ds/textfb.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace p3ds::loadscreen
{
    namespace
    {
        namespace tf = textfb;
        namespace art = menuart;

        // The start menu's layout (main3ds.cpp): the title, a line, the stages where the menu's items are - done
        // ones plain, the current one marked like a chosen item, the next ones faint - and the build at the bottom.
        constexpr int kTitleY = 14;
        constexpr int kHeaderLineY = 46;
        constexpr int kStagesY = 58;
        constexpr int kStageHeight = 25;
        constexpr int kStageX = menuart::kMargin + 12;
        constexpr int kFooterLineY = tf::kHeight - 30;
        constexpr int kFooterY = tf::kHeight - 24;

        std::atomic<bool> gActive{ false };
        constexpr const char* kStages[] = {
            "Starting the engine",
            "Reading the game data",
            "Building the world",
            "Preparing the interface",
            "Almost ready",
        };
        constexpr int kStageCount = int(sizeof(kStages) / sizeof(kStages[0]));

        std::atomic<int> gStage{ 0 };
        int gDrawnStage = -1;
        unsigned gTick = 0;

        // The current stage has one to three dots after it, which move on at each watchdog tick: still working. The
        // other lines are drawn again only when the stage changes (the screen shows this framebuffer as it is drawn).
        void drawStages()
        {
            const int current = gStage.load();
            for (int i = 0; i < kStageCount; ++i)
            {
                if (current == gDrawnStage && i != current)
                    continue;
                const int y = kStagesY + i * kStageHeight;
                art::restore(art::kMargin, y, tf::kWidth - 2 * art::kMargin, art::lineHeight(art::Font::Text));
                if (i == current)
                {
                    char line[64];
                    std::snprintf(line, sizeof(line), "%s%s", kStages[i], &"..."[2 - gTick % 3]);
                    art::mark(y, art::lineHeight(art::Font::Text));
                    art::text(kStageX, y, line, art::Font::Text, art::colour::selected);
                }
                else
                    art::text(kStageX, y, kStages[i], art::Font::Text,
                        i < current ? art::colour::text : art::colour::disabled);
            }
            gDrawnStage = current;
        }
    }

    void start(const char* build)
    {
        if (tf::gpuOwned())
            return;
        art::topScreen();
        art::background();
        art::text(art::kMargin, kTitleY, "OpenMW 3DS", art::Font::Title, art::colour::title);
        art::separator(kHeaderLineY);
        drawStages();
        art::separator(kFooterLineY);
        if (build != nullptr)
            art::text(art::kMargin, kFooterY, build, art::Font::Small, art::colour::dim);
        tf::present();
        gActive = true;
    }

    bool active()
    {
        return gActive && !tf::gpuOwned();
    }

    void noteStage(const char* label)
    {
        if (label == nullptr)
            return;
        auto starts = [label](const char* prefix) { return std::strncmp(label, prefix, std::strlen(prefix)) == 0; };
        // The labels mark the end of a step (engine.cpp, worldimp.cpp): each one starts the next stage.
        int stage = -1;
        if (starts("before loadData"))
            stage = 1;
        else if (starts("after loadData"))
            stage = 2;
        else if (starts("World::init") || starts("setupPlayer"))
            stage = 3;
        else if (starts("Lua initPostLoad") || starts("before newGame"))
            stage = 4;
        // Only forwards: a later label can belong to an earlier stage's work.
        if (stage > gStage.load())
            gStage = stage;
    }

    void tick()
    {
        if (!active())
            return;
        ++gTick;
        drawStages();
        tf::present();
    }
}
