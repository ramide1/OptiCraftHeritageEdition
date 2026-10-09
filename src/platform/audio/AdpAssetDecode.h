#pragma once

// Whole-file SPU2-ADPCM (.adp) decode shared by the software-mixing
// backends (PC's OpenAL mixer, Wii's ASND SFX cache). Stereo files are
// planar -- every block of the left channel first, then the right channel's
// blocks (the layout the PS2 streamer seeks through) -- and come out
// interleaved. The header's loop flag is ignored: a one-shot plays exactly
// samplesPerChannel frames (the software counterpart of the silent tail the
// PS2 writes so the SPU2's hardware decoder stops on ENDX).

#include <algorithm>
#include <cstdint>
#include <vector>

#include "platform/audio/Ps2AdpcmStreamDecoder.h"

namespace AdpAssetDecode
{
// Header parse plus the "file must hold every block the header promises"
// check (the same gate the PS2/3DS streamers apply). `size` is the whole
// file's byte count; the data pointer only needs the 16 header bytes.
inline bool probe(const std::uint8_t *data, std::size_t size,
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

// Whole decode -> interleaved s16. `size` is the whole file's byte count.
// Returns the per-channel frame count, 0 on failure.
inline std::uint32_t decode(const std::uint8_t *data, std::size_t size,
                            const Ps2AdpcmStream::Header &header,
                            std::vector<std::int16_t> &out)
{
    out.clear();
    using namespace Ps2AdpcmStream;

    const std::uint64_t blocks64 =
        (static_cast<std::uint64_t>(header.samplesPerChannel) + kSamplesPerBlock - 1u) /
        kSamplesPerBlock;
    const std::uint64_t required =
        kHeaderBytes + blocks64 * kBlockBytes * static_cast<std::uint64_t>(header.channels);
    if (required > size)
        return 0;

    const std::uint32_t totalBlocks = static_cast<std::uint32_t>(blocks64);
    const bool stereo = header.channels == 2;

    out.assign(static_cast<std::size_t>(header.samplesPerChannel) * header.channels, 0);

    ChannelState leftState{};
    ChannelState rightState{};
    std::int16_t leftPcm[kSamplesPerBlock];
    std::int16_t rightPcm[kSamplesPerBlock];

    for (std::uint32_t emitted = 0; emitted < header.samplesPerChannel;)
    {
        const std::uint32_t blockIndex = emitted / kSamplesPerBlock;
        if (!decodeBlock(data + kHeaderBytes +
                             static_cast<std::size_t>(blockIndex) * kBlockBytes,
                         leftState, leftPcm))
            return 0;
        if (stereo && !decodeBlock(data + kHeaderBytes +
                                       (static_cast<std::size_t>(totalBlocks) + blockIndex) * kBlockBytes,
                                   rightState, rightPcm))
            return 0;

        const std::uint32_t blockFrames =
            std::min<std::uint32_t>(kSamplesPerBlock, header.samplesPerChannel - emitted);
        for (std::uint32_t frame = 0; frame < blockFrames; ++frame)
        {
            out[(emitted + frame) * header.channels] = leftPcm[frame];
            if (stereo)
                out[(emitted + frame) * header.channels + 1] = rightPcm[frame];
        }
        emitted += blockFrames;
    }

    return header.samplesPerChannel;
}
}
