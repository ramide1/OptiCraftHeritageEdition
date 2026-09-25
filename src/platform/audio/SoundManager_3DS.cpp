// SoundManager_3DS.cpp -- Nintendo 3DS ndsp backend (Phase 4).
//
// The 3DS consumes the same OGG assets the Wii backend does: original Java
// Edition audio (resources/newsound/**.ogg, resources/music/*.ogg), decoded
// with the shared stb_vorbis path (VorbisAssetOpen.h -- pak:// aware) into
// 16-bit PCM in linear memory (the DSP DMAs from the linear heap, so the
// malloc'd decode result is always copied across), then queued on ndsp
// channels:
//
//   * One-shot SFX decode once and cache, LRU-evicted under a fixed byte
//     budget (the Wii backend's arithmetic: the set decodes to several
//     times its compressed size, and the heap has to keep a world too).
//   * Music/records stream on channel 0, at most one at a time -- the same
//     single-stream semantics the PS2 uses. Phase 4 v1 decodes the whole
//     file (the calm*.ogg are ~2.5 MB compressed) -- fine under the
//     emulator, and gated on allocation failure for real hardware; chunked
//     streaming from the pak is the follow-up.
//
// The DSP firmware: ndspInit() loads romfs:/dspfirm.cdc or
// sdmc:/3ds/dspfirm.cdc. Real hardware needs the file; Azahar's HLE
// accepts any readable placeholder.
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

#include <3ds.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "net/minecraft/src/GameSettings.h"
#include "net/minecraft/src/EntityLiving.h"
#include "net/minecraft/src/SoundPoolEntry.h"
#include "platform/Resources.h"
#include "platform/audio/AudioAssetFormat.h"
#include "platform/audio/AudioSpatialization.h"
#include "platform/audio/VorbisAssetOpen.h"

