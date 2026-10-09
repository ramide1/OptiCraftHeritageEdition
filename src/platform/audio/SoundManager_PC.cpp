#include "net/minecraft/src/SoundManager.h"
#include "platform/Log.h"

#if defined(NO_SOUND)


void *SoundManager::sndSystem = nullptr;
bool  SoundManager::loaded    = false;

SoundManager::SoundManager()
    : soundPoolSounds(), soundPoolStreaming(), soundPoolMusic(), soundVolume(0),
      options(nullptr), rand(), ticksBeforeMusic(0)
{
}

void SoundManager::loadSoundSettings(GameSettings *gamesettings) { options = gamesettings; loaded = false; }
void SoundManager::onSoundOptionsChanged() {}
void SoundManager::closeMinecraft() { loaded = false; }
void SoundManager::addSound(const jstring &s, const std::string &file) { soundPoolSounds.addSound(s, file); }
void SoundManager::addStreaming(const jstring &s, const std::string &file) { soundPoolStreaming.addSound(s, file); }
void SoundManager::addMusic(const jstring &s, const std::string &file) { soundPoolMusic.addSound(s, file); }
void SoundManager::playRandomMusicIfReady() {}
bool SoundManager::playMusicFileNow(const std::string &) { return false; }
void SoundManager::setListenerPosition(EntityLiving *, float) {}
void SoundManager::playStreaming(const jstring &, float, float, float, float, float) {}
void SoundManager::playSound(const jstring &, float, float, float, float, float) {}
void SoundManager::playSoundFX(const jstring &, float, float) {}
void SoundManager::tryToSetLibraryAndCodecs() {}


#else

#include "net/minecraft/src/SoundManager.h"

#include <cmath>
#include <iostream>
#include <vector>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdlib>

#include "AL/al.h"
#include "AL/alc.h"
#include "AL/alext.h"

#include "platform/Resources.h"
#include "platform/audio/Ps2AdpcmStreamDecoder.h"
#include "platform/audio/AdpAssetDecode.h"
#include "platform/audio/AudioConvert.h"
#include "pc/audio/PcMusicStream.h"

#include "net/minecraft/src/GameSettings.h"
#include "net/minecraft/src/EntityLiving.h"
#include "net/minecraft/src/MathHelper.h"
#include "net/minecraft/src/SoundPoolEntry.h"
#include "platform/audio/AudioAssetFormat.h"
#include "platform/audio/AudioSpatialization.h"

// ─── Internal audio mixer ────────────────────────────────────────────────────
//
// Asset formats (2026-10 unification): the desktop target consumes the same
// pack shapes the 3DS backend registers, i.e. the tree scripts/ogg to * stages:
//
//   * one-shot SFX come as .adp (SPU2-ADPCM, adpenc output) only -- the PS2's
//     own rule -- decoded here with the shared portable decoder
//     (platform/audio/Ps2AdpcmStreamDecoder.*);
//   * records/music arrive as raw 22050 Hz stereo s16le .pcm (scripts/ogg to
//     pcm), .adp (an all-adp pack, scripts/ogg to adp over the whole tree) or
//     the third script shape, headered RIFF .wav (scripts/ogg to wav).
//
// Runtime vorbis decode -- the expensive path the consoles cannot afford --
// leaves the desktop target entirely, the same switch the 3DS backend made
// when it moved from OGG to ADP and the Wii backend makes with it.
// (src/pc/external/stb_vorbis.* stays in the repo untouched; no backend
// decodes Vorbis at runtime anymore.)
//
// Output device: OpenAL (the vendored openal-soft). The app keeps its own
// software mixer verbatim (32 channels, Sound/Music/Streaming buses and
// listener attenuation from AudioSpatialization.h -- the same math the
// console backends share), and the mixed float-stereo 44100 Hz stream is
// pushed into ONE flat 2D OpenAL source through a 4x1024-frame buffer
// queue refilled by a small worker thread. No AL 3D positioning is used,
// so distance semantics stay bit-identical with the other platforms.

static constexpr int SAMPLE_RATE       = 44100;
static constexpr int CHANNELS          = 2;   // stereo
static constexpr int MAX_SOUNDS        = 32;  // concurrent sound slots
static constexpr int AL_BUFFER_FRAMES  = 1024;
static constexpr int AL_BUFFER_COUNT   = 4;   // ~93 ms of buffered audio

