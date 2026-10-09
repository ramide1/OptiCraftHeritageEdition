#include "pc/audio/PcMusicStream.h"

#include "platform/audio/Ps2AdpcmStreamDecoder.h"
#include "platform/audio/AdpAssetDecode.h"
#include "platform/audio/AudioConvert.h"
#include "platform/audio/AudioAssetFormat.h"
#include "platform/storage/AssetPak.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

// Streamed music/records for the desktop OpenAL mixer. Shapes follow the
// PS2/3DS asset tree (2026-10 unification, see SoundManager_PC.cpp):
//
//   * .pcm  headerless s16le mono 22050 (scripts/ogg to pcm): read and
//     frame-doubled to the 44100 mixer rate -- no decoder at all.
//   * .adp  SPU2-ADPCM (scripts/ogg to adp; the all-adp pack shape), decoded
//     incrementally through the shared portable decoder, so a minutes-long
//     track never has to sit decoded in RAM.
//
// Both feed the same float-stereo ring the OpenAL pump's mix() drains.

namespace
{
constexpr int kOutputSampleRate = 44100;
constexpr std::size_t kRingSamples = static_cast<std::size_t>(kOutputSampleRate) * 2u;

std::array<float, kRingSamples> s_ring{};
std::size_t s_read = 0;
std::size_t s_write = 0;
std::size_t s_count = 0;
bool s_stop = false;
bool s_running = false;
bool s_eof = false;
std::thread s_thread;
std::mutex s_mutex;
std::condition_variable s_condition;

// Pak-aware sequential/seekable reader over either a loose file or an
// AssetPak entry (the pattern PcMusicStream has always used: a fresh
// FILE* per thread, positioned inside the archive for pak paths).
struct AssetReader
{
    std::FILE *file = nullptr;
    std::size_t base = 0;   // asset start inside the archive (0 when loose)
    std::size_t extent = 0; // asset byte count

    bool open(const std::string &path)
    {
        if (AssetPak::isPakPath(path))
        {
            std::uint32_t offset = 0, size = 0;
            if (!AssetPak::locate(AssetPak::keyOf(path), &offset, &size))
                return false;
            file = std::fopen(AssetPak::archivePath().c_str(), "rb");
            if (file == nullptr)
                return false;
            base = offset;
            extent = size;
            return seek(0);
        }
        file = std::fopen(path.c_str(), "rb");
        if (file == nullptr)
            return false;
        std::fseek(file, 0, SEEK_END);
        extent = static_cast<std::size_t>(std::ftell(file));
        return seek(0);
    }

    // Offset is relative to the asset start (loose file position is `base`,
    // the archive's entry offset for pak paths).
    bool seek(std::size_t assetOffset)
    {
        return file != nullptr && assetOffset <= extent &&
               std::fseek(file, static_cast<long>(base + assetOffset), SEEK_SET) == 0;
    }

    std::size_t read(void *dst, std::size_t bytes)
    {
        return file == nullptr ? 0 : std::fread(dst, 1, bytes, file);
    }

    void close()
    {
        if (file != nullptr)
            std::fclose(file);
        file = nullptr;
    }

    ~AssetReader() { close(); }
};

void markEofAndMaybeStop()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_eof = true;
    if (s_count == 0)
        s_running = false;
}

bool ringWrite(const float *samples, std::size_t sampleCount)
{
    std::size_t source = 0;
    while (source < sampleCount)
    {
        std::unique_lock<std::mutex> lock(s_mutex);
        s_condition.wait(lock, [] { return s_stop || s_count < kRingSamples; });
        if (s_stop)
            return false;

        const std::size_t freeSamples = kRingSamples - s_count;
        const std::size_t contiguous = std::min(freeSamples, kRingSamples - s_write);
        const std::size_t copyCount = std::min(contiguous, sampleCount - source);
        std::copy_n(samples + source, copyCount, s_ring.data() + s_write);
        s_write = (s_write + copyCount) % kRingSamples;
        s_count += copyCount;
        source += copyCount;
    }
    return true;
}

