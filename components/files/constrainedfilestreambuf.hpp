#ifndef OPENMW_CONSTRAINEDFILESTREAMBUF_H
#define OPENMW_CONSTRAINEDFILESTREAMBUF_H

#include <filesystem>
#include <memory>
#include <streambuf>

#include <components/platform/file.hpp>

namespace Files
{
    /// A file streambuf constrained to a specific region in the file, specified by the 'start' and 'length' parameters.
    class ConstrainedFileStreamBuf final : public std::streambuf
    {
    public:
        ConstrainedFileStreamBuf(const std::filesystem::path& fname, std::size_t start, std::size_t length);

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
        // size grows from 32 KB to 256 KB while reads stay sequential.
        std::unique_ptr<char[]> mBuffer;
        std::size_t mCapacity = 0;
        std::size_t mChunk = 0; // current read size
        std::size_t mBufferPos = 0; // stream position (relative to mOrigin) of mBuffer[0]
        std::size_t mFilePos = 0; // stream position the file handle is at
        std::size_t position() const { return mBufferPos + (gptr() - eback()); }
        pos_type seekTo(std::size_t newPos);
#else
        char mBuffer[8192]{ 0 };
#endif
    };
}

#endif
