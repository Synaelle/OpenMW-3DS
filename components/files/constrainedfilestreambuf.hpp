#ifndef OPENMW_CONSTRAINEDFILESTREAMBUF_H
#define OPENMW_CONSTRAINEDFILESTREAMBUF_H

#include <filesystem>
#include <memory>
#include <streambuf>
#ifdef OPENMW_PLATFORM_3DS
#include <string_view>
#endif

#include <components/platform/file.hpp>

namespace Files
{
#ifdef OPENMW_PLATFORM_3DS
    struct SharedArchiveFile;
    struct ReadAhead;
#endif
#ifdef OPENMW3DS_DEBUG
    // Logs the SD reads made through ConstrainedFileStreamBuf since the last call, by request size (count,
    // data, time, MB/s: which read size the card is fastest at), then resets the counts.
    void logReadStats(std::string_view what);
#endif

    /// A file streambuf constrained to a specific region in the file, specified by the 'start' and 'length' parameters.
    class ConstrainedFileStreamBuf final : public std::streambuf
    {
    public:
        ConstrainedFileStreamBuf(const std::filesystem::path& fname, std::size_t start, std::size_t length);
#ifdef OPENMW_PLATFORM_3DS
        ~ConstrainedFileStreamBuf() override;
#endif

        int_type underflow() final;

        pos_type seekoff(off_type offset, std::ios_base::seekdir whence, std::ios_base::openmode mode) final;

        pos_type seekpos(pos_type pos, std::ios_base::openmode mode) final;

    private:
        std::size_t mOrigin;
        std::size_t mSize;
        Platform::File::ScopedHandle mFile;
#ifdef OPENMW_PLATFORM_3DS
        // Every SD request is expensive (ms), so: tellg() and seeks inside the buffer don't touch the file
        // (upstream discards the buffer on any seek, and ESMReader::skip/tellg are frequent), and the read
        // size grows from 32 KB while reads stay sequential, up to 1 MB for whole files (content files) and
        // 256 KB for archive entries. Entries of up to 256 KB are read at once.
        // Archive entries (a constrained region) read through one handle per archive, kept open: opening a
        // file on the SD card costs 15-25 ms, and every model and animation in a BSA reopened it.
        std::shared_ptr<SharedArchiveFile> mShared;
        std::unique_ptr<char[]> mBuffer;
        std::size_t mCapacity = 0; // largest read
        std::size_t mBufferSize = 0; // bytes allocated in mBuffer: mKeep + the current chunk, grown as it doubles
        std::size_t mKeep = 0; // bytes of the previous fill kept in front of a sequential refill (whole files)
        std::size_t mChunk = 0; // current read size
        std::size_t mBufferPos = 0; // stream position (relative to mOrigin) of mBuffer[0]
        std::size_t mFilePos = 0; // stream position the file handle is at
        std::size_t mHandlePos = 0; // where the file handle is (a read-ahead moves it past mFilePos)
        // M10: whole files read sequentially in 1 MB reads (the content files at startup): the next read runs on
        // a helper thread while this one is parsed. Created on the first 1 MB sequential read.
        std::unique_ptr<ReadAhead> mAhead;
        std::size_t position() const { return mBufferPos + (gptr() - eback()); }
        pos_type seekTo(std::size_t newPos);
        std::size_t readAt(std::size_t pos, char* data, std::size_t size);
        // The read-ahead's data if it covers pos (waits for it, start: where it begins), or 0 (none: anything in
        // flight is dropped).
        std::size_t takeAhead(std::size_t pos, std::size_t keep, std::size_t& start);
        void requestAhead(std::size_t pos);
#else
        char mBuffer[8192]{ 0 };
#endif
    };
}

#endif
