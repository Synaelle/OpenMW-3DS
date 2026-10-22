#include <platform3ds/crash.hpp>
#include <platform3ds/screenshot.hpp>
#include <platform3ds/textfb.hpp>

#include <3ds.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace p3ds::screenshot
{
    namespace
    {
        void put16(u8* p, uint16_t v)
        {
            p[0] = u8(v);
            p[1] = u8(v >> 8);
        }
        void put32(u8* p, uint32_t v)
        {
            put16(p, uint16_t(v));
            put16(p + 2, uint16_t(v >> 16));
        }
    }

    bool saveRotatedFramebuffer(const char* path, const void* data, int w, int h, Format fmt)
    {
        const int rowBytes = (w * 3 + 3) & ~3;
        const uint32_t imageBytes = uint32_t(rowBytes * h);
        u8 header[54] = {};
        header[0] = 'B';
        header[1] = 'M';
        put32(header + 2, 54 + imageBytes);
        put32(header + 10, 54);
        put32(header + 14, 40);
        put32(header + 18, uint32_t(w));
        put32(header + 22, uint32_t(h)); // positive: bottom-up rows
        put16(header + 26, 1);
        put16(header + 28, 24);
        put32(header + 34, imageBytes);

        // The whole file is built in memory and written at once: one small write per row took 3-7 s on the SD
        // card (hardware test #21).
        u8* file = static_cast<u8*>(calloc(1, sizeof(header) + imageBytes));
        if (file == nullptr)
            return false;
        memcpy(file, header, sizeof(header));
        // BMP row 0 is the bottom of the image. In the rotated framebuffer, screen pixel (x, y)
        // (y down) is at x*h + (h-1-y), so the bottom screen row y=h-1 is index x*h + 0.
        for (int by = 0; by < h; ++by)
        {
            const int y = h - 1 - by;
            u8* line = file + sizeof(header) + size_t(by) * size_t(rowBytes);
            for (int x = 0; x < w; ++x)
            {
                const int idx = x * h + (h - 1 - y);
                u8* dst = line + x * 3;
                if (fmt == Format::BGR8)
                {
                    const u8* src = static_cast<const u8*>(data) + idx * 3;
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                }
                else
                {
                    const uint16_t px = static_cast<const uint16_t*>(data)[idx];
                    dst[0] = u8((px & 0x1F) << 3);
                    dst[1] = u8(((px >> 5) & 0x3F) << 2);
                    dst[2] = u8(((px >> 11) & 0x1F) << 3);
                }
            }
        }
        FILE* f = fopen(path, "wb");
        bool ok = f != nullptr;
        if (ok)
        {
            ok = fwrite(file, 1, sizeof(header) + imageBytes, f) == sizeof(header) + imageBytes;
            ok = fclose(f) == 0 && ok;
        }
        free(file);
        return ok;
    }

    bool saveBottomScreen(const char* path)
    {
        // When the GPU draws it (M8's GUI), the CPU's cached copy is stale: drop it first. (Not when the CPU draws
        // it: that would drop its own writes still in the cache.)
        if (textfb::gpuOwned())
            GSPGPU_InvalidateDataCache(textfb::framebuffer(), textfb::kWidth * textfb::kHeight * sizeof(uint16_t));
        return saveRotatedFramebuffer(path, textfb::framebuffer(), textfb::kWidth, textfb::kHeight, Format::RGB565);
    }

    namespace
    {
        // One save at a time: the file built in memory, written by its thread, then freed.
        struct AsyncSave
        {
            char mPath[96];
            u8* mFile;
            size_t mSize;
        };
        std::atomic<bool> gSaving{ false };
        std::atomic<int> gResult{ 0 };
        Thread gSaveThread = nullptr;

        void saveThread(void* arg)
        {
            crash::installForCurrentThread(); // a crash here writes a report too (M20)
            auto* save = static_cast<AsyncSave*>(arg);
            FILE* f = fopen(save->mPath, "wb");
            bool ok = f != nullptr;
            if (ok)
            {
                ok = fwrite(save->mFile, 1, save->mSize, f) == save->mSize;
                ok = fclose(f) == 0 && ok;
            }
            free(save->mFile);
            delete save;
            gResult = ok ? 1 : -1;
            gSaving = false;
        }
    }

    bool saveBothScreensAsync(const char* path, const uint8_t* top)
    {
        if (gSaving)
            return false;
        if (gSaveThread != nullptr)
        {
            threadJoin(gSaveThread, U64_MAX); // finished (gSaving is false): only its resources are left
            threadFree(gSaveThread);
            gSaveThread = nullptr;
        }
        constexpr int kW = 400, kH = 480, kTopH = 240, kBottomX = 40, kBottomW = 320;
        constexpr int rowBytes = kW * 3; // 1200: already a multiple of 4
        constexpr uint32_t imageBytes = uint32_t(rowBytes) * kH;
        auto* save = new AsyncSave;
        std::snprintf(save->mPath, sizeof(save->mPath), "%s", path);
        save->mSize = 54 + imageBytes;
        save->mFile = static_cast<u8*>(calloc(1, save->mSize)); // black: the bottom screen's sides
        if (save->mFile == nullptr)
        {
            delete save;
            return false;
        }
        u8* header = save->mFile;
        header[0] = 'B';
        header[1] = 'M';
        put32(header + 2, uint32_t(save->mSize));
        put32(header + 10, 54);
        put32(header + 14, 40);
        put32(header + 18, kW);
        put32(header + 22, kH); // positive: bottom-up rows
        put16(header + 26, 1);
        put16(header + 28, 24);
        put32(header + 34, imageBytes);

        if (textfb::gpuOwned())
            GSPGPU_InvalidateDataCache(textfb::framebuffer(), textfb::kWidth * textfb::kHeight * sizeof(uint16_t));
        const auto* bottom = static_cast<const uint16_t*>(textfb::framebuffer());
        // Image row y (from the top) is BMP row kH-1-y. Rotated framebuffers: screen pixel (x, y) at x*240 + 239-y.
        for (int y = 0; y < kH; ++y)
        {
            u8* line = save->mFile + 54 + size_t(kH - 1 - y) * rowBytes;
            if (y < kTopH)
            {
                for (int x = 0; x < kW; ++x)
                    memcpy(line + x * 3, top + (x * 240 + (239 - y)) * 3, 3);
            }
            else
            {
                const int by = y - kTopH;
                for (int x = 0; x < kBottomW; ++x)
                {
                    const uint16_t px = bottom[x * 240 + (239 - by)];
                    u8* dst = line + (kBottomX + x) * 3;
                    dst[0] = u8((px & 0x1F) << 3);
                    dst[1] = u8(((px >> 5) & 0x3F) << 2);
                    dst[2] = u8(((px >> 11) & 0x1F) << 3);
                }
            }
        }

        // Above the main thread's priority, on the system core when there is one, like the log's flusher: it
        // mostly waits for the SD card.
        s32 mainPrio = 0x30;
        svcGetThreadPriority(&mainPrio, CUR_THREAD_HANDLE);
        gSaving = true;
        gSaveThread = threadCreate(saveThread, save, 8 * 1024, mainPrio - 1, 1, false);
        if (gSaveThread == nullptr)
            gSaveThread = threadCreate(saveThread, save, 8 * 1024, mainPrio - 1, 0, false);
        if (gSaveThread == nullptr)
        {
            gSaving = false;
            free(save->mFile);
            delete save;
            return false;
        }
        return true;
    }

    int takeAsyncResult()
    {
        return gResult.exchange(0);
    }
}