enum class AudioBus
{
    Sound,
    Music,
    Streaming
};

struct AudioChannel
{
    std::vector<float> pcm;    // interleaved L/R samples, normalised [-1,1]
    size_t pos     = 0;
    bool   active  = false;
    bool   looping = false;
    float  volume  = 1.0f;
    float  pitch   = 1.0f;   // resampling not yet implemented; stored for future use
    AudioBus bus   = AudioBus::Sound;
};

static ALCdevice        *s_device   = nullptr;
static ALCcontext       *s_context  = nullptr;
static ALuint            s_source   = 0;
static ALuint            s_buffers[AL_BUFFER_COUNT] = {};
static bool              s_floatFrames = false; // AL_EXT_float32 present
static std::thread       s_mixerThread;
static std::atomic<bool> s_mixerRun{false};
static AudioChannel      s_channels[MAX_SOUNDS];
static std::mutex        s_mutex;
static float             s_soundVolume   = 1.0f;
static float             s_musicVolume   = 1.0f;
static int               s_musicChannel  = -1; // index into s_channels, or -1
static int               s_streamingChannel = -1;
static AudioListenerState s_listener;

// Whole-file SPU2-ADPCM decode -> float stereo at the mixer rate, through
// the shared planar decoder (platform/audio/AdpAssetDecode.h). The header's
// loop flag is ignored: a one-shot plays exactly samplesPerChannel frames.
static std::vector<float> decodeAdp(const std::string &path)
{
    unsigned int fileBytes = 0;
    unsigned char *fileData = PlatformResources::loadFile(path, &fileBytes);
    if (fileData == nullptr)
        return {};

    Ps2AdpcmStream::Header header{};
    std::vector<std::int16_t> interleaved;
    std::uint32_t frames = 0;
    if (AdpAssetDecode::probe(fileData, fileBytes, header))
        frames = AdpAssetDecode::decode(fileData, fileBytes, header, interleaved);
    std::free(fileData);
    if (frames == 0)
        return {};

    std::vector<float> out;
    AudioConvert::s16ToFloatStereo(interleaved.data(), frames,
                                   header.channels, header.sampleRate, SAMPLE_RATE, out);
    return out;
}

// Raw s16le mono 22050 with no header (scripts/ogg to pcm) -- the pack's
// music/streaming shape, same reading as the 3DS backend.
static std::vector<float> decodePcm(const std::string &path)
{
    unsigned int fileBytes = 0;
    unsigned char *fileData = PlatformResources::loadFile(path, &fileBytes);
    if (fileData == nullptr)
        return {};

    constexpr int kRawPcmRate = 22050;
    constexpr int kRawPcmChannels = 1;
    const size_t frames = fileBytes / (sizeof(std::int16_t) * kRawPcmChannels);

    std::vector<float> out;
    AudioConvert::s16ToFloatStereo(reinterpret_cast<const std::int16_t *>(fileData),
                                   frames, kRawPcmChannels, kRawPcmRate, SAMPLE_RATE, out);
    std::free(fileData);
    return out;
}

// Decode a RIFF/WAVE blob to float stereo at the mixer rate. Loads through
// PlatformResources so a pak entry parses the same way a loose file does.
static std::vector<float> decodeWav(const std::string &path)
{
    unsigned int fileBytes = 0;
    unsigned char *fileData = PlatformResources::loadFile(path, &fileBytes);
    if (fileData == nullptr)
        return {};

    std::vector<std::int16_t> interleaved;
    int channels = 0, rate = 0;
    const bool ok = AudioConvert::wavDecodeS16(fileData, fileBytes, interleaved, channels, rate);
    std::free(fileData);
    if (!ok)
        return {};

    std::vector<float> out;
    AudioConvert::s16ToFloatStereo(interleaved.data(), interleaved.size() / channels,
                                   channels, rate, SAMPLE_RATE, out);
    return out;
}

static std::vector<float> loadSound(const std::string &path, float pitch = 1.0f)
{
    if (audioPathHasExtension(path, ".adp"))
        return decodeAdp(path);
    if (audioPathHasExtension(path, ".pcm"))
        return decodePcm(path);
    if (audioPathHasExtension(path, ".wav"))
        return decodeWav(path);
    (void)pitch;
    return {};
}

