#pragma once

#include <cstdint>

namespace p3ds::mem
{
    struct Stats
    {
        // Process commit limit/usage from the kernel resource limit: this is what actually bounds us
        // (64/96/124/178 MB depending on mode). Emulators can misreport the shared-page region size,
        // so the resource limit is authoritative.
        uint32_t regionTotal = 0;
        uint32_t regionUsed = 0; // code + heaps (libctru commits both heaps up front) + stacks
        uint32_t imageSize = 0; // executable image: text + rodata + data + bss
        uint32_t heapTotal = 0; // app heap (malloc/new)
        uint32_t heapUsed = 0; // bytes in use by malloc, as of the last heap walk (queryExact)
        uint32_t heapArena = 0; // heap already obtained by malloc via sbrk (live)
        uint32_t heapUsedAgeMs = 0; // how old heapUsed is
        uint32_t linearTotal = 0; // GPU-visible linear heap
        uint32_t linearFree = 0;
        uint32_t vramFree = 0; // of 6 MB
    };

    // Cheap: heapArena is live; heapUsed is from the last queryExact(). Safe to call every frame.
    Stats query();
    // Also walks the malloc heap (mallinfo), holding the malloc lock: ~200 ms on hardware with OpenMW's heap
    // from the watchdog's core (hardware test #15), a visible freeze. Loading, crash reports, startup only.
    Stats queryExact();
    // Heap arena when queryExact() last ran.
    uint32_t arenaAtLastWalk();

    // Heap budget the hardware would give us. Emulators can over-provision the region; in that case
    // the budget is reduced by the excess so emulator runs fail like the console would.
    uint32_t heapBudget(const Stats& s);

    // Returns false (and logs once per transition) when heap use exceeds the hardware budget.
    bool checkBudget(const Stats& s);

    // Region size real hardware gives this build (detects .3dsx vs .cia and Old vs New 3DS).
    uint32_t expectedRegion();

    // Bytes of slack the emulator gave beyond expectedRegion() (0 on hardware).
    uint32_t overProvision(const Stats& s);

    inline float toMB(uint32_t bytes)
    {
        return float(bytes) / (1024.f * 1024.f);
    }
}