namespace
{

AudioListenerState s_listener;

constexpr int CTR_MUSIC_CHANNEL = 0;
constexpr int CTR_SFX_FIRST_CHANNEL = 1;
constexpr int CTR_SFX_CHANNEL_COUNT = 24;

// Decoded-PCM budget for one-shot SFX, mirroring the Wii backend's
// arithmetic: enough to keep a busy scene's working set resident without
// competing with world data on a 64 MB Old-3DS heap.
constexpr size_t CTR_SFX_CACHE_BUDGET_BYTES = 4 * 1024 * 1024;

float s_soundVolume = 1.0f;
float s_musicVolume = 1.0f;
bool s_streamIsMusic = false; // false = record/streaming, true = music
bool s_streamActive = false;
int s_nextSfxChannel = CTR_SFX_FIRST_CHANNEL;

struct CachedSample
{
	std::int16_t *pcm = nullptr; // linearAlloc: DSP DMA reads this
	std::uint32_t samples = 0;   // frames per channel
	int channels = 1;
	int rate = 44100;
	std::uint64_t lastUsed = 0;
	std::size_t allocBytes = 0;
	// One wave buffer per ndsp channel: the same sample can be playing on
	// several channels at once (overlapping footsteps are the constant
	// case), and ndspWaveBuf is a live queue node -- re-adding the one
	// struct while it is still queued on another channel corrupts the
	// playlist and the speaker plays the chopped result (the "robotic"
	// report). Each play arms its own channel's buf below.
	ndspWaveBuf wave[CTR_SFX_CHANNEL_COUNT]{};
};

std::unordered_map<std::string, CachedSample> s_sfxCache;
std::unordered_map<std::string, bool> s_rejectedSfx;
std::size_t s_sfxCacheBytes = 0;
std::uint64_t s_cacheClock = 0;

// The one live stream's playback state (channel 0). Music and records are
// minutes of 44.1 kHz stereo -- 25-30 MB decoded, which does not fit next to
// a loaded world in the 64 MB Old-3DS application region (the whole-file
// decode was exactly what "ogg stream decode failed" reported). So the
// stream is decoded incrementally into a ring of wave buffers.
constexpr int CTR_STREAM_BUFFER_COUNT = 6;
constexpr int CTR_STREAM_BUFFER_FRAMES = 8192; // ~186 ms stereo 44.1 kHz

// Refills ran in ndsp's own sound-frame callback -- the canonical ndsp
// streaming shape, and immune to the game-thread starvation that a stream
// thread of our own suffered (the original "robotic music" report). But that
// callback fires on the DSP service thread, whose stack is a fraction of what
// stb_vorbis's decoder wants: the decode overran it, which is both the
// corrupted "robotic with echo/saturation" audio the reports described and
// the menu exit the 2026-09-25 run died with ~3 s after the music started.
// So the refills now run on a thread of our own with a stack that fits the
// decoder, at a priority ABOVE the game thread's own 0x30 -- the starvation
// the first attempt hit came from running it below the game thread -- and it
// sleeps between passes: the ring above holds ~1.1 s of audio, so a few ms of
// cadence is far under what one buffer drains in.
constexpr int CTR_STREAM_THREAD_PRIORITY = 0x2B;
constexpr size_t CTR_STREAM_THREAD_STACK_BYTES = 32 * 1024;

// Defined below, after startOggStream; the stream start needs it first.
void streamRefillThreadMain(void *);

struct StreamState
{
	stb_vorbis *vorbis = nullptr;
	std::int16_t *pcm[CTR_STREAM_BUFFER_COUNT] = {};
	ndspWaveBuf wave[CTR_STREAM_BUFFER_COUNT]{};
	std::string path;
	int channels = 2;
	int rate = 44100;
	float gain = 1.0f;
	bool running = false;
	bool eof = false;
	// libctru's Thread is already a pointer type (Thread_tag*).
	Thread thread = nullptr;
};

StreamState s_stream;

void stopStream();
void streamRefillCallback(void *);

void setChannelMix(int channel, float gain)
{
	float mix[12] = {};
	mix[0] = gain; // front left
	mix[1] = gain; // front right
	ndspChnSetMix(channel, mix);
}

// Whole-stream OGG decode, shared with the Wii backend. The result lives in
// a malloc'd (standard-heap) buffer, which the DSP cannot DMA from -- the
// result is moved into a linearAlloc'd buffer and the original freed.
bool decodeVorbisToLinear(const std::string &path, std::int16_t **outPcm,
                          int *outChannels, int *outRate, std::uint32_t *outFrames,
                          std::size_t *outBytes)
{
	std::int16_t *pcm = nullptr;
	int channels = 0;
	int rate = 0;
	const int frames = platformDecodeVorbis(path, &channels, &rate, &pcm);
	if (frames <= 0 || pcm == nullptr || channels <= 0)
	{
		std::free(pcm);
		return false;
	}

	const std::size_t bytes = static_cast<std::size_t>(frames) *
	                           static_cast<std::size_t>(channels) * sizeof(std::int16_t);
	std::int16_t *linear = static_cast<std::int16_t *>(linearAlloc(bytes));
	if (linear == nullptr)
	{
		std::free(pcm);
		return false;
	}
	std::memcpy(linear, pcm, bytes);
	std::free(pcm);

	*outPcm = linear;
	*outChannels = channels;
	*outRate = rate;
	*outFrames = static_cast<std::uint32_t>(frames);
	*outBytes = bytes;
	return true;
}

void freeSfxEntry(std::unordered_map<std::string, CachedSample>::iterator it)
{
	// ndsp may still be DMA-reading a buffer queued on a previous play of
	// this sample; clear every channel that holds it before freeing.
	for (int channel = CTR_SFX_FIRST_CHANNEL; channel < CTR_SFX_CHANNEL_COUNT; ++channel)
		ndspChnWaveBufClear(channel);
	s_sfxCacheBytes -= it->second.allocBytes;
	linearFree(it->second.pcm);
	s_sfxCache.erase(it);
}

void evictFor(std::size_t incomingBytes)
{
	while (s_sfxCacheBytes + incomingBytes > CTR_SFX_CACHE_BUDGET_BYTES && !s_sfxCache.empty())
	{
		auto oldest = s_sfxCache.begin();
		for (auto it = s_sfxCache.begin(); it != s_sfxCache.end(); ++it)
			if (it->second.lastUsed < oldest->second.lastUsed)
				oldest = it;
		freeSfxEntry(oldest);
	}
}

CachedSample *getDecodedSfx(const std::string &path)
{
	const auto hit = s_sfxCache.find(path);
	if (hit != s_sfxCache.end())
	{
		hit->second.lastUsed = ++s_cacheClock;
		return &hit->second;
	}
	if (s_rejectedSfx.count(path) != 0)
		return nullptr;

	if (PlatformResources::fileSize(path) <= 0)
	{
		MC_LOG_DEBUG("audio", "missing audio %s\n", path.c_str());
		s_rejectedSfx[path] = true;
		return nullptr;
	}
	if (!audioPathHasExtension(path, ".ogg"))
	{
		MC_LOG_DEBUG("audio", "unsupported audio asset %s\n", path.c_str());
		s_rejectedSfx[path] = true;
		return nullptr;
	}

	std::int16_t *pcm = nullptr;
	int channels = 0;
	int rate = 0;
	std::uint32_t frames = 0;
	std::size_t bytes = 0;
	if (!decodeVorbisToLinear(path, &pcm, &channels, &rate, &frames, &bytes))
	{
		MC_LOG_WARN("audio", "3ds: ogg decode failed %s\n", path.c_str());
		s_rejectedSfx[path] = true;
		return nullptr;
	}
	evictFor(bytes);

	CachedSample &entry = s_sfxCache[path];
	entry.pcm = pcm;
	entry.samples = frames;
	entry.channels = channels;
	entry.rate = rate;
	entry.lastUsed = ++s_cacheClock;
	entry.allocBytes = bytes;
	// The per-channel wave bufs stay zeroed here: each play arms its own
	// channel's node (see playSfxSample), exactly where the Wii arms
	// ASND_SetVoice's arguments at call time.
	std::memset(entry.wave, 0, sizeof(entry.wave));
	DSP_FlushDataCache(pcm, bytes);
	s_sfxCacheBytes += bytes;
	return &entry;
}

int nextSfxChannel()
{
	const int usable = CTR_SFX_CHANNEL_COUNT - CTR_SFX_FIRST_CHANNEL;
	for (int offset = 0; offset < usable; ++offset)
	{
		const int channel = CTR_SFX_FIRST_CHANNEL +
			((s_nextSfxChannel - CTR_SFX_FIRST_CHANNEL + offset) % usable);
		if (ndspChnIsPlaying(channel))
			continue;
		s_nextSfxChannel = channel + 1;
		if (s_nextSfxChannel >= CTR_SFX_CHANNEL_COUNT)
			s_nextSfxChannel = CTR_SFX_FIRST_CHANNEL;
		return channel;
	}
	// All busy: steal the round-robin cursor's channel outright.
	const int channel = s_nextSfxChannel;
	s_nextSfxChannel = channel + 1;
	if (s_nextSfxChannel >= CTR_SFX_CHANNEL_COUNT)
		s_nextSfxChannel = CTR_SFX_FIRST_CHANNEL;
	return channel;
}

void playSfxSample(CachedSample *sample, float gain, float pitch)
{
	if (sample == nullptr || sample->pcm == nullptr)
		return;
	const int channel = nextSfxChannel();
	ndspChnReset(channel);
	// POLYPHASE, like every other channel -- see the note in loadLibrary().
	ndspChnSetInterp(channel, NDSP_INTERP_POLYPHASE);
	ndspChnSetFormat(channel, sample->channels >= 2 ? NDSP_FORMAT_STEREO_PCM16
	                                                : NDSP_FORMAT_MONO_PCM16);
	const float rate = pitch > 0.0f ? sample->rate * pitch : static_cast<float>(sample->rate);
	ndspChnSetRate(channel, rate);
	setChannelMix(channel, gain);

	// Arm this channel's own queue node, the ndsp counterpart of the
	// buffer+size arguments ASND_SetVoice takes on the Wii: data the DSP
	// can DMA (the linearAlloc'd decode), frame count per channel, no loop.
	ndspWaveBuf &wave = sample->wave[channel];
	std::memset(&wave, 0, sizeof(wave));
	wave.data_vaddr = sample->pcm;
	wave.nsamples = sample->samples;
	wave.looping = false;
	ndspChnWaveBufAdd(channel, &wave);
}

bool startOggStream(const std::string &path, float gain)
{
	if (PlatformResources::fileSize(path) <= 0 || !audioPathHasExtension(path, ".ogg"))
		return false;

	// Open and probe first: the header tells channels and rate, and opening
	// costs nothing on failure.
	int error = 0;
	stb_vorbis *vorbis = platformOpenVorbis(path, &error);
	if (vorbis == nullptr)
	{
		MC_LOG_WARN("audio", "3ds: ogg stream open failed %s\n", path.c_str());
		return false;
	}
	const stb_vorbis_info info = stb_vorbis_get_info(vorbis);
	if (info.channels <= 0 || info.sample_rate <= 0)
	{
		stb_vorbis_close(vorbis);
		return false;
	}

	// The music set is stereo; a mono source would need channel duplication --
	// reject rather than mis-play it.
	if (info.channels != 2)
	{
		MC_LOG_WARN("audio", "3ds: ogg stream %s is %d-ch (need 2); skipped\n",
		            path.c_str(), info.channels);
		stb_vorbis_close(vorbis);
		return false;
	}

	s_stream.vorbis = vorbis;
	s_stream.path = path;
	s_stream.channels = static_cast<int>(info.channels);
	s_stream.rate = static_cast<int>(info.sample_rate);
	s_stream.gain = gain;
	s_stream.eof = false;

	// A fresh ndsp channel for the fresh stream: reset clears any queue the
	// previous stream left behind, and the format/rate/mix are set once
	// here -- the thread only queues buffers.
	//
	// Interpolation is the whole "music sounds robotic" question: the DSP
	// runs at ~32728 Hz and the OGG carries 44100, so this channel is a
	// 44100 -> 32728 downsample. NDSP_INTERP_NONE nearest-neighbours that
	// step and aliases the top third of the music's spectrum back down onto
	// itself -- the buzz the report describes. POLYPHASE filters properly.
	ndspChnReset(CTR_MUSIC_CHANNEL);
	ndspChnSetInterp(CTR_MUSIC_CHANNEL, NDSP_INTERP_POLYPHASE);
	ndspChnSetRate(CTR_MUSIC_CHANNEL, static_cast<float>(s_stream.rate));
	ndspChnSetFormat(CTR_MUSIC_CHANNEL, NDSP_FORMAT_STEREO_PCM16);
	setChannelMix(CTR_MUSIC_CHANNEL, gain);

	const std::size_t bufferBytes = static_cast<std::size_t>(CTR_STREAM_BUFFER_FRAMES) *
	                                static_cast<std::size_t>(s_stream.channels) *
	                                sizeof(std::int16_t);
	for (int i = 0; i < CTR_STREAM_BUFFER_COUNT; ++i)
	{
		s_stream.pcm[i] = static_cast<std::int16_t *>(linearAlloc(bufferBytes));
		if (s_stream.pcm[i] == nullptr)
		{
			MC_LOG_WARN("audio", "3ds: stream ring allocation failed (%d x %zu bytes); stopped\n",
			            CTR_STREAM_BUFFER_COUNT, bufferBytes);
			stopStream();
			return false;
		}
		std::memset(&s_stream.wave[i], 0, sizeof(ndspWaveBuf));
		s_stream.wave[i].data_vaddr = s_stream.pcm[i];
	}

	s_stream.running = false;
	s_stream.eof = false;

	// Prebuffer: fill and queue the whole ring before returning, so playback
	// starts with the full ring behind it and the first callback refill only
	// has to replace one buffer.
	for (int i = 0; i < CTR_STREAM_BUFFER_COUNT; ++i)
	{
		const int frames = stb_vorbis_get_samples_short_interleaved(
			s_stream.vorbis, s_stream.channels, s_stream.pcm[i],
			CTR_STREAM_BUFFER_FRAMES * s_stream.channels);
		if (frames <= 0)
		{
			s_stream.eof = true;
			break;
		}
		s_stream.wave[i].nsamples = static_cast<u32>(frames);
		DSP_FlushDataCache(s_stream.pcm[i],
		                   static_cast<std::size_t>(frames) * s_stream.channels * sizeof(std::int16_t));
		ndspChnWaveBufAdd(CTR_MUSIC_CHANNEL, &s_stream.wave[i]);
	}

	s_stream.running = true;
	s_streamActive = true;
	s_stream.thread = threadCreate(streamRefillThreadMain, nullptr,
	                               CTR_STREAM_THREAD_STACK_BYTES,
	                               CTR_STREAM_THREAD_PRIORITY, -2, false);
	if (s_stream.thread == nullptr)
	{
		// Without the thread the prebuffered ring above is all the music
		// there is; logged because a stream that stalls otherwise reads as a
		// silent one.
		MC_LOG_WARN("audio", "3ds: stream thread creation failed; music stops when the ring drains\n");
	}
	return true;
}

void stopStream()
{
	if (!s_streamActive && s_stream.vorbis == nullptr)
		return;

	// Told to stop: clear the queue, then every resource the stream owned
	// goes back. The callback checks s_stream.running, so clearing it first
	// means no refill can race the teardown.
	s_stream.running = false;
	s_stream.eof = true;
	if (s_stream.thread != nullptr)
	{
		// The thread observes running/eof and exits its loop promptly; join
		// before the teardown below frees anything it might still touch.
		threadJoin(s_stream.thread, U64_MAX);
		threadFree(s_stream.thread);
		s_stream.thread = nullptr;
	}
	ndspChnWaveBufClear(CTR_MUSIC_CHANNEL);
	ndspChnReset(CTR_MUSIC_CHANNEL);
	for (int i = 0; i < CTR_STREAM_BUFFER_COUNT; ++i)
	{
		if (s_stream.pcm[i] != nullptr)
		{
			linearFree(s_stream.pcm[i]);
			s_stream.pcm[i] = nullptr;
		}
	}
	if (s_stream.vorbis != nullptr)
	{
		stb_vorbis_close(s_stream.vorbis);
		s_stream.vorbis = nullptr;
	}
	s_stream.path.clear();
	s_streamActive = false;
}

// One pass over the ring, refilling whatever the channel has finished
// (NDSP_WBUF_DONE / FREE), decoding straight from stb_vorbis -- the same pull
// the Wii's asndlib callback performs. Returns true when it decoded anything.
bool streamRefillPoll()
{
	if (!s_stream.running || s_stream.eof || s_stream.vorbis == nullptr)
		return false;

	bool decoded = false;
	for (int i = 0; i < CTR_STREAM_BUFFER_COUNT; ++i)
	{
		if (s_stream.wave[i].status == NDSP_WBUF_QUEUED ||
		    s_stream.wave[i].status == NDSP_WBUF_PLAYING)
			continue;

		const int frames = stb_vorbis_get_samples_short_interleaved(
			s_stream.vorbis, s_stream.channels, s_stream.pcm[i],
			CTR_STREAM_BUFFER_FRAMES * s_stream.channels);
		if (frames <= 0)
		{
			// End of stream: stop refilling and let the queued tail drain.
			// The main thread's playRandomMusicIfReady() observes the drain
			// and re-arms the next track after its timer, exactly like the
			// PS2 path.
			s_stream.eof = true;
			return decoded;
		}
		s_stream.wave[i].nsamples = static_cast<u32>(frames);
		DSP_FlushDataCache(s_stream.pcm[i],
		                   static_cast<std::size_t>(frames) * s_stream.channels * sizeof(std::int16_t));
		ndspChnWaveBufAdd(CTR_MUSIC_CHANNEL, &s_stream.wave[i]);
		decoded = true;
	}
	return decoded;
}

// The stream thread. A priority above the game thread's own 0x30, so the
// tick-heavy game thread cannot starve it -- that starvation, on a thread run
// below the game thread, was the original "robotic music". Exits when
// stopStream() clears s_stream.running (it joins before freeing anything) or
// when the stream ends; without the thread the prebuffered ring is all the
// music there is.
void streamRefillThreadMain(void *)
{
	for (;;)
	{
		if (!s_stream.running || s_stream.eof)
			break;
		const bool decoded = streamRefillPoll();
		svcSleepThread(decoded ? 2000000ll : 16000000ll);
	}
}

} // namespace