// Play decoded PCM on the next free channel; returns channel index or -1
static int playPCM(std::vector<float> pcm, float volume, bool looping, AudioBus bus)
{
    if (pcm.empty()) return -1;
    std::lock_guard<std::mutex> lock(s_mutex);
    for (int i = 0; i < MAX_SOUNDS; i++)
    {
        if (!s_channels[i].active)
        {
            s_channels[i].pcm     = std::move(pcm);
            s_channels[i].pos     = 0;
            s_channels[i].active  = true;
            s_channels[i].looping = looping;
            s_channels[i].volume  = volume;
            s_channels[i].bus     = bus;
            return i;
        }
    }
    return -1;
}

static void stopChannel(int idx, AudioBus expectedBus)
{
    if (idx < 0 || idx >= MAX_SOUNDS) return;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_channels[idx].active || s_channels[idx].bus != expectedBus)
        return;
    s_channels[idx].active = false;
    s_channels[idx].pcm.clear();
    s_channels[idx].pos = 0;
}

// Mix all active channels plus the music stream into float32 stereo (this is
// the exact mixing contract the SDL audio callback ran before etapa 2).
static void mixFrames(float *out, int frames)
{
    std::memset(out, 0, static_cast<size_t>(frames) * CHANNELS * sizeof(float));

    float musicVolume = 1.0f;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        musicVolume = s_musicVolume;
        for (auto &ch : s_channels)
        {
            if (!ch.active) continue;
            float busVolume = ch.bus == AudioBus::Music ? s_musicVolume : s_soundVolume;
            float vol = ch.volume * busVolume;

            for (int f = 0; f < frames; f++)
            {
                if (ch.pos + 1 >= ch.pcm.size())
                {
                    if (ch.looping)
                        ch.pos = 0;
                    else
                    {
                        ch.active = false;
                        ch.pcm.clear();
                        break;
                    }
                }
                out[f * 2 + 0] += ch.pcm[ch.pos++] * vol;
                out[f * 2 + 1] += ch.pcm[ch.pos++] * vol;
            }
        }
    }

    PcMusicStream::mix(out, frames, musicVolume);

    // Soft clip to [-1, 1]
    for (int i = 0; i < frames * CHANNELS; i++)
    {
        if      (out[i] >  1.0f) out[i] =  1.0f;
        else if (out[i] < -1.0f) out[i] = -1.0f;
    }
}

// Refill one dequeued buffer with the next mix chunk.
static void fillBuffer(ALuint buffer)
{
    static thread_local std::vector<float> pcm;
    pcm.assign(static_cast<size_t>(AL_BUFFER_FRAMES) * CHANNELS, 0.0f);
    mixFrames(pcm.data(), AL_BUFFER_FRAMES);

    if (s_floatFrames)
    {
        alBufferData(buffer, AL_FORMAT_STEREO_FLOAT32, pcm.data(),
                     static_cast<ALsizei>(pcm.size() * sizeof(float)), SAMPLE_RATE);
        return;
    }

    // No AL_EXT_float32 (any system OpenAL that is not openal-soft): play the
    // same mix as 16-bit PCM instead.
    static thread_local std::vector<std::int16_t> pcm16;
    pcm16.resize(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i)
    {
        const float v = std::max(-1.0f, std::min(1.0f, pcm[i]));
        pcm16[i] = static_cast<std::int16_t>(v * 32767.0f);
    }
    alBufferData(buffer, AL_FORMAT_STEREO16, pcm16.data(),
                 static_cast<ALsizei>(pcm16.size() * sizeof(std::int16_t)), SAMPLE_RATE);
}

