#include "constrainedfilestreambuf.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>
#ifdef OPENMW_PLATFORM_3DS
#include <map>
#include <mutex>
#include <string>
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
        mCapacity = std::max<std::size_t>(1, std::min(mSize, kMaxChunk));
        mBuffer.reset(new char[mCapacity]); // not value-initialized
        mChunk = mSize <= kMaxChunk ? mCapacity : kMinChunk;
        setg(mBuffer.get(), mBuffer.get(), mBuffer.get());
    }

    std::size_t ConstrainedFileStreamBuf::readAt(std::size_t pos, char* data, std::size_t size)
    {
        OMW3DS_DETAIL("io.read");
        if (mShared != nullptr)
        {
            const std::lock_guard lock(mShared->mMutex);
            File::seek(mShared->mHandle, mOrigin + pos);
            return File::read(mShared->mHandle, data, size);
        }
        if (pos != mFilePos)
            File::seek(mFile, mOrigin + pos);
        return File::read(mFile, data, size);
    }

    std::streambuf::int_type ConstrainedFileStreamBuf::underflow()
    {
        if (gptr() < egptr())
            return traits_type::to_int_type(*gptr());
        const std::size_t pos = position();
        if (pos >= mSize)
            return traits_type::eof();
        // Consumed a whole fill without seeking away: sequential reading, read bigger chunks.
        if (pos == mFilePos && egptr() != eback())
            mChunk = std::min(mChunk * 2, mCapacity);
        const std::size_t toRead = std::min(mChunk, mSize - pos);
        const std::size_t got = readAt(pos, mBuffer.get(), toRead);
        mFilePos = pos + got;
        mBufferPos = pos;
        setg(mBuffer.get(), mBuffer.get(), mBuffer.get() + got);
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
            mChunk = std::min(mCapacity, kMinChunk);
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
