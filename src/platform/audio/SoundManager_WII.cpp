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

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>
#include <malloc.h>

#include <asndlib.h>
#include <ogc/cache.h>
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>

#include <array>
#include <cstdint>

#include "net/minecraft/src/GameSettings.h"
#include "net/minecraft/src/EntityLiving.h"
#include "net/minecraft/src/SoundPoolEntry.h"
#include "platform/Resources.h"
#include "platform/audio/AdpAssetDecode.h"
#include "platform/audio/AudioAssetFormat.h"
#include "platform/audio/AudioSpatialization.h"
#include "platform/storage/AssetPak.h"
#include "wii/WiiEarlyInit.h"

namespace
{
static AudioListenerState s_listener;

// ---- One-shot sounds: decoded-PCM cache over voices 1..MAX_SND_VOICES-1 ----
//
// Voice 0 is reserved for music/streaming below. One-shots are .adp only --
// SPU2-ADPCM as ps2sdk's adpenc writes it (scripts/ogg to adp), decoded with
// the shared portable decoder the PS2 streamer uses
// (platform/audio/AdpAssetDecode.h over
// platform/audio/Ps2AdpcmStreamDecoder.* -- pure C++, part of the common
// platform sources every target compiles) -- the same asset shape the PS2
// and 3DS backends cache.
//
// Bounded because the shipped sound set decodes to several times its
// compressed size in 16-bit PCM -- caching all of it at once would compete
// directly with world data on a ~60 MB heap. 4 MB holds dozens of distinct
// sounds at once (typical decoded SFX is tens of KB), which is enough that
// the working set of a busy scene should stay resident; least-recently-used
// entries are evicted to make room for the rest. Buffers evicted while ASND
// still reads them remain part of this budget until their voices finish.
// Starting point taken from the same arithmetic WiiTuning.h uses elsewhere,
// not yet measured on hardware.
constexpr size_t WII_SFX_CACHE_BUDGET_BYTES = 4 * 1024 * 1024;

// Compressed-ADP gate before a one-shot decode, the same 1 MB ceiling the
// PS2 SFX path applies (PS2_MAX_CACHED_ADPCM_BYTES) and the 3DS mirrors: the
// only assets that big are the minutes-long ambient beds and loops, which no
// console loads as one-shots -- and which would decode past the whole budget
// above anyway.
constexpr long WII_SFX_MAX_FILE_BYTES = 1024 * 1024;

struct WiiSfxSample
{
    void  *pcm        = nullptr;   // memalign(32, ...), 16-bit PCM, native rate
    int    sizeBytes  = 0;
    int    format     = VOICE_MONO_16BIT;
    int    sampleRate = 44100;
    u64    lastUsed   = 0;
};

struct WiiPendingSfxFree
{
    void   *pcm       = nullptr;
    size_t  allocBytes = 0;
};

std::unordered_map<std::string, WiiSfxSample> s_wiiSfxCache;
std::unordered_set<std::string> s_wiiRejectedSfx;
size_t s_wiiSfxCacheBytes = 0;
size_t s_wiiSfxResidentBytes = 0;
std::vector<WiiPendingSfxFree> s_wiiPendingFree;
u64 s_wiiVoiceLastUsed[MAX_SND_VOICES] = {};

size_t wiiAlign32(size_t n) { return (n + 31u) & ~size_t(31u); }

bool wiiSfxBufferIsPlaying(void *ptr)
{
    for (int v = 1; v < MAX_SND_VOICES; v++)
        if (ASND_StatusVoice(v) != SND_UNUSED && ASND_TestPointer(v, ptr))
            return true;
    return false;
}

// A cache entry can be evicted while its voice is still ringing out (a later,
// unrelated sound triggers the eviction). Its buffer can't be freed until no
// voice is still reading it -- ASND_TestPointer() is exactly the primitive
// for that -- so evicted buffers wait here instead of being freed outright.
void wiiReclaimPendingFree()
{
    for (size_t i = 0; i < s_wiiPendingFree.size(); )
    {
        WiiPendingSfxFree &pending = s_wiiPendingFree[i];
        if (wiiSfxBufferIsPlaying(pending.pcm))
        {
            i++;
            continue;
        }
        free(pending.pcm);
        s_wiiSfxResidentBytes -= pending.allocBytes;
        s_wiiPendingFree[i] = s_wiiPendingFree.back();
        s_wiiPendingFree.pop_back();
    }
}

void wiiEvictOneSfx()
{
    if (s_wiiSfxCache.empty())
        return;

    auto oldest = s_wiiSfxCache.begin();
    for (auto it = s_wiiSfxCache.begin(); it != s_wiiSfxCache.end(); ++it)
        if (it->second.lastUsed < oldest->second.lastUsed)
            oldest = it;

    const size_t allocBytes = wiiAlign32((size_t)oldest->second.sizeBytes);
    s_wiiSfxCacheBytes -= allocBytes;
    if (wiiSfxBufferIsPlaying(oldest->second.pcm))
        s_wiiPendingFree.push_back({ oldest->second.pcm, allocBytes });
    else
    {
        free(oldest->second.pcm);
        s_wiiSfxResidentBytes -= allocBytes;
    }
    s_wiiSfxCache.erase(oldest);
}

bool wiiHasSfxRoom(size_t allocBytes)
{
    return s_wiiSfxResidentBytes <= WII_SFX_CACHE_BUDGET_BYTES &&
           allocBytes <= WII_SFX_CACHE_BUDGET_BYTES - s_wiiSfxResidentBytes;
}

bool wiiMakeSfxRoom(size_t allocBytes)
{
    wiiReclaimPendingFree();
    while (!wiiHasSfxRoom(allocBytes) && !s_wiiSfxCache.empty())
    {
        wiiEvictOneSfx();
        wiiReclaimPendingFree();
    }
    return wiiHasSfxRoom(allocBytes);
}

void wiiRememberRejectedSfx(const std::string &path)
{
    try
    {
        s_wiiRejectedSfx.insert(path);
    }
    catch (const std::bad_alloc &)
    {
        MC_LOG_INFO("wii", "[WII][AUDIO] could not memoize rejected SFX path=%s\n",
            path.c_str());
    }
}

void wiiClearSfxCache()
{
    for (int v = 1; v < MAX_SND_VOICES; v++)
        ASND_StopVoice(v);
    for (auto &kv : s_wiiSfxCache)
        free(kv.second.pcm);
    s_wiiSfxCache.clear();
    s_wiiRejectedSfx.clear();
    s_wiiSfxCacheBytes = 0;
    for (const WiiPendingSfxFree &pending : s_wiiPendingFree)
        free(pending.pcm);
    s_wiiPendingFree.clear();
    s_wiiSfxResidentBytes = 0;
}

bool wiiProbeAdp(const unsigned char *data, unsigned int size,
                   Ps2AdpcmStream::Header &header, size_t &decodedBytes)
{
    if (!AdpAssetDecode::probe(data, size, header))
        return false;

    const size_t values = (size_t)header.samplesPerChannel * (size_t)header.channels;
    if (header.channels <= 0 ||
        values / (size_t)header.channels != (size_t)header.samplesPerChannel ||
        values > (size_t)-1 / sizeof(std::int16_t))
        return false;

    decodedBytes = values * sizeof(std::int16_t);
    return true;
}

WiiSfxSample *wiiGetSfxSample(const std::string &path)
{
    auto it = s_wiiSfxCache.find(path);
    if (it != s_wiiSfxCache.end())
    {
        it->second.lastUsed = gettime();
        return &it->second;
    }

    if (s_wiiRejectedSfx.find(path) != s_wiiRejectedSfx.end())
        return nullptr;

    if (!audioPathHasExtension(path, ".adp"))
    {
        MC_LOG_INFO("wii", "[WII][AUDIO] unsupported SFX asset %s\n", path.c_str());
        wiiRememberRejectedSfx(path);
        return nullptr;
    }

    // The size gate runs off the directory entry, before the read -- the
    // PS2/3DS path's rule -- so a rejected bed costs no allocation. A
    // missing file is retried next play (mounts settle after boot); a
    // corrupt or oversized one is memoized, those properties are stable.
    const long fileBytes = PlatformResources::fileSize(path);
    if (fileBytes <= 0)
    {
        MC_LOG_INFO("wii", "[WII][AUDIO] missing SFX %s\n", path.c_str());
        return nullptr;
    }
    if (fileBytes > WII_SFX_MAX_FILE_BYTES)
    {
        MC_LOG_INFO("wii", "[WII][AUDIO] skipped oversized SFX file=%uKB budget=%uKB path=%s\n",
            (unsigned)(fileBytes / 1024u),
            (unsigned)(WII_SFX_MAX_FILE_BYTES / 1024u), path.c_str());
        wiiRememberRejectedSfx(path);
        return nullptr;
    }

    unsigned int size = 0;
    unsigned char *data = PlatformResources::loadFile(path, &size);
    if (data == nullptr)
    {
        MC_LOG_INFO("wii", "[WII][AUDIO] failed to read SFX %s\n", path.c_str());
        return nullptr;
    }

    Ps2AdpcmStream::Header header{};
    size_t probedBytes = 0;
    if (!wiiProbeAdp(data, size, header, probedBytes))
    {
        free(data);
        MC_LOG_INFO("wii", "[WII][AUDIO] failed to inspect SFX %s\n", path.c_str());
        wiiRememberRejectedSfx(path);
        return nullptr;
    }
    if (probedBytes > WII_SFX_CACHE_BUDGET_BYTES)
    {
        free(data);
        MC_LOG_INFO("wii", "[WII][AUDIO] skipped oversized SFX decoded=%uKB budget=%uKB path=%s\n",
            (unsigned)(probedBytes / 1024u),
            (unsigned)(WII_SFX_CACHE_BUDGET_BYTES / 1024u), path.c_str());
        wiiRememberRejectedSfx(path);
        return nullptr;
    }

    const size_t probedAllocBytes = wiiAlign32(probedBytes);
    if (!wiiMakeSfxRoom(probedAllocBytes))
    {
        free(data);
        MC_LOG_INFO("wii", "[WII][AUDIO] dropped SFX: cache busy request=%uKB resident=%uKB "
               "cached=%uKB pending=%u path=%s\n",
            (unsigned)(probedAllocBytes / 1024u),
            (unsigned)(s_wiiSfxResidentBytes / 1024u),
            (unsigned)(s_wiiSfxCacheBytes / 1024u),
            (unsigned)s_wiiPendingFree.size(), path.c_str());
        return nullptr;
    }

    // Whole-file ADP decode through the shared planar decoder (stereo files
    // keep each channel's blocks contiguous, left plane first, and come out
    // interleaved). The header's loop flag is ignored: a one-shot plays
    // exactly samplesPerChannel frames.
    std::vector<std::int16_t> pcm;
    const std::uint32_t decodedFrames = AdpAssetDecode::decode(data, size, header, pcm);
    free(data);
    if (decodedFrames == 0 || pcm.empty())
    {
        MC_LOG_INFO("wii", "[WII][AUDIO] failed to decode SFX %s\n", path.c_str());
        wiiRememberRejectedSfx(path);
        return nullptr;
    }

    const size_t rawBytes = pcm.size() * sizeof(std::int16_t);
    const size_t allocBytes = wiiAlign32(rawBytes);

    void *buf = memalign(32, allocBytes);
    if (!buf)
    {
        MC_LOG_INFO("wii", "[WII][AUDIO] SFX cache allocation failed bytes=%u resident=%u "
               "cached=%u path=%s\n",
            (unsigned)allocBytes, (unsigned)s_wiiSfxResidentBytes,
            (unsigned)s_wiiSfxCacheBytes, path.c_str());
        return nullptr;
    }
    std::memset(buf, 0, allocBytes);
    std::memcpy(buf, pcm.data(), rawBytes);
    // ASND/DSP reads through DMA, not the CPU cache. Flush the entire padded
    // allocation once after decoding; cached SFX are immutable afterwards.
    DCFlushRange(buf, allocBytes);

    WiiSfxSample *entry = nullptr;
    try
    {
        auto inserted = s_wiiSfxCache.emplace(path, WiiSfxSample{});
        if (!inserted.second)
        {
            free(buf);
            inserted.first->second.lastUsed = gettime();
            return &inserted.first->second;
        }
        entry = &inserted.first->second;
    }
    catch (const std::bad_alloc &)
    {
        free(buf);
        MC_LOG_INFO("wii", "[WII][AUDIO] SFX cache metadata allocation failed path=%s\n",
            path.c_str());
        return nullptr;
    }
    entry->pcm = buf;
    entry->sizeBytes = (int)rawBytes;
    entry->format = header.channels >= 2 ? VOICE_STEREO_16BIT : VOICE_MONO_16BIT;
    entry->sampleRate = header.sampleRate;
    entry->lastUsed = gettime();
    s_wiiSfxCacheBytes += allocBytes;
    s_wiiSfxResidentBytes += allocBytes;
    return entry;
}

// First unused voice among 1..15; if all 15 are busy, steal whichever was
// triggered longest ago rather than drop the new sound. Voice 0 is never a
// candidate -- it belongs to music/streaming below.
int wiiPickSfxVoice()
{
    for (int v = 1; v < MAX_SND_VOICES; v++)
        if (ASND_StatusVoice(v) == SND_UNUSED)
            return v;

    int oldest = 1;
    for (int v = 2; v < MAX_SND_VOICES; v++)
        if (s_wiiVoiceLastUsed[v] < s_wiiVoiceLastUsed[oldest])
            oldest = v;
    ASND_StopVoice(oldest);
    return oldest;
}

void wiiPlaySfx(const std::string &path, float volume)
{
    WiiSfxSample *sample = wiiGetSfxSample(path);
    if (!sample)
        return;

    int voice = wiiPickSfxVoice();
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    int vol = (int)(volume * (float)MAX_VOLUME + 0.5f);

    int r = ASND_SetVoice(voice, sample->format, sample->sampleRate, 0,
        sample->pcm, sample->sizeBytes, vol, vol, nullptr);
#if MC_LOG_LEVEL >= 2
    //MC_LOG_INFO("wii", "[WII][AUDIO] sfx '%s' voice=%d fmt=%d rate=%d bytes=%d vol=%d -> %d\n",
        //path.c_str(), voice, sample->format, sample->sampleRate, sample->sizeBytes, vol, r);
#else
    (void)r;
#endif
    s_wiiVoiceLastUsed[voice] = gettime();
}

// ---- Streaming: background music and the jukebox share voice 0 -------------
//
// Music/streaming tracks are minutes of audio -- tens of MB decoded, which
// does not fit next to a loaded world -- so they stream incrementally and
// never sit decoded in RAM the way the SFX cache does. This double-buffers
// ~4096-frame chunks through a background LWP thread modelled on devkitPro's
// own oggplayer example (examples/wii/audio/oggplayer). Simplified relative
// to that example: a 1 ms poll instead of an LWP_ThreadSleep/Signal
// rendezvous -- easier to reason about without hardware to test the handshake
// on, at the cost of the producer thread waking slightly more often than
// strictly necessary.
//
// The streamed bytes are the same three shapes the PS2/3DS backends consume,
// so a pack staged for those consoles plays here unmodified:
//
//   * .adp  SPU2-ADPCM exactly as ps2sdk's adpenc writes it (scripts/ogg to
//     adp, mono 22050), decoded in whole 16-byte blocks through the shared
//     portable decoder;
//   * .pcm  raw s16le with no header (scripts/ogg to pcm writes -ac 1, so
//     mono 22050 -- the 3DS backend's CTR_RAW_PCM_SAMPLE_RATE/
//     CTR_RAW_PCM_CHANNELS interpretation, which the file durations also
//     support);
//   * .wav  RIFF/WAVE with a real header (scripts/ogg to wav, mono 22050),
//     so the rate and channels come from the fmt chunk instead of a
//     convention.
constexpr int WII_STREAM_FRAMES = 4096;

enum class WiiStreamKind
{
    None,
    Music,
    Streaming
};

alignas(32) short s_wiiStreamBuf[2][WII_STREAM_FRAMES * 2];
volatile int  s_wiiStreamFrames[2] = { 0, 0 };
volatile int  s_wiiStreamBytes[2]  = { 0, 0 };
volatile bool s_wiiStreamReady[2]  = { false, false };

// Sequential read-only file for the Wii audio stream thread. stdio-based
// (libfat/newlib fopen), pak-aware like the 3DS's DsStreamFile: a "pak://"
// path opens the pak itself on a handle of its own and confines every offset
// to the entry's byte range, so the stream thread never shares a file
// position with the main thread's loader. Position is tracked explicitly so
// a read past the entry's end fails instead of spilling into the next entry.
class WiiStreamFile
{
public:
    WiiStreamFile() = default;
    ~WiiStreamFile() { close(); }
    WiiStreamFile(const WiiStreamFile &) = delete;
    WiiStreamFile &operator=(const WiiStreamFile &) = delete;

