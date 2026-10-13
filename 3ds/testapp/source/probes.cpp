#include "probes.hpp"

#include <platform3ds/log.hpp>
#include <platform3ds/memstats.hpp>
#include <platform3ds/platform.hpp>
#include <platform3ds/timing.hpp>

#include <3ds.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

namespace
{
    FILE* gOut = nullptr;
    void (*gProgress)() = nullptr;
    int gFailures = 0;

    void report(bool pass, const char* name, const std::string& detail)
    {
        if (!pass)
            ++gFailures;
        const auto level = pass ? p3ds::log::Level::Info : p3ds::log::Level::Error;
        p3ds::log::writef(level, "PROBE %-14s %s %s", name, pass ? "PASS" : "FAIL", detail.c_str());
        if (gOut != nullptr)
        {
            fprintf(gOut, "%-16s %s  %s\n", name, pass ? "PASS" : "FAIL", detail.c_str());
            fflush(gOut);
        }
        if (gProgress != nullptr)
            gProgress();
    }

    template <class F>
    void guarded(const char* name, F&& f)
    {
        try
        {
            f();
        }
        catch (const std::exception& e)
        {
            report(false, name, std::string("exception: ") + e.what());
        }
        catch (...)
        {
            report(false, name, "unknown exception");
        }
    }

    // ---- CPU ----
    __attribute__((noipa)) uint32_t intWork(uint32_t iters)
    {
        uint32_t a = 1, b = 2;
        for (uint32_t i = 0; i < iters; ++i)
        {
            a = a * 1664525u + 1013904223u;
            b ^= a >> 7;
            b += (a & 0xFF) * 3;
        }
        return a ^ b;
    }

    template <class T>
    __attribute__((noipa)) T fpWork(uint32_t iters)
    {
        T x = T(1.0001), acc = T(0);
        for (uint32_t i = 0; i < iters; ++i)
        {
            acc = acc * T(0.9999) + x;
            x = x * T(1.0000001) + T(0.5) / (x + T(1));
        }
        return acc;
    }

    double timeMs(auto&& fn)
    {
        const uint64_t t0 = p3ds::ticks();
        fn();
        return p3ds::ticksToMs(p3ds::ticks() - t0);
    }

    void probeCpu()
    {
        volatile uint32_t sink = 0;
        volatile double dsink = 0;
        const double fast = timeMs([&] { sink = intWork(2000000); });
        const double f32 = timeMs([&] { dsink = fpWork<float>(500000); });
        const double f64 = timeMs([&] { dsink = fpWork<double>(500000); });
        std::string detail = std::format("int 2M: {:.1f} ms, float 500k: {:.1f} ms, double 500k: {:.1f} ms (double/float x{:.2f})",
            fast, f32, f64, f64 / f32);

        if (p3ds::info().isNew3DS)
        {
            osSetSpeedupEnable(false);
            svcSleepThread(10 * 1000 * 1000);
            const double slow = timeMs([&] { sink = intWork(2000000); });
            osSetSpeedupEnable(p3ds::info().speedupEnabled);
            svcSleepThread(10 * 1000 * 1000);
            detail += std::format("; int at 268MHz: {:.1f} ms (speedup x{:.2f})", slow, slow / fast);
        }
        report(true, "cpu", detail);
    }

    // ---- memory mode ----
    void probeMemory()
    {
        const p3ds::mem::Stats s = p3ds::mem::query();
        // A .cia must get the mode it asked for. A .3dsx just inherits whatever the launcher has, so
        // any size is acceptable; the budget guard caps emulator over-provisioning either way.
        const bool modeOk = !p3ds::info().launchedAsCia || s.regionTotal >= p3ds::mem::expectedRegion();
        report(modeOk, "memory-mode",
            std::format("region {:.1f} MB (hardware expects {} MB for {}), image {:.2f} MB, heap {:.1f} MB, linear {:.1f} MB, vram free {:.2f} MB",
                p3ds::mem::toMB(s.regionTotal), p3ds::mem::expectedRegion() >> 20, p3ds::info().launchedAsCia ? ".cia" : ".3dsx",
                p3ds::mem::toMB(s.imageSize), p3ds::mem::toMB(s.heapTotal), p3ds::mem::toMB(s.linearTotal),
                p3ds::mem::toMB(s.vramFree)));
        if (p3ds::mem::overProvision(s) > 0)
            report(true, "emulator-mem", std::format("region over-provisioned by {:.1f} MB vs hardware (emulator?)",
                                             p3ds::mem::toMB(p3ds::mem::overProvision(s))));
    }

