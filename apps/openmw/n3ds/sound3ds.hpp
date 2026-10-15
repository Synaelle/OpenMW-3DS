#ifndef OPENMW_N3DS_SOUND3DS_H
#define OPENMW_N3DS_SOUND3DS_H

// Nintendo 3DS sound (M7): the DSP's mixer (libctru's NDSP: 24 channels, PCM16 at any rate, per-channel stereo
// mix and low-pass filter) and a WAV / MP3 (mpg123) decoder, in place of the OpenAL output and the FFmpeg decoder,
// which don't exist on 3DS. They keep those classes' names, which soundmanagerimp.cpp and warmqueue.cpp use and
// which SoundManager, SoundOutput and SoundBase befriend, so upstream files only swap an #include.
//
// Morrowind's audio: sound effects are 16-bit PCM WAV (mostly 22 kHz mono), voices MP3 (44.1 kHz mono), music MP3
// (44.1 kHz stereo). Sound effects are decoded whole into linear memory (the DSP reads it directly); music and
// voices stream through a thread on the system core (n3ds/ndspoutput.cpp).

#include <array>
#include <memory>
#include <string>
#include <vector>

#include <components/files/istreamptr.hpp>

#include <osg/Vec3f>

#include "../mwsound/sounddecoder.hpp"
#include "../mwsound/soundoutput.hpp"

namespace MWSound
{
    class HeadCache;
    class SoundBase;

    // Same rule as the FFmpeg decoder (ffmpegdecoder.cpp).
    inline bool hasAudioExtension(VFS::Path::NormalizedView fname)
    {
        const auto ext = fname.extension();
        return ext == "mp3" || ext == "ogg" || ext == "wav" || ext == "flac" || ext == "opus";
    }

    // The NDSP output (not OpenAL: see above).
    class OpenALOutput final : public SoundOutput
    {
    public:
        explicit OpenALOutput(SoundManager& mgr);
        ~OpenALOutput() override;

    private:
        // A sound effect (the Sound_Handle): read and decoded on the sound thread, then copied into linear memory
        // (the DSP reads it there) on the main thread, which alone may use libctru's linear allocator.
        struct Effect;
        // One NDSP channel playing an effect: up to two wave buffers (the part after a start offset, then the whole
        // effect looping). Pending while its effect is still loading.
        struct Voice;
        struct StreamVoice;
        class StreamThread;

        std::vector<std::string> enumerate() override;
        bool init(const std::string& devname, const std::string& hrtfname, HrtfMode hrtfmode) override;
        void deinit() override;
        std::vector<std::string> enumerateHrtf() override { return {}; }

        std::pair<Sound_Handle, size_t> loadSound(VFS::Path::NormalizedView fname) override;
        size_t unloadSound(Sound_Handle data) override;

        bool playSound(Sound* sound, Sound_Handle data, float offset) override;
        bool playSound3D(Sound* sound, Sound_Handle data, float offset) override;
        void finishSound(Sound* sound) override;
        bool isSoundPlaying(Sound* sound) override;
        void updateSound(Sound* sound) override;

        bool streamSound(DecoderPtr decoder, Stream* sound, bool getLoudnessData) override;
        bool streamSound3D(DecoderPtr decoder, Stream* sound, bool getLoudnessData) override;
        void finishStream(Stream* sound) override;
        double getStreamDelay(Stream* sound) override;
        float getStreamOffset(Stream* sound) override;
        float getStreamLoudness(Stream* sound) override;
        bool isStreamPlaying(Stream* sound) override;
        void updateStream(Stream* sound) override;

        void startUpdate() override {}
        void finishUpdate() override {}
        void updateListener(const osg::Vec3f& pos, const osg::Vec3f& atdir, const osg::Vec3f& updir,
            const osg::Vec3f& vel, Environment env) override;

        void pauseSounds(int types) override;
        void resumeSounds(int types) override;
        void pauseActiveDevice() override;
        void resumeActiveDevice() override;

        bool play(Sound* sound, Sound_Handle data, float offset);
        // Sound thread: reads and decodes the effect's file.
        void decodeEffect(Effect& effect);
        // Main thread: moves a decoded effect into linear memory. True once it can play.
        bool makeReady(Effect& effect);
        // Queues a pending voice's effect, now ready, on its channel.
        void start(Voice& voice, Sound* sound);
        bool stream(DecoderPtr decoder, Stream* sound, bool getLoudnessData);
        int takeChannel();
        void releaseChannel(int channel);
        // Volume, pan, pitch and the underwater filter of the channel playing `sound`.
        void apply(int channel, SoundBase* sound, int sampleRate, int channels);
        float getTimeScaledPitch(SoundBase* sound);

        std::unique_ptr<std::array<Voice, 24>> mVoices;
        std::array<bool, 24> mFiltered{}; // the underwater low-pass is on
        std::vector<int> mFreeChannels;
        std::vector<Sound*> mActiveSounds;
        std::vector<Stream*> mActiveStreams;
        std::unique_ptr<StreamThread> mStreamThread;

        osg::Vec3f mListenerPos;
        osg::Vec3f mListenerAt{ 0.f, 1.f, 0.f };
        osg::Vec3f mListenerUp{ 0.f, 0.f, 1.f };
        Environment mListenerEnv = Env_Normal;
        unsigned mNoChannelWarnings = 0;
    };

    // For the 5-second perf log (frame3ds.cpp): what plays now, and the stream thread's decoding time since the
    // last call.
    struct SoundStats3DS
    {
        bool mActive = false;
        int mSounds = 0;
        int mStreams = 0;
        size_t mBufferBytes = 0;
        double mDecodeMs = 0.0;
        unsigned mSkipped = 0;
        unsigned mUnderruns = 0;
    };
    SoundStats3DS takeSoundStats3DS();

    // The WAV / MP3 decoder (not FFmpeg: see above). Always outputs 16-bit samples.
    class FFmpegDecoder final : public SoundDecoder
    {
    public:
        FFmpegDecoder(const VFS::Manager* vfs, HeadCache* headCache, bool recordHead);
        ~FFmpegDecoder() override;

        void open(VFS::Path::NormalizedView fname) override;
        void close() override;
        std::string getName() override { return mName; }
        void getInfo(int* samplerate, ChannelConfig* chans, SampleType* type) override;
        size_t read(char* buffer, size_t bytes) override;
        void readAll(std::vector<char>& output) override;
        size_t getSampleOffset() override { return mFramesRead; }

    private:
        void openWav();
        void openMp3();
        size_t readWav(char* buffer, size_t bytes);
        size_t readMp3(char* buffer, size_t bytes);
        // Hands mpg123 the next piece of the file; false at its end.
        bool feedMp3();

        Files::IStreamPtr mStream;
        std::string mName;
        void* mMpg = nullptr; // mpg123_handle
        std::vector<char> mFeed;
        int mSampleRate = 0;
        int mChannels = 0;
        int mWavBits = 16;
        size_t mWavBytesLeft = 0;
        size_t mFramesRead = 0;
    };
}

#endif