    bool open(const char *path)
    {
        close();
        if (path == nullptr)
            return false;

        const std::string spelled(path);
        if (AssetPak::isPakPath(spelled))
        {
            std::uint32_t dataOffset = 0;
            std::uint32_t entrySize = 0;
            if (!AssetPak::locate(AssetPak::keyOf(spelled), &dataOffset, &entrySize))
                return false;
            file_ = std::fopen(AssetPak::archivePath().c_str(), "rb");
            if (file_ == nullptr)
                return false;
            base_ = static_cast<long>(dataOffset);
            size_ = static_cast<long>(entrySize);
        }
        else
        {
            file_ = std::fopen(path, "rb");
            if (file_ == nullptr)
                return false;
            if (std::fseek(file_, 0, SEEK_END) != 0)
            {
                close();
                return false;
            }
            const long end = std::ftell(file_);
            if (end < 0)
            {
                close();
                return false;
            }
            base_ = 0;
            size_ = end;
        }
        return seek(0);
    }

    void close()
    {
        if (file_ != nullptr)
            std::fclose(file_);
        file_ = nullptr;
        base_ = 0;
        size_ = 0;
        pos_ = 0;
    }

    bool isOpen() const { return file_ != nullptr; }
    long size() const { return file_ != nullptr ? size_ : -1L; }

