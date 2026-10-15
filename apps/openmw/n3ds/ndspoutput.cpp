// Nintendo 3DS: the NDSP sound output (see sound3ds.hpp).
//
// - Sound effects: read and decoded by the sound thread (no hitch on the main thread when a new one first plays: it
//   starts once loaded, ~20-100 ms later), then copied into linear memory (the DSP reads it there) on the main thread:
//   libctru's linear allocator has no lock, and the renderer uses it on the main thread. One NDSP channel per sound.
// - Music and voices: streamed in 4 buffers of 0.25 s, decoded by the same thread.
// - The sound thread runs on the system core (core 1, time-limited, so it never takes time from the game's cores;
//   core 2 when the system core isn't available).
// - 3D: OpenAL's model as OpenMW sets it (inverse distance, clamped, rolloff 1) for the gain; the left/right mix from
//   the direction to the listener; no Doppler, no reverb. Underwater: the DSP's one-pole low-pass on the channel.
// - 24 channels in all (the DSP's limit). When they're all busy, new sounds don't play (as OpenAL without sources).

#include "sound3ds.hpp"

#include <3ds.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>

#include <components/debug/debuglog.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/vfs/manager.hpp>

#include <platform3ds/log.hpp>
#include <platform3ds/platform.hpp>

#include "../mwsound/loudness.hpp"
#include "../mwsound/sound.hpp"
#include "../mwsound/soundmanagerimp.hpp"

namespace MWSound
{
    namespace
    {
        constexpr int kChannelCount = 24;
        constexpr float kLoudnessFPS = 20.f; // loudness values per second of audio, as openaloutput.cpp
        constexpr float kUnderwaterCutoffHz = 1200.f;
        // What an effect counts for in the sound buffer pool's cache limits ([Sound] buffer cache min/max), told
        // before the file is read: a bit above Morrowind's average (84 KB). The real total is in the perf log.
        constexpr size_t kEffectAccountedBytes = 128 * 1024;

        u16 ndspFormat(int channels)
        {
            return channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16;
        }

        bool isQueued(const ndspWaveBuf& wave)
        {
            return wave.status == NDSP_WBUF_QUEUED || wave.status == NDSP_WBUF_PLAYING;
        }

        int channelCount(ChannelConfig config)
        {
            return config == ChannelConfig_Stereo ? 2 : 1;
        }

        double ticksToMs(uint64_t ticks)
        {
            return double(ticks) / (SYSCLOCK_ARM11 / 1000.0);
        }

        // takeSoundStats3DS
        bool gActive = false;
        int gSounds = 0;
        int gStreams = 0;
        size_t gEffectBytes = 0; // in linear memory
        unsigned gSkipped = 0;
        std::atomic<uint64_t> gDecodeTicks{ 0 }; // sound thread
        std::atomic<unsigned> gUnderruns{ 0 }; // sound thread: a stream played out before it was refilled
    }

    SoundStats3DS takeSoundStats3DS()
    {
        SoundStats3DS stats;
        stats.mActive = gActive;
        stats.mSounds = gSounds;
        stats.mStreams = gStreams;
        stats.mBufferBytes = gEffectBytes;
        stats.mDecodeMs = ticksToMs(gDecodeTicks.exchange(0));
        stats.mSkipped = gSkipped;
        stats.mUnderruns = gUnderruns.exchange(0);
        gSkipped = 0;
        return stats;
    }

    struct OpenALOutput::Effect
    {
        enum State
        {
            Loading, // queued for, or being read by, the sound thread
            Decoded, // in mPcm (sound thread done)
            Ready, // in linear memory (main thread)
            Failed,
        };

        std::string mName;
        std::atomic<int> mState{ Loading };
        std::vector<char> mPcm; // written by the sound thread before mState = Decoded
        int16_t* mData = nullptr;
        uint32_t mFrames = 0;
        size_t mBytes = 0;
        int mRate = 0;
        int mChannels = 1;
    };

    struct OpenALOutput::Voice
    {
        int mChannel = -1;
        Effect* mEffect = nullptr;
        bool mPending = false; // waiting for mEffect to load
        float mOffset = 0.f; // where to start, in seconds (pending voices)
        std::array<ndspWaveBuf, 2> mWave{};

        bool isPlaying() const { return isQueued(mWave[0]) || isQueued(mWave[1]); }
    };

