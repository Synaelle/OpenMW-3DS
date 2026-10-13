// M0 platform test app: citro3d rendering + GPU micro-benchmarks on the top screen, the debug
// bottom screen, logging, crash logger and hardware/toolchain probes.
//
// Controls: A = next benchmark scene, SELECT = toggle debug screen, START = exit,
//           L+R+X = data abort, L+R+Y = uncaught C++ exception, L+R+B = abort().
//
// Automation: if sdmc:/openmw/autorun.txt exists, its whitespace-separated commands drive the run:
//   probes        run the probe suite (incl. SD benchmark)
//   bench         cycle all GPU scenes (90 frames each), logging averages
//   screens       also save BMP screenshots (emulator runs; slow on real SD cards)
//   crash:data | crash:throw | crash:abort | crash:hang   trigger that failure after the other steps
// The app then writes sdmc:/openmw/autorun.done and exits.

#include "probes.hpp"
#include "renderer.hpp"

#include <platform3ds/crash.hpp>
#include <platform3ds/debugscreen.hpp>
#include <platform3ds/log.hpp>
#include <platform3ds/platform.hpp>
#include <platform3ds/screenshot.hpp>
#include <platform3ds/timing.hpp>

#include <3ds.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace
{
    // A few distinct frames so the crash backtrace has something to show.
    __attribute__((noinline)) void crashWriteNull(int depth)
    {
        if (depth > 0)
        {
            crashWriteNull(depth - 1);
            asm volatile("" ::: "memory");
            return;
        }
        volatile int* p = nullptr;
        *p = 0xDEAD;
    }

    __attribute__((noinline)) void crashThrow()
    {
        throw std::runtime_error("deliberate test exception");
    }

    void triggerCrash(const char* kind)
    {
        P3DS_LOGW("triggering test crash: %s", kind);
        p3ds::log::flush();
        if (strcmp(kind, "data") == 0)
            crashWriteNull(3);
        else if (strcmp(kind, "throw") == 0)
            crashThrow();
        else if (strcmp(kind, "abort") == 0)
            abort();
        else if (strcmp(kind, "hang") == 0)
        {
            p3ds::crash::kick("deliberate hang test");
            for (;;)
                asm volatile("" ::: "memory"); // spin: only the watchdog can end this
        }
    }

    struct Autorun
    {
        bool enabled = false;
        bool probes = false;
        bool bench = false;
        bool screens = false; // screenshots cost ~1.5 s each on real SD cards (file creation)
        char crash[16] = "";
    };

    Autorun parseAutorun()
    {
        Autorun a;
        char buf[256];
        if (!p3ds::readSmallFile(p3ds::kAutorunPath, buf, sizeof(buf)))
            return a;
        a.enabled = true;
        a.probes = strstr(buf, "probes") != nullptr;
        a.bench = strstr(buf, "bench") != nullptr;
        a.screens = strstr(buf, "screens") != nullptr;
        if (const char* c = strstr(buf, "crash:"))
            sscanf(c + 6, "%15s", a.crash);
        return a;
    }

    const char* gContext = "testapp: startup";
}

