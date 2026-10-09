#pragma once

// Shared PCM plumbing for the platform SoundManager backends:
//
//   * audioWavDecodeS16       -- RIFF/WAVE blob -> interleaved s16 at the
//     file's own rate/channels. Covers what scripts/ogg to wav emits for
//     the consoles' asset tree (mono 22050 s16le) plus the plain PCM/float
//     shapes a legacy desktop sound pack may carry (u8/s16/s24/s32 PCM,
//     f32/f64 IEEE, WAVE_FORMAT_EXTENSIBLE wrapping either). Anything
//     outside that envelope (ADPCM tags, missing chunks, ...) returns
//     false so the caller can fall through to another loader.
//   * audioS16ToFloatStereo   -- interleaved s16 -> interleaved float
//     stereo at a fixed output rate (44100 on PC), with channel mapping
//     (mono duplicates, >2 channels fold even->L / odd->R) and linear
//     resampling. This is the conversion every one-shot loader routes its
//     decoded PCM through before it enters the platform mixer.
//
// Header-only, pure std -- safe to compile into every platform even where
// nobody includes it.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace AudioConvert
{
namespace detail
{
inline std::uint16_t readLe16(const std::uint8_t *p)
{
    return static_cast<std::uint16_t>(p[0]) |
           static_cast<std::uint16_t>(p[1] << 8);
}

inline std::uint32_t readLe32(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

// One PCM sample at p (width bytes, signed/unsigned per format) -> float.
inline float pcmIntSample(const std::uint8_t *p, int widthBytes, bool unsigned8)
{
    if (widthBytes == 1)
        return unsigned8
            ? (static_cast<int>(*p) - 128) / 128.0f
            : static_cast<std::int8_t>(*p) / 128.0f;
    if (widthBytes == 2)
    {
        const std::int16_t v = static_cast<std::int16_t>(readLe16(p));
        return v / 32768.0f;
    }
    if (widthBytes == 3)
    {
        std::int32_t v = static_cast<std::int32_t>(p[0]) |
                         (static_cast<std::int32_t>(p[1]) << 8) |
                         (static_cast<std::int32_t>(p[2]) << 16);
        if ((v & 0x800000) != 0)
            v -= 0x1000000;
        return v / 8388608.0f;
    }
    // 4-byte
    const std::int32_t v = static_cast<std::int32_t>(readLe32(p));
    return static_cast<float>(static_cast<double>(v) / 2147483648.0);
}
}

// RIFF/WAVE -> interleaved s16 (native rate/channels). Returns false when
// the blob is not a plain PCM/float WAVE.
inline bool wavDecodeS16(const std::uint8_t *data, std::size_t size,
                         std::vector<std::int16_t> &out,
                         int &channelsOut, int &rateOut)
{
    out.clear();
    if (data == nullptr || size < 12 ||
        data[0] != 'R' || data[1] != 'I' || data[2] != 'F' || data[3] != 'F' ||
        data[8] != 'W' || data[9] != 'A' || data[10] != 'V' || data[11] != 'E')
        return false;

    int formatTag = 0;
    int channels = 0;
    int sampleRate = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint16_t blockAlign = 0;
    const std::uint8_t *pcmData = nullptr;
    std::size_t pcmBytes = 0;

    std::size_t pos = 12;
    while (pos + 8 <= size)
    {
        const std::uint32_t chunkSize = detail::readLe32(data + pos + 4);
        const std::uint8_t *chunkData = data + pos + 8;
        if (chunkSize > size - pos - 8)
            return false; // truncated

        const bool isFmt  = data[pos + 0] == 'f' && data[pos + 1] == 'm' &&
                            data[pos + 2] == 't' && data[pos + 3] == ' ';
        const bool isData = data[pos + 0] == 'd' && data[pos + 1] == 'a' &&
                            data[pos + 2] == 't' && data[pos + 3] == 'a';

        if (isFmt && chunkSize >= 16)
        {
            formatTag     = detail::readLe16(chunkData + 0);
            channels      = detail::readLe16(chunkData + 2);
            sampleRate    = static_cast<int>(detail::readLe32(chunkData + 4));
            blockAlign    = detail::readLe16(chunkData + 12);
            bitsPerSample = detail::readLe16(chunkData + 14);

            if (formatTag == 0xFFFE && chunkSize >= 40) // WAVE_FORMAT_EXTENSIBLE
                formatTag = detail::readLe16(chunkData + 24); // SubFormat tag
        }
        else if (isData)
        {
            pcmData  = chunkData;
            pcmBytes = chunkSize;
        }

        pos += 8 + chunkSize + (chunkSize & 1u); // chunks pad to even sizes
    }

    const bool isPcm   = formatTag == 1; // integer PCM (u8, s16, s24, s32)
    const bool isFloat = formatTag == 3; // IEEE float (f32, f64)
    const int bytesPerSample = bitsPerSample / 8;
    if ((!isPcm && !isFloat) || pcmData == nullptr ||
        channels < 1 || sampleRate < 1 ||
        (isPcm && (bytesPerSample < 1 || bytesPerSample > 4)) ||
        (isFloat && bytesPerSample != 4 && bytesPerSample != 8) ||
        blockAlign < channels * bytesPerSample)
        return false;

    const std::size_t frames = pcmBytes / blockAlign;
    if (frames == 0)
        return false;

    out.resize(frames * static_cast<std::size_t>(channels));
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const std::uint8_t *frameBase = pcmData + frame * blockAlign;
        for (int ch = 0; ch < channels; ++ch)
        {
            const std::uint8_t *p = frameBase + static_cast<std::size_t>(ch) * bytesPerSample;
            float v;
            if (isFloat)
            {
                if (bytesPerSample == 4)
                {
                    const std::uint32_t bits = detail::readLe32(p);
                    std::memcpy(&v, &bits, sizeof(v));
                }
                else
                {
                    const std::uint64_t lo = detail::readLe32(p);
                    const std::uint64_t hi = detail::readLe32(p + 4);
                    const std::uint64_t bits = lo | (hi << 32);
                    double d;
                    std::memcpy(&d, &bits, sizeof(d));
                    v = static_cast<float>(d);
                }
            }
            else
            {
                v = detail::pcmIntSample(p, bytesPerSample, bytesPerSample == 1);
            }

            v = std::max(-1.0f, std::min(1.0f, v));
            long scaled = static_cast<long>(v * 32767.0f);
            if (scaled < -32768) scaled = -32768;
            if (scaled >  32767) scaled =  32767;
            out[frame * channels + ch] = static_cast<std::int16_t>(scaled);
        }
    }

    channelsOut = channels;
    rateOut = sampleRate;
    return true;
}

// Interleaved s16 (frames x channels, inRate) -> interleaved float stereo at
// outRate, naturalized with a linear resampler. Channel map: mono doubles,
// stereo passes, wider mixes fold even channels to L and odd to R.
inline void s16ToFloatStereo(const std::int16_t *samples, std::size_t frames,
                             int channels, int inRate,
                             int outRate, std::vector<float> &out)
{
    out.clear();
    if (samples == nullptr || frames == 0 || channels < 1 || inRate < 1 || outRate < 1)
        return;

    const std::size_t outFrames =
        static_cast<std::size_t>((static_cast<std::uint64_t>(frames) *
                                  static_cast<std::uint64_t>(outRate) +
                                  static_cast<std::uint64_t>(inRate) / 2u) /
                                 static_cast<std::uint64_t>(inRate));
    out.resize(outFrames * 2, 0.0f);

    const double step = static_cast<double>(inRate) / static_cast<double>(outRate);
    for (std::size_t i = 0; i < outFrames; ++i)
    {
        const double srcPos = static_cast<double>(i) * step;
        const std::size_t i0 = static_cast<std::size_t>(srcPos);
        const std::size_t i1 = i0 + 1 < frames ? i0 + 1 : frames - 1;
        const float t = static_cast<float>(srcPos - static_cast<double>(i0));

        float l0, r0, l1, r1;
        const std::int16_t *f0 = samples + i0 * channels;
        const std::int16_t *f1 = samples + i1 * channels;
        if (channels == 1)
        {
            l0 = r0 = f0[0] / 32768.0f;
            l1 = r1 = f1[0] / 32768.0f;
        }
        else
        {
            l0 = l1 = r0 = r1 = 0.0f;
            int leftCount = 0, rightCount = 0;
            for (int ch = 0; ch < channels; ++ch)
            {
                const float v0 = f0[ch] / 32768.0f;
                const float v1 = f1[ch] / 32768.0f;
                if ((ch & 1) == 0) { l0 += v0; l1 += v1; ++leftCount; }
                else               { r0 += v0; r1 += v1; ++rightCount; }
            }
            if (leftCount  > 0) { l0 /= leftCount;  l1 /= leftCount; }
            if (rightCount > 0) { r0 /= rightCount; r1 /= rightCount; }
        }

        out[i * 2 + 0] = l0 + (l1 - l0) * t;
        out[i * 2 + 1] = r0 + (r1 - r0) * t;
    }
}
}
