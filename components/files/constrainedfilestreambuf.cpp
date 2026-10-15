#include "constrainedfilestreambuf.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>
#ifdef OPENMW_PLATFORM_3DS
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include <components/debug/debuglog.hpp>
#include <platform3ds/memprobe.hpp> // ticks
#endif

#include <components/misc/omw3dsdetail.hpp>

namespace Files
{
    namespace File = Platform::File;

#ifdef OPENMW_PLATFORM_3DS
    struct SharedArchiveFile
    {
        File::ScopedHandle mHandle;
        std::mutex mMutex; // a read is seek + read
    };

    namespace
    {
        constexpr std::size_t kMinChunk = 32 * 1024;
        constexpr std::size_t kMaxChunk = 256 * 1024;
        // Whole files (content files): Morrowind.esm took 20 s at ~4 MB/s on hardware (test #22, ~50 KB per
        // read on average) while the ref count's reads of up to 256 KB ran at ~9 MB/s.
        constexpr std::size_t kMaxFileChunk = 1024 * 1024;
        // Whole files keep the end of the previous fill in front of the next one: loading a cell, OpenMW reads
        // ahead for moved references and seeks back a few KB (Store<ESM::Cell>::handleMovedCellRefs). When the
        // read-ahead had crossed into a new fill, the seek back dropped it and the reads restarted at 32 KB: ~340
        // times in Morrowind.esm, ~760 small reads.
        constexpr std::size_t kKeep = 64 * 1024;

#ifdef OPENMW3DS_DEBUG
        struct ReadBucket
        {
            std::atomic<uint64_t> mBytes{ 0 };
            std::atomic<uint64_t> mTicks{ 0 };
            std::atomic<uint32_t> mCount{ 0 };
        };
        constexpr std::size_t kBucketLimits[] = { 16 * 1024, 32 * 1024, 64 * 1024, 128 * 1024, 256 * 1024,
            512 * 1024, kMaxFileChunk };
        constexpr const char* kBucketNames[] = { "16K", "32K", "64K", "128K", "256K", "512K", "1M" };
        ReadBucket gReadBuckets[std::size(kBucketLimits)];
        std::atomic<uint32_t> gSeeks{ 0 };
        // Seeks out of the buffer that drop it: backward, and forward past more than kMaxChunk.
        std::atomic<uint32_t> gBackSeeks{ 0 }, gLongJumps{ 0 };
        std::atomic<uint64_t> gBackBytes{ 0 }, gJumpBytes{ 0 };

        void countRead(std::size_t size, std::size_t got, uint64_t ticks)
        {
            std::size_t i = 0;
            while (i + 1 < std::size(kBucketLimits) && size > kBucketLimits[i])
                ++i;
            gReadBuckets[i].mBytes += got;
            gReadBuckets[i].mTicks += ticks;
            ++gReadBuckets[i].mCount;
        }
#endif

        // Archives stay open for the session (a handful of BSAs).
        std::shared_ptr<SharedArchiveFile> openSharedArchive(const std::filesystem::path& fname)
        {
            static std::mutex sMutex;
            static std::map<std::string, std::shared_ptr<SharedArchiveFile>> sFiles;
            const std::lock_guard lock(sMutex);
            std::shared_ptr<SharedArchiveFile>& file = sFiles[fname.string()];
            if (file == nullptr)
            {
                auto opened = std::make_shared<SharedArchiveFile>();
                opened->mHandle = File::open(fname);
                file = std::move(opened);
            }
            return file;
        }
    }

#ifdef OPENMW3DS_DEBUG
    void logReadStats(std::string_view what)
    {
        std::string line;
        char entry[96];
        for (std::size_t i = 0; i < std::size(gReadBuckets); ++i)
        {
            ReadBucket& bucket = gReadBuckets[i];
            const uint32_t count = bucket.mCount.exchange(0);
            const uint64_t bytes = bucket.mBytes.exchange(0);
            const double ms = double(bucket.mTicks.exchange(0)) / 268111.856; // ticks: SYSCLOCK_ARM11 Hz
            if (count == 0)
                continue;
            std::snprintf(entry, sizeof(entry), " | <=%s: %u reads %.1f MB %.0f ms %.1f MB/s", kBucketNames[i],
                unsigned(count), bytes / 1048576.0, ms, ms > 0 ? bytes / 1048.576 / ms : 0.0);
            line += entry;
        }
        Log(Debug::Info) << "3ds reads (" << what << "), " << gSeeks.exchange(0) << " seeks (" << gBackSeeks.exchange(0)
                         << " back by " << gBackBytes.exchange(0) / 1024 << " KB, " << gLongJumps.exchange(0)
                         << " forward past 256 KB by " << gJumpBytes.exchange(0) / 1024 << " KB)" << line;
    }
#endif

