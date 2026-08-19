// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/media/opus/Timeline.h>

#include <ao/Error.h>
#include <ao/media/ogg/Demuxer.h>
#include <ao/media/opus/Header.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ao::media::opus
{
  namespace
  {
    // Table-of-contents fields. The low two bits give the frame count, the top
    // five select a configuration, and within a configuration the two bits
    // above the mode select the frame length.
    constexpr std::uint8_t kFrameCountCodeMask = 0x03;
    constexpr std::uint8_t kArbitraryFrameCountMask = 0x3F;
    constexpr std::uint8_t kCeltConfigFlag = 0x80;
    constexpr std::uint8_t kHybridConfigMask = 0x60;
    constexpr std::uint8_t kHybridLongFrameFlag = 0x08;
    constexpr std::uint8_t kFrameLengthShift = 3;
    constexpr std::uint8_t kFrameLengthMask = 0x03;
    constexpr std::uint8_t kLongestSilkFrameLength = 3;

    // Frame lengths at kDecodedSampleRate. CELT starts at 2.5 ms and SILK at
    // 10 ms, each doubling with the frame-length bits, except that SILK's
    // longest step is 60 ms rather than 80 ms.
    constexpr std::int32_t kCeltShortestFrames = 120;
    constexpr std::int32_t kTenMillisecondFrames = 480;
    constexpr std::int32_t kTwentyMillisecondFrames = 960;
    constexpr std::int32_t kSixtyMillisecondFrames = 2880;

    // Frames the packet holds, from the two frame-count bits of the table of
    // contents. Code 3 spells the count out in a second byte.
    std::optional<std::int32_t> tocFrameCount(std::span<std::byte const> packet) noexcept
    {
      switch (static_cast<std::uint8_t>(packet[0]) & kFrameCountCodeMask)
      {
        case 0: return 1;
        case 1:
        case 2: return 2;
        default: break;
      }

      if (packet.size() < 2)
      {
        return std::nullopt;
      }

      return static_cast<std::int32_t>(static_cast<std::uint8_t>(packet[1]) & kArbitraryFrameCountMask);
    }

    // Samples one frame decodes to at kDecodedSampleRate. The configuration
    // number in the top five bits selects the mode, and within each mode the
    // low bits select the frame duration.
    std::int32_t tocFrameSampleCount(std::uint8_t toc) noexcept
    {
      auto const length = static_cast<std::uint8_t>((toc >> kFrameLengthShift) & kFrameLengthMask);

      if ((toc & kCeltConfigFlag) != 0)
      {
        // CELT: 2.5, 5, 10, or 20 ms.
        return kCeltShortestFrames << length;
      }

      if ((toc & kHybridConfigMask) == kHybridConfigMask)
      {
        // Hybrid: 10 or 20 ms.
        return (toc & kHybridLongFrameFlag) != 0 ? kTwentyMillisecondFrames : kTenMillisecondFrames;
      }

      // SILK: 10, 20, 40, or 60 ms.
      return length == kLongestSilkFrameLength ? kSixtyMillisecondFrames : kTenMillisecondFrames << length;
    }

    // Page group the first audio packet completes in, which is the group that
    // establishes the timeline origin.
    std::optional<ogg::Demuxer::PageGroup> firstAudioPageGroup(ogg::Demuxer const& demuxer) noexcept
    {
      for (std::size_t index = 0; index < demuxer.pageGroupCount(); ++index)
      {
        if (auto const group = demuxer.pageGroup(index);
            kFirstAudioPacketIndex < group.firstPacketIndex + group.packetCount)
        {
          return group;
        }
      }

      return std::nullopt;
    }
  } // namespace

  std::optional<std::int32_t> packetSampleCount(std::span<std::byte const> packet) noexcept
  {
    if (packet.empty())
    {
      return std::nullopt;
    }

    auto const optFrames = tocFrameCount(packet);

    if (!optFrames || *optFrames < 1)
    {
      return std::nullopt;
    }

    auto const samples = *optFrames * tocFrameSampleCount(static_cast<std::uint8_t>(packet[0]));

    // An Opus packet may hold at most 120 ms, which bounds any frame count the
    // second byte can spell out.
    if (samples > kMaxPacketFrames)
    {
      return std::nullopt;
    }

    return samples;
  }

  Result<Timeline> deriveOggTimeline(ogg::Demuxer const& demuxer, Head const& head)
  {
    auto const optGroup = firstAudioPageGroup(demuxer);

    if (!optGroup || demuxer.packetCount() <= kFirstAudioPacketIndex)
    {
      return makeError(Error::Code::CorruptData, "ogg opus stream carries no audio packets");
    }

    // Everything this page completes from the first audio packet onward is what
    // its granule position accounts for beyond the origin.
    auto const groupEnd = optGroup->firstPacketIndex + optGroup->packetCount;
    std::int64_t pageSamples = 0;

    for (auto index = kFirstAudioPacketIndex; index < groupEnd; ++index)
    {
      auto const optSamples = packetSampleCount(demuxer.packet(index).bytes);

      if (!optSamples)
      {
        return makeError(Error::Code::CorruptData, "opus packet declares an unusable table of contents");
      }

      pageSamples += *optSamples;
    }

    auto timeline = Timeline{};

    if (optGroup->granulePosition < pageSamples)
    {
      // Only a first audio page that is also the final page may use a smaller
      // granule position to trim the decoded packet tail. On any earlier page,
      // the same shape cannot name a valid decode origin.
      if (!optGroup->endsOnEndOfStreamPage)
      {
        return makeError(
          Error::Code::CorruptData, "first non-EOS opus audio page ends before the samples it completes");
      }

      timeline.decodeStartGranule = 0;
    }
    else
    {
      timeline.decodeStartGranule = optGroup->granulePosition - pageSamples;
    }

    timeline.playbackStartGranule = timeline.decodeStartGranule + head.preSkip;

    auto const finalGranulePosition = demuxer.finalGranulePosition();

    // Only a stream that reached its declared end states a length. Anything
    // that stopped early leaves the total unknown so a reader still decodes the
    // packets it does have.
    if (finalGranulePosition == ogg::Demuxer::kUnsetGranulePosition || demuxer.hasIncompleteTail())
    {
      return timeline;
    }

    if (finalGranulePosition < timeline.playbackStartGranule)
    {
      return makeError(Error::Code::CorruptData, "opus stream ends before its own pre-skip completes");
    }

    timeline.optTotalFrames = static_cast<std::uint64_t>(finalGranulePosition - timeline.playbackStartGranule);
    return timeline;
  }
} // namespace ao::media::opus