    bool seek(long offset)
    {
        if (file_ == nullptr || offset < 0 || offset > size_)
            return false;
        if (std::fseek(file_, base_ + offset, SEEK_SET) != 0)
            return false;
        pos_ = offset;
        return true;
    }

    bool readExact(void *dst, int bytes)
    {
        if (file_ == nullptr || dst == nullptr || bytes < 0)
            return false;
        if (bytes == 0)
            return true;
        if (pos_ + bytes > size_)
            return false;
        if (std::fread(dst, 1, static_cast<size_t>(bytes), file_) != static_cast<size_t>(bytes))
            return false;
        pos_ += bytes;
        return true;
    }

private:
    std::FILE *file_ = nullptr;
    long base_ = 0; // byte offset of the entry inside the handle (0 for a loose file)
    long size_ = 0; // bytes readable from base_
    long pos_ = 0;  // next byte to read, relative to base_
};

// The stream's source format: which of the PS2's three asset shapes the open
// file carries. `Adp` walks 16-byte blocks through the shared decoder; the
// other two are already 16-bit PCM on disk and a fill is a plain read.
enum class WiiStreamFormat
{
    Adp,
    RawPcm,
    Wav
};

// Parsed from a RIFF/WAVE header, the same fields the PS2/3DS streamers
// carry (and the same chunk walk their probes perform).
struct WiiWavStreamInfo
{
    int channels = 0;
    int sampleRate = 0;
    long dataOffset = 0;
    std::uint32_t dataBytes = 0;
};

// The 3DS raw-PCM interpretation: scripts/ogg to pcm writes headerless
// s16le mono, so the rate and channel count are conventions, not file
// contents. These are the constants the 3DS backend plays .pcm files with
// (CTR_RAW_PCM_SAMPLE_RATE/CTR_RAW_PCM_CHANNELS); this backend follows it
// so music sounds the same on every platform.
constexpr int WII_RAW_PCM_SAMPLE_RATE = 22050;
constexpr int WII_RAW_PCM_CHANNELS = 1;

std::uint16_t wiiReadLe16(const std::uint8_t *data)
{
    return static_cast<std::uint16_t>(data[0]) |
           (static_cast<std::uint16_t>(data[1]) << 8);
}

std::uint32_t wiiReadLe32(const std::uint8_t *data)
{
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

WiiStreamFile s_wiiStreamLeft;
WiiStreamFile s_wiiStreamRight;
WiiStreamFormat s_wiiStreamFormat = WiiStreamFormat::Adp;
WiiWavStreamInfo s_wiiStreamWav{};
Ps2AdpcmStream::Header s_wiiStreamAdpHeader{};
Ps2AdpcmStream::ChannelState s_wiiStreamLeftState{};
Ps2AdpcmStream::ChannelState s_wiiStreamRightState{};
std::uint32_t s_wiiStreamFramesDecoded = 0; // frames pulled through decodeStreamFrames
std::uint32_t s_wiiStreamFramesTotal = 0;   // raw/wav: whole frames the source holds
std::string s_wiiStreamPath;
int   s_wiiStreamChannels          = 0;
int   s_wiiStreamSampleRate        = 44100;
float s_wiiStreamVolume            = 1.0f;
WiiStreamKind s_wiiStreamKind      = WiiStreamKind::None;
volatile bool s_wiiStreamEof       = false;
volatile bool s_wiiStreamRunning   = false;
volatile u32  s_wiiStreamStarves   = 0;
volatile u32  s_wiiStreamAddBusy   = 0;

lwp_t  s_wiiStreamThread = LWP_THREAD_NULL;
u8     s_wiiStreamStack[16384] __attribute__((aligned(8)));

// ADP asset validation: the header must parse (APCM magic, supported
// rate/channels) and the file must hold every block the header promises --
// the same check the PS2/3DS streamers apply. `data` is the 16-byte header
// probe, `size` the whole file's.
bool wiiValidAdpHeader(const std::uint8_t *data, std::size_t size,
                       Ps2AdpcmStream::Header &header)
{
    if (!Ps2AdpcmStream::parseHeader(data, size, header))
        return false;

    const std::uint64_t blocks =
        (static_cast<std::uint64_t>(header.samplesPerChannel) +
         Ps2AdpcmStream::kSamplesPerBlock - 1u) /
        static_cast<std::uint64_t>(Ps2AdpcmStream::kSamplesPerBlock);
    const std::uint64_t required = Ps2AdpcmStream::kHeaderBytes +
        blocks * Ps2AdpcmStream::kBlockBytes * static_cast<std::uint64_t>(header.channels);
    return required <= static_cast<std::uint64_t>(size);
}

int wiiStreamChannelCount()
{
    switch (s_wiiStreamFormat)
    {
    case WiiStreamFormat::Adp:    return s_wiiStreamAdpHeader.channels;
    case WiiStreamFormat::Wav:    return s_wiiStreamWav.channels;
    case WiiStreamFormat::RawPcm: return WII_RAW_PCM_CHANNELS;
    }
    return 1;
}

int wiiStreamSampleRate()
{
    switch (s_wiiStreamFormat)
    {
    case WiiStreamFormat::Adp:    return s_wiiStreamAdpHeader.sampleRate;
    case WiiStreamFormat::Wav:    return s_wiiStreamWav.sampleRate;
    case WiiStreamFormat::RawPcm: return WII_RAW_PCM_SAMPLE_RATE;
    }
    return WII_RAW_PCM_SAMPLE_RATE;
}

// The RIFF chunk walk, the same one the PS2/3DS streamers perform: every
// chunk header is read from an absolute cursor so the position never needs a
// tell(), the fmt chunk decides whether the file is playable (PCM s16le,
// mono/stereo), and the data chunk's offset/size are what the stream reads
// back. `file` is expected to be open at RIFF start; it is left open either
// way, seeked to the data payload on success.
bool wiiProbeWavStream(WiiStreamFile &file, WiiWavStreamInfo &info)
{
    std::array<std::uint8_t, 12> riff{};
    if (!file.readExact(riff.data(), static_cast<int>(riff.size())) ||
        std::memcmp(riff.data(), "RIFF", 4) != 0 ||
        std::memcmp(riff.data() + 8, "WAVE", 4) != 0)
        return false;

    long cursor = static_cast<long>(riff.size());
    bool haveFormat = false;
    bool haveData = false;
    while (!haveData)
    {
        std::array<std::uint8_t, 8> chunk{};
        if (!file.seek(cursor) || !file.readExact(chunk.data(), static_cast<int>(chunk.size())))
            break;

        const std::uint32_t chunkSize = wiiReadLe32(chunk.data() + 4);
        const long payloadOffset = cursor + static_cast<long>(chunk.size());

        if (std::memcmp(chunk.data(), "fmt ", 4) == 0)
        {
            if (chunkSize < 16)
                break;

            std::array<std::uint8_t, 16> format{};
            if (!file.readExact(format.data(), static_cast<int>(format.size())))
                break;

            const int encoding = static_cast<int>(wiiReadLe16(format.data()));
            info.channels = static_cast<int>(wiiReadLe16(format.data() + 2));
            info.sampleRate = static_cast<int>(wiiReadLe32(format.data() + 4));
            const int blockAlign = static_cast<int>(wiiReadLe16(format.data() + 12));
            const int bits = static_cast<int>(wiiReadLe16(format.data() + 14));
            haveFormat = encoding == 1 && bits == 16 &&
                         (info.channels == 1 || info.channels == 2) &&
                         blockAlign == info.channels * static_cast<int>(sizeof(std::int16_t));
            if (!haveFormat)
                break;
        }
        else if (std::memcmp(chunk.data(), "data", 4) == 0)
        {
            info.dataOffset = payloadOffset;
            info.dataBytes = chunkSize;
            haveData = true;
        }

        cursor = payloadOffset + static_cast<long>(chunkSize) +
                 static_cast<long>(chunkSize & 1u);
    }

    if (!haveFormat || !haveData || info.sampleRate <= 0 || info.dataBytes == 0 ||
        !file.seek(info.dataOffset))
        return false;
    return true;
}

// Opens the stream's source and probes which of the three asset shapes it
// is. On success the reader(s) are open and positioned at the first playable
// byte: after the APCM header (with the right plane seeked for stereo ADP),
// at offset 0 for raw PCM, at the data payload for WAV.
bool wiiOpenAudioStream(const std::string &path)
{
    s_wiiStreamLeft.close();
    s_wiiStreamRight.close();
    s_wiiStreamFormat = WiiStreamFormat::Adp;
    s_wiiStreamWav = WiiWavStreamInfo{};
    s_wiiStreamFramesTotal = 0;

    if (audioPathHasExtension(path, ".pcm"))
    {
        // Headerless s16le mono 22050: the frame count is the whole file's
        // worth of samples (an odd trailing byte is dropped rather than read
        // as a half sample).
        if (!s_wiiStreamLeft.open(path.c_str()))
            return false;
        const long size = s_wiiStreamLeft.size();
        if (size < WII_RAW_PCM_CHANNELS * static_cast<int>(sizeof(std::int16_t)))
            return false;
        s_wiiStreamFormat = WiiStreamFormat::RawPcm;
        s_wiiStreamFramesTotal = static_cast<std::uint32_t>(
            size / (WII_RAW_PCM_CHANNELS * static_cast<int>(sizeof(std::int16_t))));
        return true;
    }

    if (audioPathHasExtension(path, ".wav"))
    {
        if (!s_wiiStreamLeft.open(path.c_str()))
            return false;
        WiiWavStreamInfo wav{};
        if (!wiiProbeWavStream(s_wiiStreamLeft, wav))
            return false;
        s_wiiStreamFormat = WiiStreamFormat::Wav;
        s_wiiStreamWav = wav;
        s_wiiStreamFramesTotal = wav.dataBytes /
            (static_cast<std::uint32_t>(wav.channels) * sizeof(std::int16_t));
        return s_wiiStreamFramesTotal > 0;
    }

    // ADP: the left plane starts right after the 16-byte header, the right
    // plane (stereo only) after the left plane's blocks -- the planar layout
    // the shared decoder walks for one-shots above.
    Ps2AdpcmStream::Header &header = s_wiiStreamAdpHeader;
    std::array<std::uint8_t, Ps2AdpcmStream::kHeaderBytes> bytes{};

    if (!s_wiiStreamLeft.open(path.c_str()) ||
        !s_wiiStreamLeft.readExact(bytes.data(), static_cast<int>(bytes.size())))
        return false;

    const long streamSize = s_wiiStreamLeft.size();
    if (streamSize < 0 ||
        !wiiValidAdpHeader(bytes.data(), static_cast<std::size_t>(streamSize), header))
        return false;

    const std::uint32_t totalBlocks =
        (header.samplesPerChannel + Ps2AdpcmStream::kSamplesPerBlock - 1u) /
        Ps2AdpcmStream::kSamplesPerBlock;
    if (!s_wiiStreamLeft.seek(Ps2AdpcmStream::kHeaderBytes))
        return false;
    if (header.channels == 2)
    {
        const long rightOffset = Ps2AdpcmStream::kHeaderBytes +
            static_cast<long>(totalBlocks) * Ps2AdpcmStream::kBlockBytes;
        if (!s_wiiStreamRight.open(path.c_str()) || !s_wiiStreamRight.seek(rightOffset))
            return false;
    }
    return true;
}

void wiiCloseAudioStream()
{
    s_wiiStreamLeft.close();
    s_wiiStreamRight.close();
}

// Raw-PCM and WAV fills: the bytes on disk are already interleaved s16le
// PCM, so a fill is a plain read -- no decoder state, no per-channel
// readers. Returns the frame count read, 0 at end of stream, negative on a
// read failure.
int wiiDecodePcmStreamFrames(std::int16_t *out, int framesWanted)
{
    const int channels = wiiStreamChannelCount();
    const std::uint32_t framesLeft = s_wiiStreamFramesTotal - s_wiiStreamFramesDecoded;
    if (framesLeft == 0)
        return 0;
    const int frames = static_cast<int>(std::min<std::uint32_t>(
        static_cast<std::uint32_t>(framesWanted), framesLeft));
    const int bytes = frames * channels * static_cast<int>(sizeof(std::int16_t));
    if (!s_wiiStreamLeft.readExact(out, bytes))
        return -1;
    s_wiiStreamFramesDecoded += static_cast<std::uint32_t>(frames);
    return frames;
}

// Pulls the next framesWanted interleaved frames off the stream source. The
// ADP path walks whole 16-byte blocks: a buffer fills to the largest whole
// multiple of 28 frames that fits (4096 -> 4088), which keeps the
// per-channel decoder state and the two block readers in lockstep across
// refill calls. The PCM paths read whole frames directly. Returns the frame
// count written, 0 at end of stream, negative on a read/decode failure.
int wiiDecodeStreamFrames(std::int16_t *out, int framesWanted)
{
    if (s_wiiStreamFormat != WiiStreamFormat::Adp)
        return wiiDecodePcmStreamFrames(out, framesWanted);

    const Ps2AdpcmStream::Header &header = s_wiiStreamAdpHeader;
    const bool stereo = header.channels == 2;
    std::array<std::uint8_t, Ps2AdpcmStream::kBlockBytes> block{};
    std::array<std::int16_t, Ps2AdpcmStream::kSamplesPerBlock> leftPcm{};
    std::array<std::int16_t, Ps2AdpcmStream::kSamplesPerBlock> rightPcm{};

    int frames = 0;
    while (frames + Ps2AdpcmStream::kSamplesPerBlock <= framesWanted &&
           s_wiiStreamFramesDecoded < header.samplesPerChannel)
    {
        if (!s_wiiStreamLeft.readExact(block.data(), Ps2AdpcmStream::kBlockBytes) ||
            !Ps2AdpcmStream::decodeBlock(block.data(), s_wiiStreamLeftState, leftPcm.data()))
            return -1;
        if (stereo)
        {
            if (!s_wiiStreamRight.readExact(block.data(), Ps2AdpcmStream::kBlockBytes) ||
                !Ps2AdpcmStream::decodeBlock(block.data(), s_wiiStreamRightState, rightPcm.data()))
                return -1;
        }

        const std::uint32_t framesLeft = header.samplesPerChannel - s_wiiStreamFramesDecoded;
        const std::uint32_t blockFrames =
            std::min<std::uint32_t>(Ps2AdpcmStream::kSamplesPerBlock, framesLeft);
        for (std::uint32_t frame = 0; frame < blockFrames; ++frame)
        {
            out[static_cast<std::size_t>(frames) * header.channels] = leftPcm[frame];
            if (stereo)
                out[static_cast<std::size_t>(frames) * header.channels + 1] = rightPcm[frame];
            ++frames;
        }
        s_wiiStreamFramesDecoded += blockFrames;
    }
    return frames;
}

void wiiStreamCallback(s32 voice)
{
    if (!s_wiiStreamRunning)
    {
        ASND_StopVoice(voice);
        return;
    }
    for (int slot = 0; slot < 2; slot++)
    {
        if (s_wiiStreamReady[slot])
        {
            const int result = ASND_AddVoice(voice, s_wiiStreamBuf[slot],
                s_wiiStreamBytes[slot]);
            if (result == SND_OK)
                s_wiiStreamReady[slot] = false;
            else if (result == SND_BUSY)
                ++s_wiiStreamAddBusy;
            return;
        }
    }
    // Producer fell behind (or the track just ended): nothing to hand off.
    // With a callback ASND remains in SND_WAITING and calls us again; the worker
    // can therefore publish the next buffer without restarting the whole track.
    if (!s_wiiStreamEof)
        ++s_wiiStreamStarves;
}

void *wiiStreamThreadMain(void *)
{
    if (!wiiOpenAudioStream(s_wiiStreamPath))
    {
        MC_LOG_INFO("wii", "[WII][AUDIO] failed to open stream %s\n",
               s_wiiStreamPath.c_str());
        s_wiiStreamRunning = false;
        return nullptr;
    }

    s_wiiStreamLeftState = Ps2AdpcmStream::ChannelState{};
    s_wiiStreamRightState = Ps2AdpcmStream::ChannelState{};
    s_wiiStreamFramesDecoded = 0;
    s_wiiStreamChannels = wiiStreamChannelCount();
    s_wiiStreamSampleRate = wiiStreamSampleRate();
    MC_LOG_INFO("wii", "[WII][AUDIO] streaming %s chans=%d rate=%d vol=%.2f\n",
           s_wiiStreamPath.c_str(), s_wiiStreamChannels,
           s_wiiStreamSampleRate, s_wiiStreamVolume);

    while (s_wiiStreamRunning)
    {
        // ready=false only means the buffer was submitted. ASND owns it until
        // TestPointer says otherwise; this is the ownership test used by the
        // official libogc oggplayer and prevents decoding over live DMA data.
        for (int slot = 0; slot < 2; ++slot)
        {
            if (!s_wiiStreamReady[slot] && s_wiiStreamFrames[slot] > 0
                && ASND_TestPointer(0, s_wiiStreamBuf[slot]) == 0)
            {
                s_wiiStreamFrames[slot] = 0;
                s_wiiStreamBytes[slot] = 0;
            }
        }

        for (int fillSlot = 0; fillSlot < 2 && !s_wiiStreamEof; ++fillSlot)
        {
            if (s_wiiStreamReady[fillSlot] || s_wiiStreamFrames[fillSlot] != 0)
                continue;

            const int got = wiiDecodeStreamFrames(
                reinterpret_cast<std::int16_t *>(s_wiiStreamBuf[fillSlot]),
                WII_STREAM_FRAMES);

            if (got <= 0)
            {
                if (got < 0)
                    MC_LOG_INFO("wii", "[WII][AUDIO] stream read failed %s\n",
                           s_wiiStreamPath.c_str());
                s_wiiStreamEof = true;
            }
            else
            {
                const size_t rawBytes = (size_t)got * (size_t)s_wiiStreamChannels * sizeof(short);
                const size_t dmaBytes = wiiAlign32(rawBytes);
                if (dmaBytes > rawBytes)
                    std::memset(reinterpret_cast<byte_t *>(s_wiiStreamBuf[fillSlot]) + rawBytes,
                                0, dmaBytes - rawBytes);
                DCFlushRange(s_wiiStreamBuf[fillSlot], dmaBytes);
                s_wiiStreamFrames[fillSlot] = got;
                s_wiiStreamBytes[fillSlot] = (int)dmaBytes;
                s_wiiStreamReady[fillSlot] = true;
            }
        }

        // Initial start and recovery from an actually stopped voice happen here.
        // Normal starvation leaves a callback voice in SND_WAITING; once a
        // buffer becomes ready the repeatedly invoked callback chains it.
        if (ASND_StatusVoice(0) == SND_UNUSED && s_wiiStreamRunning)
        {
            for (int slot = 0; slot < 2; ++slot)
            {
                if (!s_wiiStreamReady[slot])
                    continue;
                int fmt = s_wiiStreamChannels >= 2 ? VOICE_STEREO_16BIT : VOICE_MONO_16BIT;
                int vol = (int)(s_wiiStreamVolume * (float)MAX_VOLUME + 0.5f);
                ASND_SetVoice(0, fmt, s_wiiStreamSampleRate, 0,
                    s_wiiStreamBuf[slot], s_wiiStreamBytes[slot],
                    vol, vol, wiiStreamCallback);
                s_wiiStreamReady[slot] = false;
                break;
            }
        }

        if (s_wiiStreamEof && !s_wiiStreamReady[0] && !s_wiiStreamReady[1]
            && s_wiiStreamFrames[0] == 0 && s_wiiStreamFrames[1] == 0)
        {
            // A callback voice intentionally remains SND_WAITING when starved;
            // at true EOF no buffer will ever arrive, so end it explicitly.
            ASND_StopVoice(0);
            s_wiiStreamRunning = false;
            break;
        }

        usleep(1000);
    }

    wiiCloseAudioStream();
    return nullptr;
}

void wiiStopStream()
{
    if (!s_wiiStreamRunning && s_wiiStreamThread == LWP_THREAD_NULL)
        return;

    s_wiiStreamRunning = false;
    ASND_StopVoice(0);

    if (s_wiiStreamThread != LWP_THREAD_NULL)
    {
        LWP_JoinThread(s_wiiStreamThread, nullptr);
        s_wiiStreamThread = LWP_THREAD_NULL;
    }
    s_wiiStreamReady[0] = false;
    s_wiiStreamReady[1] = false;
    s_wiiStreamFrames[0] = 0;
    s_wiiStreamFrames[1] = 0;
    s_wiiStreamBytes[0] = 0;
    s_wiiStreamBytes[1] = 0;
    wiiCloseAudioStream();
    s_wiiStreamFormat = WiiStreamFormat::Adp;
    s_wiiStreamWav = WiiWavStreamInfo{};
    s_wiiStreamAdpHeader = Ps2AdpcmStream::Header{};
    s_wiiStreamFramesDecoded = 0;
    s_wiiStreamFramesTotal = 0;
    s_wiiStreamPath.clear();
    s_wiiStreamKind = WiiStreamKind::None;
}

bool wiiValidateStream(const std::string &path)
{
    if (audioPathHasExtension(path, ".pcm"))
    {
        WiiStreamFile file;
        if (!file.open(path.c_str()))
            return false;
        return file.size() >=
            WII_RAW_PCM_CHANNELS * static_cast<long>(sizeof(std::int16_t));
    }

    if (audioPathHasExtension(path, ".wav"))
    {
        WiiStreamFile file;
        if (!file.open(path.c_str()))
            return false;
        WiiWavStreamInfo info{};
        return wiiProbeWavStream(file, info);
    }

    if (!audioPathHasExtension(path, ".adp"))
        return false;

    WiiStreamFile file;
    if (!file.open(path.c_str()))
        return false;
    const long streamSize = file.size();
    if (streamSize <= 0)
        return false;
    std::array<std::uint8_t, Ps2AdpcmStream::kHeaderBytes> bytes{};
    if (!file.readExact(bytes.data(), static_cast<int>(bytes.size())))
        return false;
    Ps2AdpcmStream::Header header{};
    return wiiValidAdpHeader(bytes.data(), static_cast<std::size_t>(streamSize), header);
}

bool wiiStartStream(const std::string &path, float volume, WiiStreamKind kind)
{
    if (!wiiValidateStream(path))
        return false;

    wiiStopStream();

    s_wiiStreamPath       = path;
    s_wiiStreamKind       = kind;
    s_wiiStreamVolume     = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
    s_wiiStreamEof        = false;
    s_wiiStreamReady[0]  = false;
    s_wiiStreamReady[1]  = false;
    s_wiiStreamFrames[0] = 0;
    s_wiiStreamFrames[1] = 0;
    s_wiiStreamBytes[0] = 0;
    s_wiiStreamBytes[1] = 0;
    s_wiiStreamRunning    = true;

    // Match devkitPro's oggplayer priority. The previous 64 could pre-empt the
    // game more aggressively while opening/decoding a track and merely move the
    // visible startup hitch from the main thread to a higher-priority worker.
    if (LWP_CreateThread(&s_wiiStreamThread, wiiStreamThreadMain, nullptr,
            s_wiiStreamStack, sizeof(s_wiiStreamStack), 80) == -1)
    {
        MC_LOG_INFO("wii", "[WII][AUDIO] failed to start stream thread\n");
        s_wiiStreamRunning = false;
        s_wiiStreamPath.clear();
        return false;
    }
    return true;
}

bool wiiStreamIsActive()
{
    // Opening/priming is active too. Requiring an already-running ASND voice
    // made the next game tick stop and reopen a stream whose first buffer was
    // still being decoded.
    return s_wiiStreamRunning;
}

} // namespace

extern "C" void wiiAudioMemoryStats(u32 *cachedBytes, u32 *residentBytes,
                                     u32 *pendingBuffers, u32 *rejectedSounds,
                                     u32 *streamStarves, u32 *streamAddBusy)
{
    if (cachedBytes)
        *cachedBytes = (u32)s_wiiSfxCacheBytes;
    if (residentBytes)
        *residentBytes = (u32)s_wiiSfxResidentBytes;
    if (pendingBuffers)
        *pendingBuffers = (u32)s_wiiPendingFree.size();
    if (rejectedSounds)
        *rejectedSounds = (u32)s_wiiRejectedSfx.size();
    if (streamStarves)
        *streamStarves = s_wiiStreamStarves;
    if (streamAddBusy)
        *streamAddBusy = s_wiiStreamAddBusy;
}

void *SoundManager::sndSystem = nullptr;
bool SoundManager::loaded = false;

SoundManager::SoundManager()
    : soundPoolSounds(), soundPoolStreaming(), soundPoolMusic(), soundVolume(0),
      options(nullptr), rand(), ticksBeforeMusic(rand.nextInt(100) + 40)
{
}

void SoundManager::tryToSetLibraryAndCodecs()
{
    if (loaded)
        return;

    ASND_Init();
    ASND_Pause(0); // ASND_Init() starts paused
    s_wiiPendingFree.reserve(MAX_SND_VOICES - 1);

    loaded = true;
    MC_LOG_INFO("wii", "[WII][AUDIO] ASND ready\n");
}

void SoundManager::loadSoundSettings(GameSettings *gamesettings)
{
    soundPoolStreaming.motionX = false;
    options = gamesettings;

    if (!loaded && (gamesettings == nullptr || gamesettings->soundVolume != 0.0f || gamesettings->musicVolume != 0.0f))
        tryToSetLibraryAndCodecs();
}

void SoundManager::onSoundOptionsChanged()
{
    if (!loaded && options && (options->soundVolume != 0.0f || options->musicVolume != 0.0f))
        tryToSetLibraryAndCodecs();

    if (!loaded || options == nullptr)
        return;
    if (s_wiiStreamKind == WiiStreamKind::Music)
    {
        if (options->musicVolume <= 0.0f)
        {
            wiiStopStream();
        }
        else
        {
            s_wiiStreamVolume = options->musicVolume;
            const int volume = (int)(s_wiiStreamVolume * (float)MAX_VOLUME + 0.5f);
            ASND_ChangeVolumeVoice(0, volume, volume);
        }
    }
}

void SoundManager::closeMinecraft()
{
    if (loaded)
    {
        wiiStopStream();
        wiiClearSfxCache();
        ASND_Pause(1);
        ASND_End();
    }
    loaded = false;
}

void SoundManager::addSound(const jstring &s, const std::string &file)
{
    if (audioPathHasExtension(file, ".adp"))
        soundPoolSounds.addSound(s, file);
}

void SoundManager::addStreaming(const jstring &s, const std::string &file)
{
    // The same three shapes the PS2/3DS streamers accept, so a pack staged
    // for those consoles registers here unmodified.
    if (audioPathHasExtension(file, ".adp") || audioPathHasExtension(file, ".pcm") ||
        audioPathHasExtension(file, ".wav"))
        soundPoolStreaming.addSound(s, file);
}

void SoundManager::addMusic(const jstring &s, const std::string &file)
{
    // Same trio: the pack's music folders may carry raw .pcm tracks, an
    // all-.adp pack, or the third script's headered .wav shape.
    if (audioPathHasExtension(file, ".adp") || audioPathHasExtension(file, ".pcm") ||
        audioPathHasExtension(file, ".wav"))
        soundPoolMusic.addSound(s, file);
}

bool SoundManager::playMusicFileNow(const std::string &file)
{
    if (!loaded || !options || options->musicVolume == 0.0f)
        return false;
    if ((!audioPathHasExtension(file, ".adp") && !audioPathHasExtension(file, ".pcm") &&
         !audioPathHasExtension(file, ".wav")) || wiiStreamIsActive())
        return false;

    if (!wiiStartStream(file, options->musicVolume, WiiStreamKind::Music))
        return false;

    ticksBeforeMusic = rand.nextInt(1200) + 600;
    return true;
}

void SoundManager::playRandomMusicIfReady()
{
    if (!loaded || !options || options->musicVolume == 0.0f)
        return;

    // Shares voice 0 with the jukebox (playStreaming) below, so "is voice 0
    // busy" is the right question regardless of which of the two started it.
    if (wiiStreamIsActive())
        return;

    if (ticksBeforeMusic > 0)
    {
        ticksBeforeMusic--;
        return;
    }

    SoundPoolEntry *entry = soundPoolMusic.getRandomSound();
    ticksBeforeMusic = rand.nextInt(1200) + 600;
    if (!entry)
    {
#if MC_LOG_LEVEL >= 2
        MC_LOG_INFO("wii", "[WII][AUDIO] playRandomMusicIfReady: music pool is empty\n");
#endif
        return;
    }

    wiiStartStream(entry->soundUrl, options->musicVolume, WiiStreamKind::Music);
}

void SoundManager::setListenerPosition(EntityLiving *entityliving, float partialTick)
{
    if (!loaded || !options || options->soundVolume == 0.0f)
        return;

    updateAudioListener(s_listener, entityliving, partialTick);
}

void SoundManager::playStreaming(const jstring &s, float x, float y, float z, float f3, float)
{
    if (!loaded || !options || (options->soundVolume == 0.0f && !s.empty()))
        return;

    if (s.empty())
    {
        if (s_wiiStreamKind == WiiStreamKind::Streaming)
            wiiStopStream();
        return;
    }

    if (f3 <= 0.0f)
        return;

    const float attenuation = audioStreamingAttenuation(s_listener, x, y, z);
    if (attenuation <= 0.0f)
        return;

    SoundPoolEntry *entry = soundPoolStreaming.getRandomSoundFromSoundPool(s);
    if (entry && wiiStartStream(entry->soundUrl, 0.5f * attenuation * options->soundVolume, WiiStreamKind::Streaming))
        ticksBeforeMusic = rand.nextInt(1200) + 600;
}

void SoundManager::playSound(const jstring &s, float x, float y, float z, float volume, float)
{
    if (!loaded || !options || options->soundVolume == 0.0f)
        return;

    SoundPoolEntry *entry = soundPoolSounds.getRandomSoundFromSoundPool(s);
#if MC_LOG_LEVEL >= 2
    if (!entry)
        MC_LOG_INFO("wii", "[WII][AUDIO] playSound: no entry for '%s'\n", s.c_str());
#endif
    if (entry && volume > 0.0f)
    {
        const float attenuation = audioSpatialAttenuation(s_listener, x, y, z, volume);
        if (attenuation <= 0.0f)
            return;

        wiiPlaySfx(entry->soundUrl, std::min(volume, 1.0f) * attenuation * options->soundVolume);
    }
}

void SoundManager::playSoundFX(const jstring &s, float volume, float)
{
    if (!loaded || !options || options->soundVolume == 0.0f)
        return;

    SoundPoolEntry *entry = soundPoolSounds.getRandomSoundFromSoundPool(s);
#if MC_LOG_LEVEL >= 2
    if (!entry)
        MC_LOG_DEBUG("audio", "playSoundFX: no entry for '%s'", s.c_str());
#endif
    if (entry)
        wiiPlaySfx(entry->soundUrl, std::min(volume, 1.0f) * 0.25f * options->soundVolume);
}

#endif