    // A music or voice stream. The sound thread decodes into it outside of any lock that the main thread waits on;
    // mLock only guards the few fields both read (held for microseconds).
    struct OpenALOutput::StreamVoice
    {
        static constexpr int kBuffers = 4;
        static constexpr float kBufferSeconds = 0.25f;

        int mChannel = -1;
        DecoderPtr mDecoder;
        int mRate = 0;
        int mChannels = 1;
        uint32_t mBufferFrames = 0;
        std::array<int16_t*, kBuffers> mData{};
        std::array<ndspWaveBuf, kBuffers> mWave{};
        int mNext = 0; // sound thread only
        std::unique_ptr<Sound_Loudness> mLoudness;
        std::vector<char> mLoudnessScratch; // sound thread only

        mutable std::mutex mLock;
        std::array<uint64_t, kBuffers> mStartFrame{};
        uint64_t mDecodedFrames = 0;
        bool mFinished = false;

        ~StreamVoice()
        {
            for (int16_t* data : mData)
                if (data != nullptr)
                    linearFree(data);
        }

        bool init(bool getLoudnessData)
        {
            ChannelConfig chans;
            SampleType type;
            mDecoder->getInfo(&mRate, &chans, &type);
            if (type != SampleType_Int16 || (chans != ChannelConfig_Mono && chans != ChannelConfig_Stereo)
                || mRate <= 0)
                return false;
            mChannels = channelCount(chans);
            mBufferFrames = uint32_t(kBufferSeconds * float(mRate));
            for (int i = 0; i < kBuffers; ++i)
            {
                mData[i] = static_cast<int16_t*>(linearAlloc(size_t(mBufferFrames) * size_t(mChannels) * 2));
                if (mData[i] == nullptr)
                {
                    P3DS_LOGW("sound: no linear memory for a stream of %s", mDecoder->getName().c_str());
                    return false;
                }
                mWave[i].data_pcm16 = mData[i];
                mWave[i].status = NDSP_WBUF_FREE;
            }
            if (getLoudnessData)
                mLoudness = std::make_unique<Sound_Loudness>(kLoudnessFPS, mRate, chans, type);
            return true;
        }

        // Decodes into each played-out buffer, in order. False once the decoder has nothing left.
        bool process()
        {
            const size_t bufferBytes = size_t(mBufferFrames) * size_t(mChannels) * 2;
            if (mDecodedFrames > 0 && !std::any_of(mWave.begin(), mWave.end(), isQueued))
                ++gUnderruns; // everything decoded so far has played: a gap was heard
            while (!isQueued(mWave[mNext]))
            {
                size_t got = 0;
                const uint64_t startTick = svcGetSystemTick();
                try
                {
                    got = mDecoder->read(reinterpret_cast<char*>(mData[mNext]), bufferBytes);
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Error) << "Error updating stream \"" << mDecoder->getName() << "\": " << e.what();
                }
                gDecodeTicks += svcGetSystemTick() - startTick;
                const uint32_t frames = uint32_t(got / size_t(mChannels * 2));
                if (frames > 0)
                    DSP_FlushDataCache(mData[mNext], got);

                std::lock_guard<std::mutex> lock(mLock);
                if (frames > 0)
                {
                    if (mLoudness != nullptr)
                    {
                        const char* bytes = reinterpret_cast<const char*>(mData[mNext]);
                        mLoudnessScratch.assign(bytes, bytes + got);
                        mLoudness->analyzeLoudness(mLoudnessScratch);
                    }
                    ndspWaveBuf& wave = mWave[mNext];
                    wave.nsamples = frames;
                    wave.looping = false;
                    mStartFrame[mNext] = mDecodedFrames;
                    mDecodedFrames += frames;
                    ndspChnWaveBufAdd(mChannel, &wave);
                    mNext = (mNext + 1) % kBuffers;
                }
                if (got < bufferBytes)
                {
                    mFinished = true;
                    return false;
                }
            }
            return true;
        }

        bool isPlaying() const
        {
            std::lock_guard<std::mutex> lock(mLock);
            if (!mFinished)
                return true;
            return std::any_of(mWave.begin(), mWave.end(), isQueued);
        }