void *SoundManager::sndSystem = nullptr;
bool SoundManager::loaded = false;

SoundManager::SoundManager()
    : soundPoolSounds(), soundPoolStreaming(), soundPoolMusic(), soundVolume(0),
      options(nullptr), rand(), ticksBeforeMusic(rand.nextInt(12000))
{
}

void SoundManager::loadSoundSettings(GameSettings *gamesettings)
{
	soundPoolStreaming.motionX = false;
	options = gamesettings;
	s_soundVolume = gamesettings ? gamesettings->soundVolume : 1.0f;
	s_musicVolume = gamesettings ? gamesettings->musicVolume : 1.0f;

	if (!loaded && (gamesettings == nullptr || gamesettings->soundVolume != 0.0f ||
	                gamesettings->musicVolume != 0.0f))
		tryToSetLibraryAndCodecs();
}

void SoundManager::tryToSetLibraryAndCodecs()
{
	if (loaded)
		return;

	if (R_FAILED(ndspInit()))
	{
		MC_LOG_WARN("audio", "3ds: ndspInit failed -- no sound (dspfirm.cdc missing on SD?)\n");
		return;
	}

	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspSetMasterVol(1.0f);
	// No sound-frame callback: the stream refills on its own thread now (see
	// the StreamState note) -- the callback's thread could not host a
	// stb_vorbis decode.
	// The interpolation mode matters on *every* channel here, because ndsp
	// mixes at NDSP_SAMPLE_RATE (SYSCLOCK_SOC/512 ~ 32728 Hz) while the
	// assets arrive at their own rates -- the OGGs at 44100, the SFX at
	// whatever the pack carries -- so each channel is being resampled by
	// the DSP no matter what. NDSP_INTERP_NONE is nearest-neighbour through
	// that conversion: each output sample copies whichever input sample is
	// closest, folding everything above ~16 kHz back over the spectrum.
	// That aliasing is the metallic/robotic cast the music was reported
	// with, and it is on the SFX path too. POLYPHASE is ndsp's band-limited
	// resampler -- the step a rate conversion actually needs -- and is what
	// this loop leaves every channel on; playSfxSample/startOggStream must
	// not undo it (see their own ndspChnSetInterp calls).
	for (int channel = 0; channel < CTR_SFX_CHANNEL_COUNT; ++channel)
	{
		ndspChnReset(channel);
		ndspChnSetInterp(channel, NDSP_INTERP_POLYPHASE);
	}
	s_nextSfxChannel = CTR_SFX_FIRST_CHANNEL;
	loaded = true;
	MC_LOG_DEBUG("audio", "3ds: ndsp ready\n");
}