bool validatePcm(const std::string &path)
{
    // Headerless s16le mono 22050: size is the only thing to check (an
    // odd trailing byte is dropped rather than read as a half sample, but a
    // whole-file odd size still marks a truncated asset -- same gate as
    // before).
    AssetReader reader;
    return reader.open(path) && reader.extent >= 2 && (reader.extent & 1u) == 0u;
}

bool validateAdp(const std::string &path)
{
    AssetReader reader;
    if (!reader.open(path))
        return false;

    std::uint8_t headerBytes[Ps2AdpcmStream::kHeaderBytes]{};
    if (reader.read(headerBytes, sizeof(headerBytes)) != sizeof(headerBytes))
        return false;

    Ps2AdpcmStream::Header header{};
    // probe() compares the header's promised block count against the whole
    // file size; only the 16 header bytes are actually read.
    return AdpAssetDecode::probe(headerBytes, reader.extent, header);
}

void decoderPcmThread(std::string path)
{
    AssetReader reader;
    if (!reader.open(path))
    {
        markEofAndMaybeStop();
        return;
    }
    std::size_t remainingBytes = reader.extent;

    constexpr std::size_t kChunkSamples = 2048;
    std::array<std::int16_t, kChunkSamples> rawSamples{};
    std::array<float, kChunkSamples * 4> stereo{};

    while (remainingBytes >= sizeof(std::int16_t))
    {
        const std::size_t toRead = std::min(remainingBytes / sizeof(std::int16_t), kChunkSamples);
        const std::size_t readCount = reader.read(rawSamples.data(), toRead * sizeof(std::int16_t))
                                      / sizeof(std::int16_t);
        if (readCount == 0)
            break;
        remainingBytes -= readCount * sizeof(std::int16_t);

        // Convert 22050 mono to 44100 stereo (same sample on both ears,
        // frame-doubled -- the 3DS backend's own reading of these files).
        for (std::size_t i = 0; i < readCount; ++i)
        {
            const float s = rawSamples[i] / 32768.0f;
            const std::size_t outBase = i * 4;
            stereo[outBase + 0] = s;
            stereo[outBase + 1] = s;
            stereo[outBase + 2] = s;
            stereo[outBase + 3] = s;
        }

        if (!ringWrite(stereo.data(), readCount * 4))
            return; // stopped mid-write
    }

    markEofAndMaybeStop();
}

