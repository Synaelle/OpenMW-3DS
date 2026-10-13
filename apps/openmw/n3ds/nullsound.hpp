#ifndef OPENMW_N3DS_NULLSOUND_H
#define OPENMW_N3DS_NULLSOUND_H

// Nintendo 3DS: silent stand-ins for the OpenAL output and the FFmpeg decoder, which do not exist
// on 3DS. They keep the class names soundmanagerimp.cpp / warmqueue.cpp use (and that SoundOutput
// befriends), so those files only swap an #include. init() fails, so the sound manager runs exactly
// as on a PC without an audio device. Replaced by an NDSP output + mpg123/WAV decoders in M7.

#include <stdexcept>
#include <string>
#include <vector>

#include "../mwsound/soundoutput.hpp"
#include "../mwsound/sounddecoder.hpp"

namespace MWSound
{
    class HeadCache;

    class OpenALOutput final : public SoundOutput
    {
    public:
        explicit OpenALOutput(SoundManager& mgr)
            : SoundOutput(mgr)
        {
        }

    private:
        std::vector<std::string> enumerate() override { return {}; }
        bool init(const std::string&, const std::string&, HrtfMode) override { return false; }
        void deinit() override {}
        std::vector<std::string> enumerateHrtf() override { return {}; }

        std::pair<Sound_Handle, size_t> loadSound(VFS::Path::NormalizedView) override { return { nullptr, 0 }; }
        size_t unloadSound(Sound_Handle) override { return 0; }

        bool playSound(Sound*, Sound_Handle, float) override { return false; }
        bool playSound3D(Sound*, Sound_Handle, float) override { return false; }
        void finishSound(Sound*) override {}
        bool isSoundPlaying(Sound*) override { return false; }
        void updateSound(Sound*) override {}

        bool streamSound(DecoderPtr, Stream*, bool) override { return false; }
        bool streamSound3D(DecoderPtr, Stream*, bool) override { return false; }
        void finishStream(Stream*) override {}
        double getStreamDelay(Stream*) override { return 0.0; }
        float getStreamOffset(Stream*) override { return 0.f; }
        float getStreamLoudness(Stream*) override { return 0.f; }
        bool isStreamPlaying(Stream*) override { return false; }
        void updateStream(Stream*) override {}

        void startUpdate() override {}
        void finishUpdate() override {}
        void updateListener(const osg::Vec3f&, const osg::Vec3f&, const osg::Vec3f&, const osg::Vec3f&, Environment) override {}

        void pauseSounds(int) override {}
        void resumeSounds(int) override {}
        void pauseActiveDevice() override {}
        void resumeActiveDevice() override {}
    };

    class FFmpegDecoder final : public SoundDecoder
    {
    public:
        FFmpegDecoder(const VFS::Manager* vfs, HeadCache*, bool /*recordHead*/)
            : SoundDecoder(vfs)
        {
        }

        void open(VFS::Path::NormalizedView fname) override
        {
            throw std::runtime_error("no audio decoder on 3DS yet: " + std::string(fname.value()));
        }
        void close() override {}
        std::string getName() override { return {}; }
        void getInfo(int* samplerate, ChannelConfig* chans, SampleType* type) override
        {
            *samplerate = 0;
            *chans = ChannelConfig_Mono;
            *type = SampleType_Int16;
        }
        size_t read(char*, size_t) override { return 0; }
        size_t getSampleOffset() override { return 0; }
    };
}

#endif