        // Seconds into the stream of what's heard now (mLock held).
        double offsetLocked() const
        {
            for (int i = 0; i < kBuffers; ++i)
                if (mWave[i].status == NDSP_WBUF_PLAYING)
                    return double(mStartFrame[i] + ndspChnGetSamplePos(mChannel)) / mRate;
            uint64_t earliest = mDecodedFrames; // underrun, or not started yet: what's decoded next
            for (int i = 0; i < kBuffers; ++i)
                if (mWave[i].status == NDSP_WBUF_QUEUED)
                    earliest = std::min(earliest, mStartFrame[i]);
            return double(earliest) / mRate;
        }

        float offset() const
        {
            std::lock_guard<std::mutex> lock(mLock);
            return float(offsetLocked());
        }

        double delay() const
        {
            std::lock_guard<std::mutex> lock(mLock);
            return std::max(0.0, double(mDecodedFrames) / mRate - offsetLocked());
        }

        float loudness() const
        {
            std::lock_guard<std::mutex> lock(mLock);
            return mLoudness != nullptr ? mLoudness->getLoudnessAtTime(float(offsetLocked())) : 0.f;
        }
    };

    // The sound thread: keeps the streams' buffers filled (a buffer lasts 250 ms; it wakes every 40 ms) and loads
    // effects, one at a time between stream refills.
    class OpenALOutput::StreamThread
    {
    public:
        explicit StreamThread(OpenALOutput& output)
            : mOutput(output)
        {
            s32 mainPriority = 0x30;
            svcGetThreadPriority(&mainPriority, CUR_THREAD_HANDLE);
            const int core = p3ds::info().syscoreLimit > 0 ? 1 : 2;
            mThread = threadCreate(entry, this, 64 * 1024, mainPriority - 1, core, false);
            if (mThread == nullptr)
                mThread = threadCreate(entry, this, 64 * 1024, mainPriority - 1, 0, false);
            P3DS_LOGI("sound: sound thread on core %d", mThread != nullptr ? core : -1);
        }

        ~StreamThread()
        {
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mQuit = true;
            }
            mWake.notify_all();
            if (mThread != nullptr)
            {
                threadJoin(mThread, U64_MAX);
                threadFree(mThread);
            }
        }

        void add(StreamVoice* stream)
        {
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (std::find(mStreams.begin(), mStreams.end(), stream) == mStreams.end())
                    mStreams.push_back(stream);
            }
            mWake.notify_all();
        }

        // Returns once the thread no longer touches `stream`.
        void remove(StreamVoice* stream)
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mStreams.erase(std::remove(mStreams.begin(), mStreams.end(), stream), mStreams.end());
            mIdle.wait(lock, [&] { return mBusyStream != stream; });
        }

        void load(Effect* effect)
        {
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mLoads.push_back(effect);
            }
            mWake.notify_all();
        }

        // Returns once the thread no longer touches `effect` (dropped from the queue, or its load finished).
        void cancel(Effect* effect)
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mLoads.erase(std::remove(mLoads.begin(), mLoads.end(), effect), mLoads.end());
            mIdle.wait(lock, [&] { return mBusyEffect != effect; });
        }

    private:
        static void entry(void* self) { static_cast<StreamThread*>(self)->run(); }

        void run()
        {
            std::unique_lock<std::mutex> lock(mMutex);
            while (!mQuit)
            {
                const std::vector<StreamVoice*> work = mStreams;
                for (StreamVoice* stream : work)
                {
                    if (std::find(mStreams.begin(), mStreams.end(), stream) == mStreams.end())
                        continue; // removed meanwhile
                    mBusyStream = stream;
                    lock.unlock();
                    const bool more = stream->process();
                    lock.lock();
                    mBusyStream = nullptr;
                    if (!more)
                        mStreams.erase(std::remove(mStreams.begin(), mStreams.end(), stream), mStreams.end());
                    mIdle.notify_all();
                }
                if (!mLoads.empty())
                {
                    Effect* effect = mLoads.front();
                    mLoads.pop_front();
                    mBusyEffect = effect;
                    lock.unlock();
                    mOutput.decodeEffect(*effect);
                    lock.lock();
                    mBusyEffect = nullptr;
                    mIdle.notify_all();
                    continue; // next effect right after the streams
                }
                mWake.wait_for(lock, std::chrono::milliseconds(40));
            }
        }

        OpenALOutput& mOutput;
        std::mutex mMutex;
        std::condition_variable mWake;
        std::condition_variable mIdle;
        std::vector<StreamVoice*> mStreams;
        std::deque<Effect*> mLoads;
        StreamVoice* mBusyStream = nullptr;
        Effect* mBusyEffect = nullptr;
        bool mQuit = false;
        Thread mThread = nullptr;
    };

    OpenALOutput::OpenALOutput(SoundManager& mgr)
        : SoundOutput(mgr)
        , mVoices(std::make_unique<std::array<Voice, kChannelCount>>())
    {
    }

    OpenALOutput::~OpenALOutput()
    {
        OpenALOutput::deinit();
    }

    std::vector<std::string> OpenALOutput::enumerate()
    {
        return { "NDSP" };
    }

    bool OpenALOutput::init(const std::string& /*devname*/, const std::string& /*hrtfname*/, HrtfMode /*hrtfmode*/)
    {
        deinit();
        const Result rc = ndspInit();
        if (R_FAILED(rc))
        {
            P3DS_LOGE("sound: the DSP didn't start (0x%08lX): it needs its firmware, sdmc:/3ds/dspfirm.cdc (dump it "
                      "once with the DSP1 homebrew). No sound this session.",
                (unsigned long)rc);
            return false;
        }
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspSetClippingMode(NDSP_CLIP_SOFT);
        ndspSetMasterVol(1.f);
        mFreeChannels.clear();
        for (int i = kChannelCount - 1; i >= 0; --i)
        {
            (*mVoices)[i].mChannel = i;
            mFreeChannels.push_back(i);
        }
        mFiltered.fill(false);
        mStreamThread = std::make_unique<StreamThread>(*this);
        mInitialized = true;
        gActive = true;
        P3DS_LOGI("sound: NDSP ready, %d channels", kChannelCount);
        return true;
    }

    void OpenALOutput::deinit()
    {
        if (!mInitialized)
            return;
        for (Stream* stream : std::vector<Stream*>(mActiveStreams))
            finishStream(stream);
        for (Sound* sound : std::vector<Sound*>(mActiveSounds))
            finishSound(sound);
        mStreamThread.reset();
        ndspExit();
        mInitialized = false;
        gActive = false;
    }

    std::pair<Sound_Handle, size_t> OpenALOutput::loadSound(VFS::Path::NormalizedView fname)
    {
        if (mStreamThread == nullptr)
            return { nullptr, 0 };
        auto* effect = new Effect;
        effect->mName = std::string(fname.value());
        mStreamThread->load(effect);
        return { effect, kEffectAccountedBytes };
    }

    void OpenALOutput::decodeEffect(Effect& effect)
    {
        const uint64_t startTick = svcGetSystemTick();
        std::vector<char> data;
        try
        {
            DecoderPtr decoder = mManager.getDecoder();
            decoder->open(Misc::ResourceHelpers::correctSoundPath(
                VFS::Path::Normalized(effect.mName), *decoder->mResourceMgr));
            ChannelConfig chans;
            SampleType type;
            decoder->getInfo(&effect.mRate, &chans, &type);
            effect.mChannels = channelCount(chans);
            decoder->readAll(data);
        }
        catch (const std::exception& e)
        {
            Log(Debug::Error) << "Failed to load audio from " << effect.mName << ": " << e.what();
            data.clear();
        }
        if (data.size() < size_t(2 * effect.mChannels))
        {
            // As OpenAL: silence instead of a sound that failed to load (one second at 8 kHz).
            effect.mRate = 8000;
            effect.mChannels = 1;
            data.assign(2 * 8000, 0);
        }
        effect.mFrames = uint32_t(data.size() / size_t(2 * effect.mChannels));
        effect.mBytes = size_t(effect.mFrames) * size_t(2 * effect.mChannels);
        data.resize(effect.mBytes);
        effect.mPcm = std::move(data);
        const double ms = ticksToMs(svcGetSystemTick() - startTick);
        if (ms >= 30.0)
            P3DS_LOGI("sound: loading %s took %.0f ms (%zu KB)", effect.mName.c_str(), ms, effect.mBytes / 1024);
        effect.mState.store(Effect::Decoded, std::memory_order_release);
    }

    bool OpenALOutput::makeReady(Effect& effect)
    {
        const int state = effect.mState.load(std::memory_order_acquire);
        if (state != Effect::Decoded)
            return state == Effect::Ready;
        effect.mData = static_cast<int16_t*>(linearAlloc(effect.mBytes));
        if (effect.mData == nullptr)
        {
            P3DS_LOGW("sound: no linear memory for %s (%zu KB, %lu KB free)", effect.mName.c_str(),
                effect.mBytes / 1024, (unsigned long)(linearSpaceFree() / 1024));
            effect.mPcm = {};
            effect.mState = Effect::Failed;
            return false;
        }
        std::memcpy(effect.mData, effect.mPcm.data(), effect.mBytes);
        DSP_FlushDataCache(effect.mData, effect.mBytes);
        effect.mPcm = {};
        gEffectBytes += effect.mBytes;
        effect.mState = Effect::Ready;
        return true;
    }

    size_t OpenALOutput::unloadSound(Sound_Handle data)
    {
        auto* effect = static_cast<Effect*>(data);
        if (effect == nullptr)
            return 0;
        if (mStreamThread != nullptr)
            mStreamThread->cancel(effect);
        // Nothing may keep playing it.
        for (Sound* sound : mActiveSounds)
        {
            auto* voice = static_cast<Voice*>(sound->mHandle);
            if (voice != nullptr && voice->mEffect == effect)
            {
                ndspChnWaveBufClear(voice->mChannel);
                voice->mWave = {};
                voice->mEffect = nullptr;
                voice->mPending = false;
            }
        }
        if (effect->mData != nullptr)
        {
            linearFree(effect->mData);
            gEffectBytes -= effect->mBytes;
        }
        delete effect;
        return kEffectAccountedBytes;
    }

    int OpenALOutput::takeChannel()
    {
        if (mFreeChannels.empty())
        {
            ++gSkipped;
            if (mNoChannelWarnings++ % 100 == 0)
                P3DS_LOGW("sound: all %d channels busy, a sound was skipped (%u times so far)", kChannelCount,
                    mNoChannelWarnings);
            return -1;
        }
        const int channel = mFreeChannels.back();
        mFreeChannels.pop_back();
        ndspChnReset(channel);
        mFiltered[channel] = false;
        return channel;
    }

    void OpenALOutput::releaseChannel(int channel)
    {
        ndspChnWaveBufClear(channel);
        mFreeChannels.push_back(channel);
    }

    void OpenALOutput::apply(int channel, SoundBase* sound, int sampleRate, int channels)
    {
        float gain = sound->getRealVolume();
        float pan = 0.f;
        if (sound->getIs3D())
        {
            const osg::Vec3f toSound = sound->getPosition() - mListenerPos;
            const float distance = toSound.length();
            const float minDistance = std::max(sound->getMinDistance(), 1e-3f);
            const float maxDistance = std::max(sound->getMaxDistance(), minDistance);
            const float clamped = std::clamp(distance, minDistance, maxDistance);
            gain *= minDistance / (minDistance + (clamped - minDistance));
            if (distance > 1e-3f)
            {
                osg::Vec3f right = mListenerAt ^ mListenerUp;
                right.normalize();
                pan = std::clamp((toSound * right) / distance, -1.f, 1.f);
            }
        }
        const bool underwater = sound->getUseEnv() && mListenerEnv == Env_Underwater;
        if (underwater)
            gain *= 0.9f;
        gain = std::clamp(gain, 0.f, 1.f);

        float mix[12] = {};
        if (channels == 2)
        {
            mix[0] = gain;
            mix[1] = gain;
        }
        else
        {
            // Never fully on one side; centred sounds at full gain on both.
            const float p = 0.8f * pan;
            mix[0] = gain * std::min(1.f, std::sqrt(1.f - p));
            mix[1] = gain * std::min(1.f, std::sqrt(1.f + p));
        }
        ndspChnSetMix(channel, mix);
        ndspChnSetRate(channel, float(sampleRate) * getTimeScaledPitch(sound));
        if (underwater != mFiltered[channel])
        {
            if (underwater)
                ndspChnIirMonoSetParamsLowPassFilter(channel, kUnderwaterCutoffHz);
            ndspChnIirMonoSetEnable(channel, underwater);
            mFiltered[channel] = underwater;
        }
    }

    void OpenALOutput::start(Voice& voice, Sound* sound)
    {
        const Effect& effect = *voice.mEffect;
        voice.mPending = false;
        voice.mWave = {};
        const uint32_t start = std::min(uint32_t(std::max(0.f, voice.mOffset) * float(effect.mRate)), effect.mFrames);
        const bool loop = sound->getIsLooping();
        ndspChnSetFormat(voice.mChannel, ndspFormat(effect.mChannels));
        apply(voice.mChannel, sound, effect.mRate, effect.mChannels);
        int count = 0;
        if (start < effect.mFrames && (start > 0 || !loop))
        {
            ndspWaveBuf& wave = voice.mWave[count++];
            wave.data_pcm16 = effect.mData + size_t(start) * size_t(effect.mChannels);
            wave.nsamples = effect.mFrames - start;
            wave.looping = false;
        }
        if (loop)
        {
            ndspWaveBuf& wave = voice.mWave[count++];
            wave.data_pcm16 = effect.mData;
            wave.nsamples = effect.mFrames;
            wave.looping = true;
        }
        for (int i = 0; i < count; ++i)
            ndspChnWaveBufAdd(voice.mChannel, &voice.mWave[i]);
    }

    bool OpenALOutput::play(Sound* sound, Sound_Handle data, float offset)
    {
        auto* effect = static_cast<Effect*>(data);
        if (effect == nullptr)
            return false;
        const bool ready = makeReady(*effect);
        if (!ready && effect->mState == Effect::Failed)
            return false;
        const int channel = takeChannel();
        if (channel < 0)
            return false;

        Voice& voice = (*mVoices)[channel];
        voice.mEffect = effect;
        voice.mOffset = offset;
        voice.mPending = true;
        voice.mWave = {};
        ndspChnSetInterp(channel, NDSP_INTERP_LINEAR);
        if (ready)
            start(voice, sound);
        sound->mHandle = &voice;
        mActiveSounds.push_back(sound);
        gSounds = int(mActiveSounds.size());
        return true;
    }

    bool OpenALOutput::playSound(Sound* sound, Sound_Handle data, float offset)
    {
        return play(sound, data, offset);
    }

    bool OpenALOutput::playSound3D(Sound* sound, Sound_Handle data, float offset)
    {
        return play(sound, data, offset);
    }

    void OpenALOutput::finishSound(Sound* sound)
    {
        auto* voice = static_cast<Voice*>(sound->mHandle);
        if (voice == nullptr)
            return;
        sound->mHandle = nullptr;
        releaseChannel(voice->mChannel);
        voice->mEffect = nullptr;
        voice->mPending = false;
        voice->mWave = {};
        mActiveSounds.erase(std::find(mActiveSounds.begin(), mActiveSounds.end(), sound));
        gSounds = int(mActiveSounds.size());
    }

    bool OpenALOutput::isSoundPlaying(Sound* sound)
    {
        auto* voice = static_cast<Voice*>(sound->mHandle);
        if (voice == nullptr || voice->mEffect == nullptr)
            return false;
        if (voice->mPending)
        {
            if (makeReady(*voice->mEffect))
                start(*voice, sound);
            else
                return voice->mEffect->mState != Effect::Failed; // still loading
        }
        return voice->isPlaying();
    }

    void OpenALOutput::updateSound(Sound* sound)
    {
        auto* voice = static_cast<Voice*>(sound->mHandle);
        if (voice == nullptr || voice->mEffect == nullptr)
            return;
        if (voice->mPending)
        {
            if (makeReady(*voice->mEffect))
                start(*voice, sound);
            return;
        }
        apply(voice->mChannel, sound, voice->mEffect->mRate, voice->mEffect->mChannels);
    }

    bool OpenALOutput::stream(DecoderPtr decoder, Stream* sound, bool getLoudnessData)
    {
        const int channel = takeChannel();
        if (channel < 0)
            return false;
        auto voice = std::make_unique<StreamVoice>();
        voice->mChannel = channel;
        voice->mDecoder = std::move(decoder);
        bool ok = false;
        try
        {
            ok = voice->init(getLoudnessData);
        }
        catch (const std::exception& e)
        {
            Log(Debug::Error) << "Failed to get stream info: " << e.what();
        }
        if (!ok)
        {
            releaseChannel(channel);
            return false;
        }
        ndspChnSetInterp(channel, NDSP_INTERP_POLYPHASE);
        ndspChnSetFormat(channel, ndspFormat(voice->mChannels));
        apply(channel, sound, voice->mRate, voice->mChannels);
        sound->mHandle = voice.get();
        mActiveStreams.push_back(sound);
        gStreams = int(mActiveStreams.size());
        mStreamThread->add(voice.release());
        return true;
    }

    bool OpenALOutput::streamSound(DecoderPtr decoder, Stream* sound, bool getLoudnessData)
    {
        return stream(std::move(decoder), sound, getLoudnessData);
    }

    bool OpenALOutput::streamSound3D(DecoderPtr decoder, Stream* sound, bool getLoudnessData)
    {
        return stream(std::move(decoder), sound, getLoudnessData);
    }

    void OpenALOutput::finishStream(Stream* sound)
    {
        auto* voice = static_cast<StreamVoice*>(sound->mHandle);
        if (voice == nullptr)
            return;
        sound->mHandle = nullptr;
        mStreamThread->remove(voice);
        releaseChannel(voice->mChannel);
        mActiveStreams.erase(std::find(mActiveStreams.begin(), mActiveStreams.end(), sound));
        gStreams = int(mActiveStreams.size());
        delete voice;
    }

    double OpenALOutput::getStreamDelay(Stream* sound)
    {
        const auto* voice = static_cast<const StreamVoice*>(sound->mHandle);
        return voice != nullptr ? voice->delay() : 0.0;
    }

    float OpenALOutput::getStreamOffset(Stream* sound)
    {
        const auto* voice = static_cast<const StreamVoice*>(sound->mHandle);
        return voice != nullptr ? voice->offset() : 0.f;
    }

    float OpenALOutput::getStreamLoudness(Stream* sound)
    {
        const auto* voice = static_cast<const StreamVoice*>(sound->mHandle);
        return voice != nullptr ? voice->loudness() : 0.f;
    }

    bool OpenALOutput::isStreamPlaying(Stream* sound)
    {
        const auto* voice = static_cast<const StreamVoice*>(sound->mHandle);
        return voice != nullptr && voice->isPlaying();
    }

    void OpenALOutput::updateStream(Stream* sound)
    {
        const auto* voice = static_cast<const StreamVoice*>(sound->mHandle);
        if (voice != nullptr)
            apply(voice->mChannel, sound, voice->mRate, voice->mChannels);
    }

    void OpenALOutput::updateListener(const osg::Vec3f& pos, const osg::Vec3f& atdir, const osg::Vec3f& updir,
        const osg::Vec3f& /*vel*/, Environment env)
    {
        mListenerPos = pos;
        mListenerAt = atdir;
        mListenerUp = updir;
        mListenerEnv = env;
    }

    void OpenALOutput::pauseSounds(int types)
    {
        for (Sound* sound : mActiveSounds)
            if (types & sound->getPlayType())
                ndspChnSetPaused(static_cast<Voice*>(sound->mHandle)->mChannel, true);
        for (Stream* sound : mActiveStreams)
            if (types & sound->getPlayType())
                ndspChnSetPaused(static_cast<StreamVoice*>(sound->mHandle)->mChannel, true);
    }

    void OpenALOutput::resumeSounds(int types)
    {
        for (Sound* sound : mActiveSounds)
            if (types & sound->getPlayType())
                ndspChnSetPaused(static_cast<Voice*>(sound->mHandle)->mChannel, false);
        for (Stream* sound : mActiveStreams)
            if (types & sound->getPlayType())
                ndspChnSetPaused(static_cast<StreamVoice*>(sound->mHandle)->mChannel, false);
    }

    void OpenALOutput::pauseActiveDevice()
    {
        if (mInitialized)
            ndspSetMasterVol(0.f);
    }

    void OpenALOutput::resumeActiveDevice()
    {
        if (mInitialized)
            ndspSetMasterVol(1.f);
    }

    float OpenALOutput::getTimeScaledPitch(SoundBase* sound)
    {
        const bool shouldScale = !(sound->mParams.mFlags & PlayMode::NoScaling);
        return shouldScale ? sound->getPitch() * mManager.getSimulationTimeScale() : sound->getPitch();
    }
}