// Worker: keep the source's queue topped up and the source playing. OpenAL
// has no pull callback, so this thread fills the role the SDL audio-device
// callback played before; 4x1024 frames (~93 ms) make a 4 ms poll loop
// comfortably ahead of the drain rate.
static void mixerLoop()
{
    // openal-soft tracks the current context per thread.
    alcMakeContextCurrent(s_context);
    while (s_mixerRun.load(std::memory_order_relaxed))
    {
        ALint processed = 0;
        alGetSourcei(s_source, AL_BUFFERS_PROCESSED, &processed);
        while (processed-- > 0)
        {
            ALuint buffer = 0;
            alSourceUnqueueBuffers(s_source, 1, &buffer);
            fillBuffer(buffer);
            alSourceQueueBuffers(s_source, 1, &buffer);
        }

        ALint state = AL_STOPPED;
        alGetSourcei(s_source, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING)
        {
            ALint queued = 0;
            alGetSourcei(s_source, AL_BUFFERS_QUEUED, &queued);
            if (queued > 0)
                alSourcePlay(s_source);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

// Tear down whatever tryToSetLibraryAndCodecs() managed to set up; safe on a
// partial init (any mix of null handles counts as "nothing set").
static void shutdownOpenAL()
{
    s_mixerRun.store(false, std::memory_order_relaxed);
    if (s_mixerThread.joinable())
        s_mixerThread.join();

    // The mixer thread may have re-pointed the process-wide current context
    // (or its own thread-local one); make sure the teardown thread owns one.
    if (s_context != nullptr)
        alcMakeContextCurrent(s_context);

    if (s_source != 0)
    {
        alSourceStop(s_source);
        ALint queued = 0;
        alGetSourcei(s_source, AL_BUFFERS_QUEUED, &queued);
        while (queued-- > 0)
        {
            ALuint buffer = 0;
            alSourceUnqueueBuffers(s_source, 1, &buffer);
        }
        alDeleteSources(1, &s_source);
        s_source = 0;
    }
    if (s_buffers[0] != 0)
    {
        alDeleteBuffers(AL_BUFFER_COUNT, s_buffers);
        std::memset(s_buffers, 0, sizeof(s_buffers));
    }
    if (s_context != nullptr)
    {
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(s_context);
        s_context = nullptr;
    }
    if (s_device != nullptr)
    {
        alcCloseDevice(s_device);
        s_device = nullptr;
    }
    // SoundManager::sndSystem is reset by closeMinecraft(), which owns the
    // member access; this free function only releases the AL objects.
}

// ─── SoundManager static members ─────────────────────────────────────────────

void *SoundManager::sndSystem = nullptr;
bool  SoundManager::loaded    = false;

// ─── SoundManager implementation ─────────────────────────────────────────────

SoundManager::SoundManager() :
    soundPoolSounds(),
    soundPoolStreaming(),
    soundPoolMusic(),
    soundVolume(0),
    options(nullptr),
    rand(),
    ticksBeforeMusic(rand.nextInt(100) + 40)
{
}

void SoundManager::loadSoundSettings(GameSettings *gamesettings)
{
    soundPoolStreaming.motionX = false;
    options = gamesettings;

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_soundVolume = gamesettings ? gamesettings->soundVolume : 1.0f;
        s_musicVolume = gamesettings ? gamesettings->musicVolume : 1.0f;
    }

    if (!loaded && (gamesettings == nullptr || gamesettings->soundVolume != 0.0f || gamesettings->musicVolume != 0.0f))
        tryToSetLibraryAndCodecs();
}

void SoundManager::tryToSetLibraryAndCodecs()
{
    if (loaded) return;

#if !defined(AL_FORMAT_STEREO_FLOAT32)
    // alext.h guards AL_EXT_float32 constants behind the extension define;
    // the vendored openal-soft headers always provide them.
    MC_LOG_ERROR("audio", "AL headers missing AL_EXT_float32, cannot start\n");
    return;
#endif

    s_device = alcOpenDevice(nullptr);
    if (s_device == nullptr)
    {
        MC_LOG_ERROR("audio", "alcOpenDevice failed: no OpenAL device available\n");
        return;
    }

    const ALCint attrs[] = {
        ALC_FREQUENCY,      SAMPLE_RATE,
        ALC_STEREO_SOURCES, AL_BUFFER_COUNT,
        0
    };
    s_context = alcCreateContext(s_device, attrs);
    if (s_context == nullptr || !alcMakeContextCurrent(s_context))
    {
        MC_LOG_ERROR("audio", "alcCreateContext failed (err=0x%x)\n", alcGetError(s_device));
        shutdownOpenAL();
        return;
    }

    // The mixer bakes distance attenuation itself; the single output stream
    // must not be re-attenuated by the AL distance model.
    alDistanceModel(AL_NONE);
    s_floatFrames = alIsExtensionPresent("AL_EXT_float32") == AL_TRUE;

    alGenSources(1, &s_source);
    alGenBuffers(AL_BUFFER_COUNT, s_buffers);
    if (s_source == 0 || s_buffers[0] == 0 || alGetError() != AL_NO_ERROR)
    {
        MC_LOG_ERROR("audio", "alGenSources/alGenBuffers failed\n");
        shutdownOpenAL();
        return;
    }

    // Prime the queue with a full round of silence/mix, then hand refills to
    // the worker thread.
    for (int i = 0; i < AL_BUFFER_COUNT; ++i)
    {
        fillBuffer(s_buffers[i]);
        alSourceQueueBuffers(s_source, 1, &s_buffers[i]);
    }
    if (alGetError() != AL_NO_ERROR)
    {
        MC_LOG_ERROR("audio", "alBufferData/alSourceQueueBuffers failed\n");
        shutdownOpenAL();
        return;
    }
    alSourcePlay(s_source);

    s_mixerRun.store(true, std::memory_order_relaxed);
    s_mixerThread = std::thread(mixerLoop);

    sndSystem = s_device; // the "OpenAL handle" the opaque pointer stands for
    loaded = true;
}

void SoundManager::onSoundOptionsChanged()
{
    if (!loaded && options && (options->soundVolume != 0.0f || options->musicVolume != 0.0f))
        tryToSetLibraryAndCodecs();

    if (!loaded) return;

    float musicVolume = options ? options->musicVolume : 1.0f;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_soundVolume = options ? options->soundVolume : 1.0f;
        s_musicVolume = musicVolume;
    }

    // Match Java: setting Music to zero stops only ambient background music.
    // Records/streaming remain on the Sound bus.
    if (musicVolume == 0.0f)
    {
        PcMusicStream::stop();
        if (s_musicChannel >= 0)
            stopChannel(s_musicChannel, AudioBus::Music);
    }
}

void SoundManager::closeMinecraft()
{
    PcMusicStream::stop();
    shutdownOpenAL();
    sndSystem = nullptr;

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (auto &ch : s_channels)
        {
            ch.active = false;
            ch.looping = false;
            ch.pos = 0;
            std::vector<float>().swap(ch.pcm);
        }
        s_musicChannel = -1;
        s_streamingChannel = -1;
    }

    loaded = false;
}