    ConstrainedFileStreamBuf::ConstrainedFileStreamBuf(
        const std::filesystem::path& fname, std::size_t start, std::size_t length)
        : mOrigin(start)
    {
        OMW3DS_DETAIL("io.open");
        if (length != std::numeric_limits<std::size_t>::max())
        {
            mShared = openSharedArchive(fname);
            mSize = length;
        }
        else
        {
            mFile = File::open(fname);
            mSize = File::size(mFile) - start;
            File::seek(mFile, start);
        }
        mCapacity = std::max<std::size_t>(1, std::min(mSize, mShared != nullptr ? kMaxChunk : kMaxFileChunk));
        mKeep = mShared == nullptr && mSize > mCapacity ? kKeep : 0;
        mBuffer.reset(new char[mKeep + mCapacity]); // not value-initialized
        mChunk = mSize <= kMaxChunk ? mCapacity : kMinChunk;
        setg(mBuffer.get(), mBuffer.get(), mBuffer.get());
    }

    std::size_t ConstrainedFileStreamBuf::readAt(std::size_t pos, char* data, std::size_t size)
    {
        OMW3DS_DETAIL("io.read");
        std::size_t got;
        uint64_t ticks;
        if (mShared != nullptr)
        {
            const std::lock_guard lock(mShared->mMutex);
            const uint64_t start = p3ds::memprobe::ticks();
            File::seek(mShared->mHandle, mOrigin + pos);
            got = File::read(mShared->mHandle, data, size);
            ticks = p3ds::memprobe::ticks() - start;
#ifdef OPENMW3DS_DEBUG
            ++gSeeks;
#endif
        }
        else
        {
            const uint64_t start = p3ds::memprobe::ticks();
            if (pos != mFilePos)
            {
                File::seek(mFile, mOrigin + pos);
#ifdef OPENMW3DS_DEBUG
                ++gSeeks;
#endif
            }
            got = File::read(mFile, data, size);
            ticks = p3ds::memprobe::ticks() - start;
        }
#ifdef OPENMW3DS_DEBUG
        countRead(size, got, ticks);
#else
        static_cast<void>(ticks);
#endif
        return got;
    }

    std::streambuf::int_type ConstrainedFileStreamBuf::underflow()
    {
        if (gptr() < egptr())
            return traits_type::to_int_type(*gptr());
        const std::size_t pos = position();
        if (pos >= mSize)
            return traits_type::eof();
        // Consumed a whole fill without seeking away: sequential reading, read bigger chunks.
        const bool sequential = pos == mFilePos && egptr() != eback();
        if (sequential)
            mChunk = std::min(mChunk * 2, mCapacity);
        std::size_t keep = 0;
        if (sequential && mKeep > 0)
        {
            keep = std::min<std::size_t>(mKeep, egptr() - eback());
            std::memmove(mBuffer.get(), egptr() - keep, keep);
        }
        const std::size_t toRead = std::min(mChunk, mSize - pos);
        const std::size_t got = readAt(pos, mBuffer.get() + keep, toRead);
        mFilePos = pos + got;
        mBufferPos = pos - keep;
        setg(mBuffer.get(), mBuffer.get() + keep, mBuffer.get() + keep + got);
        if (got == 0)
            return traits_type::eof();
        return traits_type::to_int_type(*gptr());
    }