    // ---- C++ runtime features ----
    struct Base
    {
        virtual ~Base() = default;
    };
    struct Derived : Base
    {
        int v = 7;
    };

    thread_local int tlsValue = 1;

    void probeCxx()
    {
        guarded("exceptions", [] {
            bool caught = false;
            try
            {
                throw std::runtime_error("probe");
            }
            catch (const std::runtime_error& e)
            {
                caught = std::strcmp(e.what(), "probe") == 0;
            }
            report(caught, "exceptions", "throw/catch std::runtime_error");
        });

        guarded("rtti", [] {
            std::unique_ptr<Base> b = std::make_unique<Derived>();
            Derived* d = dynamic_cast<Derived*>(b.get());
            const bool ok = d != nullptr && d->v == 7 && typeid(*b) == typeid(Derived);
            report(ok, "rtti", std::string("dynamic_cast + typeid: ") + typeid(*b).name());
        });

        guarded("std-format", [] {
            const std::string s = std::format("{:>6.2f}|{:#x}|{}", 3.14159, 255, std::string_view("ok"));
            report(s == "  3.14|0xff|ok", "std-format", s);
        });

        guarded("thread-local", [] {
            tlsValue = 42;
            int seen = 0;
            std::thread t([&] { seen = tlsValue; tlsValue = 5; });
            t.join();
            report(seen == 1 && tlsValue == 42, "thread-local", std::format("main=42 thread saw {} (expect 1)", seen));
        });

        guarded("std-thread", [] {
            std::mutex m;
            std::condition_variable cv;
            int counter = 0;
            bool go = false;
            std::vector<std::thread> threads;
            for (int i = 0; i < 3; ++i)
                threads.emplace_back([&] {
                    std::unique_lock lock(m);
                    cv.wait(lock, [&] { return go; });
                    for (int k = 0; k < 1000; ++k)
                        ++counter;
                });
            {
                std::lock_guard lock(m);
                go = true;
            }
            cv.notify_all();
            for (auto& t : threads)
                t.join();
            report(counter == 3000, "std-thread", std::format("3 threads + mutex + condvar, counter {}", counter));
        });

        guarded("chrono", [] {
            const auto a = std::chrono::steady_clock::now();
            svcSleepThread(20 * 1000 * 1000);
            const auto b = std::chrono::steady_clock::now();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
            report(ms >= 15 && ms < 200, "chrono", std::format("steady_clock 20 ms sleep measured {} ms", ms));
        });
    }

    // ---- cores ----
    struct CoreResult
    {
        double ms = -1.0;
        int core = -1;
    };

    void coreBench(void* arg)
    {
        auto* r = static_cast<CoreResult*>(arg);
        volatile uint32_t sink = 0;
        r->ms = timeMs([&] { sink = intWork(2000000); });
        r->core = svcGetProcessorID();
    }

    void probeCores()
    {
        for (int core : { 1, 2 })
        {
            CoreResult res;
            Thread t = threadCreate(coreBench, &res, 16 * 1024, 0x30, core, false);
            if (t == nullptr)
            {
                report(core == 2 && !p3ds::info().isNew3DS, std::format("core{}", core).c_str(),
                    std::format("threadCreate on core {} failed", core));
                continue;
            }
            threadJoin(t, U64_MAX);
            threadFree(t);
            // svcGetProcessorID is unreliable under emulation; the timing shows the core's time slice.
            report(res.ms > 0, std::format("core{}", core).c_str(),
                std::format("thread created on core {}, int 2M: {:.1f} ms (svcGetProcessorID={})", core, res.ms, res.core));
        }
    }

