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
        uint32_t heapUsed = 0; // bytes in use by malloc
        uint32_t heapArena = 0; // heap already obtained by malloc via sbrk
        uint32_t linearTotal = 0; // GPU-visible linear heap
        uint32_t linearFree = 0;
        uint32_t vramFree = 0; // of 6 MB
    };

    // Cheap: heapUsed/heapArena are the values from the last queryExact() (the watchdog refreshes them
    // every 5 s). Safe to call every frame.
    Stats query();
    // Also walks the malloc heap (mallinfo): ~10+ ms on hardware with a large fragmented heap, holding
    // the malloc lock. Watchdog log, crash reports, startup only.
    Stats queryExact();

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