void decoderAdpThread(std::string path)
{
    AssetReader reader;
    std::uint8_t headerBytes[Ps2AdpcmStream::kHeaderBytes]{};

    Ps2AdpcmStream::Header header{};
    const bool opened = reader.open(path) &&
        reader.read(headerBytes, sizeof(headerBytes)) == sizeof(headerBytes) &&
        AdpAssetDecode::probe(headerBytes, reader.extent, header);
    if (!opened)
    {
        markEofAndMaybeStop();
        return;
    }

    using namespace Ps2AdpcmStream;
    const bool stereo = header.channels == 2;
    const std::uint32_t totalBlocks =
        (header.samplesPerChannel + kSamplesPerBlock - 1u) / kSamplesPerBlock;
    // Planar layout: the left plane starts after the header, the right
    // channel's plane after the left plane's last block.
    const std::size_t leftBase = kHeaderBytes;
    const std::size_t rightBase = kHeaderBytes +
        static_cast<std::size_t>(totalBlocks) * kBlockBytes;

    ChannelState leftState{};
    ChannelState rightState{};
    std::array<std::uint8_t, kBlockBytes> block{};
    std::array<std::int16_t, kSamplesPerBlock> leftPcm{};
    std::array<std::int16_t, kSamplesPerBlock> rightPcm{};

    // Batches of whole blocks keep the planar seeks linear-ish per rate
    // conversion; 32 blocks = 896 source frames per push. Capacity holds one
    // extra block so the threshold check can run after appending.
    constexpr std::size_t kChunkBlocks = 32;
    std::array<std::int16_t, (kChunkBlocks + 1) * kSamplesPerBlock * 2> chunkS16{};
    std::vector<float> chunkFloat;
    std::size_t chunkFrames = 0;

    const auto flushChunk = [&]() -> bool
    {
        if (chunkFrames == 0)
            return true;
        AudioConvert::s16ToFloatStereo(chunkS16.data(), chunkFrames,
                                       header.channels, header.sampleRate,
                                       kOutputSampleRate, chunkFloat);
        chunkFrames = 0;
        return chunkFloat.empty() || ringWrite(chunkFloat.data(), chunkFloat.size());
    };

    for (std::uint32_t emitted = 0; emitted < header.samplesPerChannel;)
    {
        const std::uint32_t blockIndex = emitted / kSamplesPerBlock;
        if (!reader.seek(leftBase + static_cast<std::size_t>(blockIndex) * kBlockBytes) ||
            reader.read(block.data(), kBlockBytes) != kBlockBytes ||
            !decodeBlock(block.data(), leftState, leftPcm.data()))
        {
            flushChunk();
            markEofAndMaybeStop();
            return;
        }
        if (stereo)
        {
            if (!reader.seek(rightBase + static_cast<std::size_t>(blockIndex) * kBlockBytes) ||
                reader.read(block.data(), kBlockBytes) != kBlockBytes ||
                !decodeBlock(block.data(), rightState, rightPcm.data()))
            {
                flushChunk();
                markEofAndMaybeStop();
                return;
            }
        }

        const std::uint32_t blockFrames =
            std::min<std::uint32_t>(kSamplesPerBlock, header.samplesPerChannel - emitted);
        for (std::uint32_t frame = 0; frame < blockFrames; ++frame)
        {
            const std::size_t base = (chunkFrames + frame) * header.channels;
            chunkS16[base] = leftPcm[frame];
            if (stereo)
                chunkS16[base + 1] = rightPcm[frame];
        }
        chunkFrames += blockFrames;
        emitted += blockFrames;

        if (chunkFrames >= kChunkBlocks * kSamplesPerBlock && !flushChunk())
            return; // stopped mid-write
    }

    if (flushChunk())
        markEofAndMaybeStop();
}
}

namespace PcMusicStream
{
bool start(const std::string &path)
{
    const bool isPcm = audioPathHasExtension(path, ".pcm");
    const bool isAdp = audioPathHasExtension(path, ".adp");

    if (isPcm)
    {
        if (!validatePcm(path))
            return false;
    }
    else if (isAdp)
    {
        if (!validateAdp(path))
            return false;
    }
    else
    {
        return false;
    }

    stop();
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_read = 0;
        s_write = 0;
        s_count = 0;
        s_stop = false;
        s_eof = false;
        s_running = true;
    }
    s_thread = isPcm ? std::thread(decoderPcmThread, path) : std::thread(decoderAdpThread, path);
    {
        std::unique_lock<std::mutex> lock(s_mutex);
        s_condition.wait_for(lock, std::chrono::milliseconds(50), [] { return s_count >= 4096 || s_eof || s_stop; });
    }
    return true;
}

void stop()
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_stop = true;
    }
    s_condition.notify_all();
    if (s_thread.joinable())
        s_thread.join();
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_read = 0;
        s_write = 0;
        s_count = 0;
        s_stop = false;
        s_eof = false;
        s_running = false;
    }
}

bool active()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_running || s_count > 0;
}

void mix(float *stereoOutput, int frames, float volume)
{
    if (stereoOutput == nullptr || frames <= 0 || volume <= 0.0f)
        return;

    std::lock_guard<std::mutex> lock(s_mutex);
    const std::size_t requested = static_cast<std::size_t>(frames) * 2u;
    const std::size_t available = std::min(requested, s_count);
    for (std::size_t i = 0; i < available; ++i)
    {
        stereoOutput[i] += s_ring[s_read] * volume;
        s_read = (s_read + 1) % kRingSamples;
    }
    s_count -= available;
    if (s_eof && s_count == 0)
        s_running = false;
    s_condition.notify_one();
}
}