void SoundManager::addSound(const jstring &s, const std::string &file)
{
    // The PS2 rule for one-shots: .adp only (scripts/ogg to adp).
    if (audioPathHasExtension(file, ".adp"))
        soundPoolSounds.addSound(s, file);
}

void SoundManager::addStreaming(const jstring &s, const std::string &file)
{
    // Same trio the 3DS registers, so a PS2-staged pack works unmodified.
    if (audioPathHasExtension(file, ".adp") || audioPathHasExtension(file, ".pcm") ||
        audioPathHasExtension(file, ".wav"))
        soundPoolStreaming.addSound(s, file);
}

void SoundManager::addMusic(const jstring &s, const std::string &file)
{
    if (audioPathHasExtension(file, ".adp") || audioPathHasExtension(file, ".pcm") ||
        audioPathHasExtension(file, ".wav"))
        soundPoolMusic.addSound(s, file);
}

bool SoundManager::playMusicFileNow(const std::string &file)
{
    if (!loaded || !options || options->musicVolume == 0.0f)
        return false;

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_streamingChannel >= 0 && s_channels[s_streamingChannel].active
            && s_channels[s_streamingChannel].bus == AudioBus::Streaming)
            return false;
    }

    if (PcMusicStream::active())
        return false;

    if (audioPathHasExtension(file, ".adp") || audioPathHasExtension(file, ".pcm"))
    {
        if (s_musicChannel >= 0)
            stopChannel(s_musicChannel, AudioBus::Music);
        s_musicChannel = -1;
        if (!PcMusicStream::start(file))
            return false;
    }
    else
    {
        std::vector<float> pcm = loadSound(file);
        if (pcm.empty())
            return false;
        if (s_musicChannel >= 0)
            stopChannel(s_musicChannel, AudioBus::Music);
        const int channel = playPCM(std::move(pcm), 1.0f, false, AudioBus::Music);
        if (channel < 0)
            return false;
        s_musicChannel = channel;
    }

    ticksBeforeMusic = rand.nextInt(1200) + 600;
    return true;
}