int main(int argc, char** argv)
{
    gfxInitDefault();
    p3ds::InitOptions opts;
    opts.ciaUniqueId = 0xF6D30; // must match testapp/CMakeLists.txt UNIQUE_ID
    const p3ds::PlatformInfo pi = p3ds::init(opts);
    p3ds::crash::setContextString(&gContext);
    // Test app: every log line goes to the SD card immediately, so a hard freeze still leaves a trail.
    p3ds::log::setSyncMode(true);
    P3DS_LOGI("OpenMW3DS testapp M0");

    const Autorun autorun = parseAutorun();

    // Any 15 s without progress => hang report + exit to HOME instead of a frozen console.
    p3ds::crash::startWatchdog(15, p3ds::info().syscoreLimit > 0 ? 1 : 0);
    p3ds::crash::kick("renderer init");

    p3ds::DebugBottomScreen bottom;
    p3ds::IBottomScreen& screen = bottom; // the game loop only knows the interface
    screen.init();
    static p3ds::DebugBottomScreen* sBottom = &bottom;
    const auto progress = [] {
        p3ds::GameSnapshot s;
        s.cellName = "(probes running)";
        sBottom->update(s);
        sBottom->forceRedraw();
    };
    progress();

    TestRenderer renderer;
    if (!renderer.init())
        p3ds::crash::fatal("renderer init failed");

    if (autorun.probes)
    {
        gContext = "testapp: probes";
        runProbes(true, progress);
    }
    else if (!autorun.enabled)
        runProbes(false, progress); // quick probes without the SD benchmark

    p3ds::FrameTimer timer;
    p3ds::FrameStats stats;
    char status[48] = "";
    uint64_t last = p3ds::ticks();

    // Benchmark bookkeeping (autorun).
    const int kFramesPerScene = 90;
    int benchScene = 0, benchFrame = 0;
    double sumFrame = 0, sumCpu = 0, sumGpu = 0;
    bool done = !autorun.bench;

    gContext = "testapp: main loop";
    while (aptMainLoop())
    {
        hidScanInput();
        const u32 down = hidKeysDown();
        const u32 held = hidKeysHeld();
        if (down & KEY_START)
            break;
        if (down & KEY_SELECT)
            screen.setVisible(!screen.isVisible());
        if ((down & KEY_A) && !autorun.bench)
        {
            renderer.setScene(renderer.scene() + 1);
            P3DS_LOGI("scene: %s", TestRenderer::sceneName(renderer.scene()));
        }
        if ((held & KEY_L) && (held & KEY_R))
        {
            if (down & KEY_X)
                triggerCrash("data");
            if (down & KEY_Y)
                triggerCrash("throw");
            if (down & KEY_B)
                triggerCrash("abort");
        }
        if (down & KEY_TOUCH)
        {
            touchPosition tp;
            hidTouchRead(&tp);
            screen.onTouch({ tp.px, tp.py, true });
        }

        p3ds::crash::kick(TestRenderer::sceneName(renderer.scene()));
        renderer.renderFrame(stats);

        const uint64_t now = p3ds::ticks();
        const float frameMs = float(p3ds::ticksToMs(now - last));
        last = now;
        timer.addFrame(frameMs);
        timer.fill(stats);
        ++stats.frameIndex;

        if (autorun.bench && !done)
        {
            if (benchFrame == 0)
            {
                renderer.setScene(benchScene);
                P3DS_LOGI("bench scene start: %s", TestRenderer::sceneName(benchScene));
            }
            if (benchFrame >= 10) // skip warm-up frames
            {
                sumFrame += frameMs;
                sumCpu += stats.cpuMs;
                sumGpu += stats.gpuMs;
            }
            if (++benchFrame == kFramesPerScene)
            {
                const double n = kFramesPerScene - 10;
                P3DS_LOGI("BENCH %-12s draws %4lu tris %6lu | frame %6.2f ms  cpu %6.2f ms  gpu %6.2f ms",
                    TestRenderer::sceneName(benchScene), (unsigned long)stats.drawCalls, (unsigned long)stats.triangles,
                    sumFrame / n, sumCpu / n, sumGpu / n);
                char path[64];
                snprintf(path, sizeof(path), "sdmc:/openmw/shot_scene%d_top.bmp", benchScene);
                if (autorun.screens)
                    renderer.captureTop(path);
                sumFrame = sumCpu = sumGpu = 0;
                benchFrame = 0;
                if (++benchScene == TestRenderer::SceneCount)
                    done = true;
            }
        }

        snprintf(status, sizeof(status), "%s", TestRenderer::sceneName(renderer.scene()));
        p3ds::GameSnapshot snap;
        snap.frame = &stats;
        snap.cellName = "(testapp: no world)";
        snap.status = status;
        screen.update(snap);
        screen.render();

        if (autorun.enabled && done)
        {
            // Make sure the bottom screen shows the final state before capturing it.
            screen.render();
            svcSleepThread(300 * 1000 * 1000);
            bottom.setVisible(false);
            bottom.setVisible(true);
            screen.update(snap);
            screen.render();
            if (autorun.screens)
                p3ds::screenshot::saveBottomScreen("sdmc:/openmw/shot_bottom.bmp");
            if (autorun.crash[0] != '\0')
                triggerCrash(autorun.crash);
            break;
        }
    }

    P3DS_LOGI("exiting main loop after %lu frames", (unsigned long)stats.frameIndex);
    renderer.shutdown();
    screen.shutdown();
    if (autorun.enabled)
        p3ds::writeSmallFile(p3ds::kAutorunDonePath, "done\n");
    p3ds::shutdown();
    gfxExit();
    (void)pi;
    (void)argc;
    (void)argv;
    return 0;
}