void SoundManager::onSoundOptionsChanged()
{
	if (options)
	{
		s_soundVolume = options->soundVolume;
		s_musicVolume = options->musicVolume;
	}

	if (!loaded && options && (options->soundVolume != 0.0f || options->musicVolume != 0.0f))
		tryToSetLibraryAndCodecs();

	if (loaded && s_streamActive)
	{
		const float gain = s_streamIsMusic ? s_musicVolume : s_soundVolume * s_stream.gain;
		if (gain <= 0.0f)
			stopStream();
		else
			setChannelMix(CTR_MUSIC_CHANNEL, gain);
	}
}

void SoundManager::closeMinecraft()
{
	stopStream();
	for (auto it = s_sfxCache.begin(); it != s_sfxCache.end(); it = s_sfxCache.begin())
		freeSfxEntry(it);
	s_rejectedSfx.clear();
	if (loaded)
	{
		for (int channel = 0; channel < CTR_SFX_CHANNEL_COUNT; ++channel)
			ndspChnWaveBufClear(channel);
		ndspExit();
	}
	loaded = false;
}

void SoundManager::addSound(const jstring &s, const std::string &file)
{
	if (audioPathHasExtension(file, ".ogg"))
		soundPoolSounds.addSound(s, file);
}

void SoundManager::addStreaming(const jstring &s, const std::string &file)
{
	if (audioPathHasExtension(file, ".ogg"))
		soundPoolStreaming.addSound(s, file);
}