void SoundManager::playRandomMusicIfReady()
{
    if (!loaded || !options || options->musicVolume == 0.0f)
        return;

    bool channelMusicPlaying = false;
    bool streamingPlaying = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        channelMusicPlaying = s_musicChannel >= 0 && s_channels[s_musicChannel].active
            && s_channels[s_musicChannel].bus == AudioBus::Music;
        streamingPlaying = s_streamingChannel >= 0 && s_channels[s_streamingChannel].active
            && s_channels[s_streamingChannel].bus == AudioBus::Streaming;
    }

    if (PcMusicStream::active() || channelMusicPlaying || streamingPlaying)
        return;

    if (ticksBeforeMusic > 0)
    {
        ticksBeforeMusic--;
        return;
    }

    SoundPoolEntry *entry = soundPoolMusic.getRandomSound();
    ticksBeforeMusic = rand.nextInt(1200) + 600;
    if (entry == nullptr)
        return;

    if (audioPathHasExtension(entry->soundUrl, ".adp") ||
        audioPathHasExtension(entry->soundUrl, ".pcm"))
    {
        PcMusicStream::start(entry->soundUrl);
        s_musicChannel = -1;
        return;
    }

    auto pcm = loadSound(entry->soundUrl);
    s_musicChannel = playPCM(std::move(pcm), 1.0f, false, AudioBus::Music);
}

void SoundManager::setListenerPosition(EntityLiving *entityliving, float f)
{
    if (!loaded || !options || options->soundVolume == 0.0f) return;
    updateAudioListener(s_listener, entityliving, f);
}

void SoundManager::playStreaming(const jstring &s, float f, float f1, float f2, float f3, float f4)
{
    if (!loaded || !options || (options->soundVolume == 0.0f && !s.empty())) return;

    // Java uses a distinct source named "streaming".
    if (s_streamingChannel >= 0)
        stopChannel(s_streamingChannel, AudioBus::Streaming);
    s_streamingChannel = -1;

    if (s.empty()) return;

    SoundPoolEntry *entry = soundPoolStreaming.getRandomSoundFromSoundPool(s);
    if (entry != nullptr && f3 > 0.0f)
    {
        const float attenuation = audioStreamingAttenuation(s_listener, f, f1, f2);
        if (attenuation <= 0.0f)
            return;

        // A record/stream temporarily replaces ambient background music.
        PcMusicStream::stop();
        if (s_musicChannel >= 0)
            stopChannel(s_musicChannel, AudioBus::Music);
        s_musicChannel = -1;
        ticksBeforeMusic = rand.nextInt(1200) + 600;

        auto pcm = loadSound(entry->soundUrl, f4);
        s_streamingChannel = playPCM(std::move(pcm), 0.5f * attenuation, false, AudioBus::Streaming);
    }
}

void SoundManager::playSound(const jstring &s, float f, float f1, float f2, float f3, float f4)
{
    if (!loaded || !options || options->soundVolume == 0.0f) return;

    SoundPoolEntry *entry = soundPoolSounds.getRandomSoundFromSoundPool(s);
    if (entry != nullptr && f3 > 0.0f)
    {
        const float attenuation = audioSpatialAttenuation(s_listener, f, f1, f2, f3);
        if (attenuation <= 0.0f)
            return;

        const float vol = std::min(f3, 1.0f) * attenuation;
        auto pcm = loadSound(entry->soundUrl, f4);
        playPCM(std::move(pcm), vol, false, AudioBus::Sound);
    }
}

void SoundManager::playSoundFX(const jstring &s, float f, float f1)
{
    if (!loaded || !options || options->soundVolume == 0.0f) return;

    SoundPoolEntry *entry = soundPoolSounds.getRandomSoundFromSoundPool(s);
    if (entry != nullptr)
    {
        float vol = (f > 1.0f ? 1.0f : f) * 0.25f;
        auto pcm = loadSound(entry->soundUrl, f1);
        playPCM(std::move(pcm), vol, false, AudioBus::Sound);
    }
}

#endif
