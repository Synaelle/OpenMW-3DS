// Nintendo 3DS: the WAV / MP3 sound decoder (see sound3ds.hpp). Output is always 16-bit signed, mono or stereo.
// MP3 goes through mpg123 in feed mode (the file is read in 16 KB pieces from the VFS stream, no seeking).

#include "sound3ds.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <istream>
#include <stdexcept>

#include <mpg123.h>

#include <components/vfs/manager.hpp>

#include <platform3ds/log.hpp>

namespace MWSound
{
    namespace
    {
        constexpr size_t kFeedBytes = 16 * 1024;

        mpg123_handle* handle(void* p)
        {
            return static_cast<mpg123_handle*>(p);
        }

        uint32_t le32(const char* p)
        {
            return uint32_t(uint8_t(p[0])) | uint32_t(uint8_t(p[1])) << 8 | uint32_t(uint8_t(p[2])) << 16
                | uint32_t(uint8_t(p[3])) << 24;
        }

        uint16_t le16(const char* p)
        {
            return uint16_t(uint8_t(p[0]) | uint8_t(p[1]) << 8);
        }

        void initMpg123Once()
        {
            static const bool done = [] {
                mpg123_init();
                const char** decoders = mpg123_decoders();
                P3DS_LOGI("sound: mpg123 API %d, decoder %s", MPG123_API_VERSION,
                    decoders != nullptr && decoders[0] != nullptr ? decoders[0] : "?");
                return true;
            }();
            (void)done;
        }
    }

    FFmpegDecoder::FFmpegDecoder(const VFS::Manager* vfs, HeadCache* /*headCache*/, bool /*recordHead*/)
        : SoundDecoder(vfs)
    {
    }

    FFmpegDecoder::~FFmpegDecoder()
    {
        close();
    }

    void FFmpegDecoder::open(VFS::Path::NormalizedView fname)
    {
        close();
        mName = std::string(fname.value());
        mStream = mResourceMgr->get(fname);
        char magic[4] = {};
        mStream->read(magic, 4);
        if (mStream->gcount() != 4)
            throw std::runtime_error("empty sound file: " + mName);
        mStream->seekg(0);
        if (std::memcmp(magic, "RIFF", 4) == 0)
            openWav();
        else
            openMp3();
    }

    void FFmpegDecoder::close()
    {
        if (mMpg != nullptr)
        {
            mpg123_delete(handle(mMpg));
            mMpg = nullptr;
        }
        mStream.reset();
        mFeed.clear();
        mFeed.shrink_to_fit();
        mSampleRate = 0;
        mChannels = 0;
        mWavBytesLeft = 0;
        mFramesRead = 0;
    }

    void FFmpegDecoder::getInfo(int* samplerate, ChannelConfig* chans, SampleType* type)
    {
        if (mSampleRate == 0)
            throw std::runtime_error("no sound file open");
        *samplerate = mSampleRate;
        *chans = mChannels == 2 ? ChannelConfig_Stereo : ChannelConfig_Mono;
        *type = SampleType_Int16;
    }

    size_t FFmpegDecoder::read(char* buffer, size_t bytes)
    {
        if (mStream == nullptr)
            return 0;
        bytes -= bytes % size_t(2 * mChannels); // whole frames only
        const size_t got = mMpg != nullptr ? readMp3(buffer, bytes) : readWav(buffer, bytes);
        mFramesRead += got / size_t(2 * mChannels);
        return got;
    }

    void FFmpegDecoder::readAll(std::vector<char>& output)
    {
        if (mMpg != nullptr || mWavBits != 16)
        {
            SoundDecoder::readAll(output);
            return;
        }
        // 16-bit WAV (nearly every sound effect): the data chunk is the output, read in one go.
        const size_t start = output.size();
        output.resize(start + mWavBytesLeft);
        output.resize(start + read(output.data() + start, mWavBytesLeft));
    }

    // ------------------------------------------------------------------ WAV