void SoundManager::addMusic(const jstring &s, const std::string &file)
{
	if (audioPathHasExtension(file, ".ogg"))
		soundPoolMusic.addSound(s, file);
}

bool SoundManager::playMusicFileNow(const std::string &file)
{
	if (!loaded || !options || options->musicVolume == 0.0f)
		return false;

	stopStream();
	if (!startOggStream(file, s_musicVolume))
		return false;

	s_streamIsMusic = true;
	ticksBeforeMusic = rand.nextInt(12000) + 12000;
	return true;
}

void SoundManager::playRandomMusicIfReady()
{
	if (!loaded || !options || options->musicVolume == 0.0f)
		return;

	if (s_streamActive && s_streamIsMusic && ndspChnIsPlaying(CTR_MUSIC_CHANNEL))
		return;

	if (s_streamActive && s_streamIsMusic)
		stopStream();

	if (ticksBeforeMusic > 0)
	{
		ticksBeforeMusic--;
		return;
	}

	SoundPoolEntry *entry = soundPoolMusic.getRandomSound();
	ticksBeforeMusic = rand.nextInt(12000) + 12000;
	if (entry == nullptr)
		return;

	if (startOggStream(entry->soundUrl, s_musicVolume))
		s_streamIsMusic = true;
}

void SoundManager::setListenerPosition(EntityLiving *entityliving, float partialTick)
{
	if (!loaded || !options || options->soundVolume == 0.0f)
		return;

	updateAudioListener(s_listener, entityliving, partialTick);
}