    // ---- filesystem ----
    void probeFilesystem()
    {
        guarded("std-filesystem", [] {
            namespace fs = std::filesystem;
            const fs::path root = "sdmc:/openmw/probe_fs";
            fs::remove_all(root);
            fs::create_directories(root / "a" / "b");
            {
                std::ofstream f(root / "a" / "b" / "hello.txt", std::ios::binary);
                f << "hello 3ds";
            }
            const auto size = fs::file_size(root / "a" / "b" / "hello.txt");
            int entries = 0;
            for (const auto& e : fs::recursive_directory_iterator(root))
            {
                (void)e;
                ++entries;
            }
            const bool exists = fs::exists(root / "a");
            const bool isDir = fs::is_directory(root / "a" / "b");
            const std::string ext = (root / "x.esm").extension().string();
            const auto removed = fs::remove_all(root);
            const bool ok = size == 9 && entries == 3 && exists && isDir && ext == ".esm" && removed == 4;
            report(ok, "std-filesystem",
                std::format("size {} entries {} exists {} dir {} ext {} removed {}", size, entries, exists, isDir, ext, removed));
        });
    }

    // ---- SD card throughput ----
    void probeSd()
    {
        guarded("sd-bench", [] {
            const char* path = "sdmc:/openmw/sdbench.bin";
            const size_t total = 8u * 1024 * 1024;
            std::vector<char> chunk(1024 * 1024);
            for (size_t i = 0; i < chunk.size(); ++i)
                chunk[i] = char(i * 31);

            FILE* f = fopen(path, "wb");
            if (f == nullptr)
                throw std::runtime_error("cannot create bench file");
            const double writeMs = timeMs([&] {
                for (size_t done = 0; done < total; done += chunk.size())
                    fwrite(chunk.data(), 1, chunk.size(), f);
                fclose(f);
            });
            const auto mbps = [&](size_t bytes, double ms) { return ms > 0 ? (double(bytes) / (1024.0 * 1024.0)) / (ms / 1000.0) : 0.0; };
            std::string detail = std::format("write {:.2f} MB/s", mbps(total, writeMs));

            for (size_t bs : { size_t(4096), size_t(65536), size_t(262144), size_t(1048576) })
            {
                FILE* r = fopen(path, "rb");
                setvbuf(r, nullptr, _IONBF, 0);
                const double ms = timeMs([&] {
                    while (fread(chunk.data(), 1, bs, r) == bs)
                    {
                    }
                });
                fclose(r);
                detail += std::format(", read {}K {:.2f} MB/s", bs / 1024, mbps(total, ms));
            }

            FILE* r = fopen(path, "rb");
            setvbuf(r, nullptr, _IONBF, 0);
            uint32_t seed = 12345;
            const int seeks = 256;
            const double rms = timeMs([&] {
                for (int i = 0; i < seeks; ++i)
                {
                    seed = seed * 1664525u + 1013904223u;
                    fseek(r, long((seed % (total / 4096)) * 4096), SEEK_SET);
                    fread(chunk.data(), 1, 4096, r);
                }
            });
            fclose(r);
            remove(path);
            detail += std::format(", random 4K {:.2f} ms/read", rms / seeks);
            report(true, "sd-bench", detail);
        });
    }
}

int runProbes(bool includeSdBenchmark, void (*onProgress)())
{
    gFailures = 0;
    gProgress = onProgress;
    gOut = fopen("sdmc:/openmw/probe.txt", "w");
    P3DS_LOGI("---- probes start ----");

    char sysver[64] = "";
    osGetSystemVersionDataString(nullptr, nullptr, sysver, sizeof(sysver));
    report(true, "system",
        std::format("{} | kernel {}.{} | {} | {}", p3ds::info().isNew3DS ? "New 3DS" : "Old 3DS",
            GET_VERSION_MAJOR(osGetKernelVersion()), GET_VERSION_MINOR(osGetKernelVersion()),
            p3ds::info().launchedAsCia ? "cia" : "3dsx", sysver[0] ? sysver : "sysver n/a"));

    // Each step is announced first, so a freeze leaves the culprit as the last log line.
    P3DS_LOGI("probe step: memory");
    probeMemory();
    P3DS_LOGI("probe step: cpu");
    probeCpu();
    P3DS_LOGI("probe step: c++ runtime");
    probeCxx();
    P3DS_LOGI("probe step: cores");
    probeCores();
    P3DS_LOGI("probe step: filesystem");
    probeFilesystem();
    if (includeSdBenchmark)
    {
        P3DS_LOGI("probe step: sd benchmark (8 MB)");
        probeSd();
    }

    P3DS_LOGI("---- probes done: %d failure(s) ----", gFailures);
    if (gOut != nullptr)
    {
        fprintf(gOut, "\nfailures: %d\n", gFailures);
        fclose(gOut);
        gOut = nullptr;
    }
    return gFailures;
}