    std::streambuf::pos_type ConstrainedFileStreamBuf::seekTo(std::size_t newPos)
    {
        if (newPos > mSize)
            return traits_type::eof();
        const std::size_t bufferEnd = mBufferPos + (egptr() - eback());
        if (newPos >= mBufferPos && newPos <= bufferEnd)
        {
            // Inside the buffer (including tellg()): no file access.
            setg(eback(), eback() + (newPos - mBufferPos), egptr());
            return newPos;
        }
        // Outside: drop the buffer, the next underflow() seeks. Short forward skips keep the chunk size
        // (still a sequential scan, e.g. ESMReader skipping subrecords).
        if (newPos < mBufferPos || newPos - bufferEnd > kMaxChunk)
        {
            mChunk = std::min(mCapacity, kMinChunk);
#ifdef OPENMW3DS_DEBUG
            if (newPos < mBufferPos)
            {
                ++gBackSeeks;
                gBackBytes += mBufferPos - newPos;
            }
            else
            {
                ++gLongJumps;
                gJumpBytes += newPos - bufferEnd;
            }
#endif
        }
        mBufferPos = newPos;
        setg(mBuffer.get(), mBuffer.get(), mBuffer.get());
        return newPos;
    }

    std::streambuf::pos_type ConstrainedFileStreamBuf::seekoff(
        off_type offset, std::ios_base::seekdir whence, std::ios_base::openmode mode)
    {
        if ((mode & std::ios_base::out) || !(mode & std::ios_base::in))
            return traits_type::eof();
        off_type base;
        switch (whence)
        {
            case std::ios_base::beg:
                base = 0;
                break;
            case std::ios_base::cur:
                base = static_cast<off_type>(position());
                break;
            case std::ios_base::end:
                base = static_cast<off_type>(mSize);
                break;
            default:
                return traits_type::eof();
        }
        if (base + offset < 0)
            return traits_type::eof();
        return seekTo(static_cast<std::size_t>(base + offset));
    }

    std::streambuf::pos_type ConstrainedFileStreamBuf::seekpos(pos_type pos, std::ios_base::openmode mode)
    {
        if ((mode & std::ios_base::out) || !(mode & std::ios_base::in))
            return traits_type::eof();
        return seekTo(static_cast<std::size_t>(pos));
    }
#else
    ConstrainedFileStreamBuf::ConstrainedFileStreamBuf(
        const std::filesystem::path& fname, std::size_t start, std::size_t length)
        : mOrigin(start)
    {
        mFile = File::open(fname);
        mSize = length != std::numeric_limits<std::size_t>::max() ? length : File::size(mFile) - start;

        if (start != 0)
            File::seek(mFile, start);

        setg(nullptr, nullptr, nullptr);
    }

    std::streambuf::int_type ConstrainedFileStreamBuf::underflow()
    {
        if (gptr() == egptr())
        {
            const std::size_t toRead = std::min((mOrigin + mSize) - (File::tell(mFile)), sizeof(mBuffer));
            // Read in the next chunk of data, and set the read pointers on success
            // Failure will throw exception.
            const std::size_t got = File::read(mFile, mBuffer, toRead);
            setg(mBuffer, mBuffer, mBuffer + got);
        }
        if (gptr() == egptr())
            return traits_type::eof();

        return traits_type::to_int_type(*gptr());
    }

    std::streambuf::pos_type ConstrainedFileStreamBuf::seekoff(
        off_type offset, std::ios_base::seekdir whence, std::ios_base::openmode mode)
    {
        if ((mode & std::ios_base::out) || !(mode & std::ios_base::in))
            return traits_type::eof();

        // new file position, relative to mOrigin
        size_t newPos;
        switch (whence)
        {
            case std::ios_base::beg:
                newPos = offset;
                break;
            case std::ios_base::cur:
                newPos = (File::tell(mFile) - mOrigin - (egptr() - gptr())) + offset;
                break;
            case std::ios_base::end:
                newPos = mSize + offset;
                break;
            default:
                return traits_type::eof();
        }

        if (newPos > mSize)
            return traits_type::eof();

        File::seek(mFile, mOrigin + newPos);

        // Clear read pointers so underflow() gets called on the next read attempt.
        setg(nullptr, nullptr, nullptr);

        return newPos;
    }

    std::streambuf::pos_type ConstrainedFileStreamBuf::seekpos(pos_type pos, std::ios_base::openmode mode)
    {
        if ((mode & std::ios_base::out) || !(mode & std::ios_base::in))
            return traits_type::eof();

        if (static_cast<std::size_t>(pos) > mSize)
            return traits_type::eof();

        File::seek(mFile, mOrigin + pos);

        // Clear read pointers so underflow() gets called on the next read attempt.
        setg(nullptr, nullptr, nullptr);
        return pos;
    }
#endif
}