    void FFmpegDecoder::openWav()
    {
        char header[12];
        mStream->read(header, 12);
        if (mStream->gcount() != 12 || std::memcmp(header + 8, "WAVE", 4) != 0)
            throw std::runtime_error("not a WAVE file: " + mName);
        bool haveFormat = false;
        for (;;)
        {
            char chunk[8];
            mStream->read(chunk, 8);
            if (mStream->gcount() != 8)
                throw std::runtime_error("no data chunk in " + mName);
            const uint32_t size = le32(chunk + 4);
            if (std::memcmp(chunk, "fmt ", 4) == 0)
            {
                char fmt[16];
                if (size < 16)
                    throw std::runtime_error("short fmt chunk in " + mName);
                mStream->read(fmt, 16);
                const uint16_t tag = le16(fmt);
                mChannels = le16(fmt + 2);
                mSampleRate = int(le32(fmt + 4));
                mWavBits = le16(fmt + 14);
                if (tag != 1 || (mWavBits != 8 && mWavBits != 16) || mChannels < 1 || mChannels > 2)
                    throw std::runtime_error("unsupported WAV format (tag " + std::to_string(tag) + ", "
                        + std::to_string(mWavBits) + " bits, " + std::to_string(mChannels) + " channels) in " + mName);
                mStream->ignore(size - 16 + (size & 1));
                haveFormat = true;
            }
            else if (std::memcmp(chunk, "data", 4) == 0)
            {
                if (!haveFormat)
                    throw std::runtime_error("data before fmt in " + mName);
                mWavBytesLeft = size;
                return;
            }
            else
                mStream->ignore(size + (size & 1));
        }
    }

    size_t FFmpegDecoder::readWav(char* buffer, size_t bytes)
    {
        if (mWavBits == 16)
        {
            mStream->read(buffer, std::streamsize(std::min(bytes, mWavBytesLeft)));
            const size_t got = size_t(mStream->gcount()) & ~size_t(1);
            mWavBytesLeft -= got;
            return got;
        }
        // 8-bit unsigned: widen in place, from the end.
        const size_t samples = std::min(bytes / 2, mWavBytesLeft);
        mStream->read(buffer, std::streamsize(samples));
        const size_t got = size_t(mStream->gcount());
        mWavBytesLeft -= got;
        int16_t* out = reinterpret_cast<int16_t*>(buffer);
        for (size_t i = got; i-- > 0;)
            out[i] = int16_t((int(uint8_t(buffer[i])) - 128) << 8);
        return got * 2;
    }

    // ------------------------------------------------------------------ MP3

    void FFmpegDecoder::openMp3()
    {
        initMpg123Once();
        int err = MPG123_OK;
        mpg123_handle* mh = mpg123_new(nullptr, &err);
        if (mh == nullptr)
            throw std::runtime_error(std::string("mpg123_new: ") + mpg123_plain_strerror(err));
        mMpg = mh;
        mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_QUIET, 0);
        mpg123_format_none(mh);
        for (long rate : { 8000L, 11025L, 12000L, 16000L, 22050L, 24000L, 32000L, 44100L, 48000L })
            mpg123_format(mh, rate, MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
        if (mpg123_open_feed(mh) != MPG123_OK)
            throw std::runtime_error(std::string("mpg123_open_feed: ") + mpg123_strerror(mh));
        mFeed.resize(kFeedBytes);

        long rate = 0;
        int channels = 0;
        int encoding = 0;
        int r;
        while ((r = mpg123_getformat(mh, &rate, &channels, &encoding)) == MPG123_NEED_MORE)
            if (!feedMp3())
                throw std::runtime_error("no MPEG audio in " + mName);
        if (r != MPG123_OK)
            throw std::runtime_error("mpg123: " + std::string(mpg123_strerror(mh)) + " in " + mName);
        mSampleRate = int(rate);
        mChannels = channels;
    }

    bool FFmpegDecoder::feedMp3()
    {
        mStream->read(mFeed.data(), std::streamsize(mFeed.size()));
        const std::streamsize got = mStream->gcount();
        if (got <= 0)
            return false;
        return mpg123_feed(handle(mMpg), reinterpret_cast<const unsigned char*>(mFeed.data()), size_t(got))
            == MPG123_OK;
    }

    size_t FFmpegDecoder::readMp3(char* buffer, size_t bytes)
    {
        size_t total = 0;
        while (total < bytes)
        {
            size_t done = 0;
            const int r = mpg123_read(
                handle(mMpg), reinterpret_cast<unsigned char*>(buffer + total), bytes - total, &done);
            total += done;
            if (r == MPG123_OK || r == MPG123_NEW_FORMAT)
                continue;
            if (r == MPG123_NEED_MORE)
            {
                if (!feedMp3())
                    break; // end of the file
                continue;
            }
            if (r != MPG123_DONE)
                P3DS_LOGW("sound: mpg123 stopped in %s: %s", mName.c_str(), mpg123_strerror(handle(mMpg)));
            break;
        }
        return total;
    }
}