void SoundManager::playStreaming(const jstring &s, float x, float y, float z, float volume, float)
{
	if (!loaded || !options || (options->soundVolume == 0.0f && !s.empty()))
		return;

	if (s.empty())
	{
		if (s_streamActive && !s_streamIsMusic)
			stopStream();
		return;
	}

	if (volume <= 0.0f)
		return;

	const float attenuation = audioStreamingAttenuation(s_listener, x, y, z);
	if (attenuation <= 0.0f)
		return;

	SoundPoolEntry *entry = soundPoolStreaming.getRandomSoundFromSoundPool(s);
	if (entry == nullptr)
	{
		MC_LOG_WARN("audio", "missing 3ds streaming sound %s\n", s.c_str());
		return;
	}

	stopStream();
	ticksBeforeMusic = rand.nextInt(12000) + 12000;
	if (startOggStream(entry->soundUrl, 0.5f * attenuation * s_soundVolume))
		s_streamIsMusic = false;}

void SoundManager::playSound(const jstring &s, float x, float y, float z, float volume, float pitch)
{
	if (!loaded || !options || options->soundVolume == 0.0f)
		return;

	SoundPoolEntry *entry = soundPoolSounds.getRandomSoundFromSoundPool(s);
	if (entry == nullptr || volume <= 0.0f)
		return;

	const float attenuation = audioSpatialAttenuation(s_listener, x, y, z, volume);
	if (attenuation <= 0.0f)
		return;

	CachedSample *sample = getDecodedSfx(entry->soundUrl);
	if (sample == nullptr)
		return;

	playSfxSample(sample, attenuation * s_soundVolume, pitch);
}

void SoundManager::playSoundFX(const jstring &s, float volume, float pitch)
{
	if (!loaded || !options || options->soundVolume == 0.0f)
		return;

	SoundPoolEntry *entry = soundPoolSounds.getRandomSoundFromSoundPool(s);
	if (entry == nullptr || volume <= 0.0f)
		return;

	CachedSample *sample = getDecodedSfx(entry->soundUrl);
	if (sample == nullptr)
		return;

	playSfxSample(sample, volume * s_soundVolume, pitch);
}

#endif // NO_SOUND
