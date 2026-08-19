// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/media/opus/Timeline.h>

#include "test/unit/media/ogg/TestOgg.h"
#include <ao/Error.h>
#include <ao/media/ogg/Demuxer.h>
#include <ao/media/ogg/PageLayout.h>
#include <ao/media/opus/Header.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ao::media::opus::test
{
  namespace
  {
    using ao::test::ogg::makeStream;
    using ao::test::ogg::Page;

    constexpr std::int64_t kPreSkip = 312;

    // One 60 ms SILK frame, the packet shape the audio pages below carry.
    constexpr std::uint8_t kSixtyMillisecondToc = 0x18;
    constexpr std::int64_t kSixtyMillisecondFrames = 2880;

    std::vector<std::uint8_t> makeHeadPacket()
    {
      auto packet = std::vector<std::uint8_t>{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
      packet.push_back(1);
      packet.push_back(2);
      packet.push_back(static_cast<std::uint8_t>(kPreSkip & 0xFFU));
      packet.push_back(static_cast<std::uint8_t>((kPreSkip >> 8U) & 0xFFU));
      packet.insert(packet.end(), {0x80, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00});
      return packet;
    }

    std::vector<std::uint8_t> makeAudioPacket(std::uint8_t toc = kSixtyMillisecondToc)
    {
      auto packet = std::vector<std::uint8_t>(24, 0x00);
      packet[0] = toc;
      return packet;
    }

    Head parsedHead()
    {
      auto const packet = makeHeadPacket();
      auto bytes = std::vector<std::byte>{};

      for (auto const value : packet)
      {
        bytes.push_back(static_cast<std::byte>(value));
      }

      auto headRes = parseHead(bytes);
      REQUIRE(headRes);
      return *headRes;
    }

    // One packet per audio page, each page declaring the granule position the
    // caller gives it, so a test can name any origin or end trim it wants.
    std::vector<std::byte> makeStreamBytes(std::span<std::int64_t const> audioGranules, bool closeStream = true)
    {
      auto pages = std::vector<Page>{};
      std::uint32_t sequence = 0;

      pages.push_back(Page{.headerType = ogg::kBeginOfStreamFlag,
                           .granulePosition = 0,
                           .pageSequence = sequence++,
                           .lacingValues = ao::test::ogg::lacingFor({makeHeadPacket().size()}),
                           .payload = makeHeadPacket()});

      auto tags = std::vector<std::uint8_t>{'O', 'p', 'u', 's', 'T', 'a', 'g', 's', 0, 0, 0, 0, 0, 0, 0, 0};
      pages.push_back(Page{.granulePosition = 0,
                           .pageSequence = sequence++,
                           .lacingValues = ao::test::ogg::lacingFor({tags.size()}),
                           .payload = tags});

      for (std::size_t index = 0; index < audioGranules.size(); ++index)
      {
        auto const isLast = index + 1 == audioGranules.size();
        pages.push_back(Page{.headerType = isLast && closeStream ? ogg::kEndOfStreamFlag : std::uint8_t{0},
                             .granulePosition = audioGranules[index],
                             .pageSequence = sequence++,
                             .lacingValues = ao::test::ogg::lacingFor({makeAudioPacket().size()}),
                             .payload = makeAudioPacket()});
      }

      return makeStream(pages);
    }

    Timeline requireTimeline(std::span<std::byte const> bytes)
    {
      auto const demuxerRes = ogg::Demuxer::parse(bytes);
      REQUIRE(demuxerRes);
      auto const timelineRes = deriveOggTimeline(*demuxerRes, parsedHead());
      REQUIRE(timelineRes);
      return *timelineRes;
    }

    Error::Code timelineError(std::span<std::byte const> bytes)
    {
      auto const demuxerRes = ogg::Demuxer::parse(bytes);
      REQUIRE(demuxerRes);
      auto const timelineRes = deriveOggTimeline(*demuxerRes, parsedHead());
      REQUIRE_FALSE(timelineRes);
      return timelineRes.error().code;
    }
  } // namespace

  TEST_CASE("packetSampleCount reads the table of contents", "[media][unit][opus]")
  {
    SECTION("SILK configurations carry 10, 20, 40, or 60 ms")
    {
      CHECK(packetSampleCount(ao::test::ogg::toBytes(makeAudioPacket(0x00))) == 480);
      CHECK(packetSampleCount(ao::test::ogg::toBytes(makeAudioPacket(0x08))) == 960);
      CHECK(packetSampleCount(ao::test::ogg::toBytes(makeAudioPacket(0x10))) == 1920);
      CHECK(packetSampleCount(ao::test::ogg::toBytes(makeAudioPacket(0x18))) == 2880);
    }

    SECTION("Hybrid configurations carry 10 or 20 ms")
    {
      CHECK(packetSampleCount(ao::test::ogg::toBytes(makeAudioPacket(0x60))) == 480);
      CHECK(packetSampleCount(ao::test::ogg::toBytes(makeAudioPacket(0x68))) == 960);
    }

    SECTION("CELT configurations carry 2.5, 5, 10, or 20 ms")
    {
      CHECK(packetSampleCount(ao::test::ogg::toBytes(makeAudioPacket(0x80))) == 120);
      CHECK(packetSampleCount(ao::test::ogg::toBytes(makeAudioPacket(0x88))) == 240);
      CHECK(packetSampleCount(ao::test::ogg::toBytes(makeAudioPacket(0x90))) == 480);
      CHECK(packetSampleCount(ao::test::ogg::toBytes(makeAudioPacket(0x98))) == 960);
    }

    SECTION("The frame count code multiplies the frame length")
    {
      // Codes 1 and 2 both mean two frames; code 3 spells the count out.
      CHECK(packetSampleCount(ao::test::ogg::toBytes(makeAudioPacket(0x19))) == 2 * kSixtyMillisecondFrames);
      CHECK(packetSampleCount(ao::test::ogg::toBytes(makeAudioPacket(0x1A))) == 2 * kSixtyMillisecondFrames);

      auto packet = makeAudioPacket(0x83);
      packet[1] = 4;
      CHECK(packetSampleCount(ao::test::ogg::toBytes(packet)) == 480);
    }

    SECTION("Packets that declare nothing usable are rejected")
    {
      CHECK_FALSE(packetSampleCount({}));

      auto zeroFrames = makeAudioPacket(0x83);
      zeroFrames[1] = 0;
      CHECK_FALSE(packetSampleCount(ao::test::ogg::toBytes(zeroFrames)));

      // 48 frames of 60 ms is far past the 120 ms an Opus packet may hold.
      auto tooLong = makeAudioPacket(0x1B);
      tooLong[1] = 48;
      CHECK_FALSE(packetSampleCount(ao::test::ogg::toBytes(tooLong)));

      auto const truncated = std::vector<std::uint8_t>{0x1B};
      CHECK_FALSE(packetSampleCount(ao::test::ogg::toBytes(truncated)));
    }
  }

  TEST_CASE("deriveOggTimeline places a stream on its own timeline", "[media][unit][opus]")
  {
    SECTION("A stream starting at zero begins decoding there")
    {
      auto const granules = std::vector<std::int64_t>{kSixtyMillisecondFrames, 2 * kSixtyMillisecondFrames};
      auto const timeline = requireTimeline(makeStreamBytes(granules));

      CHECK(timeline.decodeStartGranule == 0);
      CHECK(timeline.playbackStartGranule == kPreSkip);
      REQUIRE(timeline.optTotalFrames);
      CHECK(std::cmp_equal(*timeline.optTotalFrames, (2 * kSixtyMillisecondFrames) - kPreSkip));
    }

    SECTION("A cropped stream begins decoding at the granule its first page names")
    {
      // The first audio page ends at 100000 while carrying 2880 samples, so
      // everything before 97120 was cropped away without renumbering.
      constexpr std::int64_t kOrigin = 97120;
      auto const granules =
        std::vector<std::int64_t>{kOrigin + kSixtyMillisecondFrames, kOrigin + (2 * kSixtyMillisecondFrames)};
      auto const timeline = requireTimeline(makeStreamBytes(granules));

      CHECK(timeline.decodeStartGranule == kOrigin);
      CHECK(timeline.playbackStartGranule == kOrigin + kPreSkip);
      REQUIRE(timeline.optTotalFrames);
      CHECK(std::cmp_equal(*timeline.optTotalFrames, (2 * kSixtyMillisecondFrames) - kPreSkip));
    }

    SECTION("A stream whose end meets its pre-skip exactly holds zero frames")
    {
      // Known and zero, which is not the same as unknown: a reader must reach
      // end of stream rather than emit what the packet decodes to.
      auto const granules = std::vector<std::int64_t>{kPreSkip};
      auto const timeline = requireTimeline(makeStreamBytes(granules));

      CHECK(timeline.decodeStartGranule == 0);
      REQUIRE(timeline.optTotalFrames);
      CHECK(*timeline.optTotalFrames == 0);
    }

    SECTION("A stream that never closed leaves its length unknown")
    {
      auto const granules = std::vector<std::int64_t>{kSixtyMillisecondFrames, 2 * kSixtyMillisecondFrames};
      auto const timeline = requireTimeline(makeStreamBytes(granules, false));

      CHECK(timeline.decodeStartGranule == 0);
      CHECK_FALSE(timeline.optTotalFrames);
    }

    SECTION("A single audio page that also ends the stream is trimmed, not cropped")
    {
      // The page decodes 2880 samples but declares 1000, which only the last
      // page may do. That is an end trim, so the origin stays at zero.
      auto const granules = std::vector<std::int64_t>{1000};
      auto const timeline = requireTimeline(makeStreamBytes(granules));

      CHECK(timeline.decodeStartGranule == 0);
      CHECK(timeline.playbackStartGranule == kPreSkip);
      REQUIRE(timeline.optTotalFrames);
      CHECK(std::cmp_equal(*timeline.optTotalFrames, 1000 - kPreSkip));
    }

    SECTION("A non-final first audio page cannot declare fewer samples than it completes")
    {
      auto const granules = std::vector<std::int64_t>{1000, 2 * kSixtyMillisecondFrames};
      CHECK(timelineError(makeStreamBytes(granules)) == Error::Code::CorruptData);
    }

    SECTION("A closed stream ending before its pre-skip is corrupt")
    {
      auto const granules = std::vector<std::int64_t>{kPreSkip - 1};
      CHECK(timelineError(makeStreamBytes(granules)) == Error::Code::CorruptData);
    }

    SECTION("An audio packet with an unusable table of contents is corrupt")
    {
      auto pages = std::vector<Page>{};
      auto const head = makeHeadPacket();
      pages.push_back(Page{.headerType = ogg::kBeginOfStreamFlag,
                           .granulePosition = 0,
                           .lacingValues = ao::test::ogg::lacingFor({head.size()}),
                           .payload = head});
      auto tags = std::vector<std::uint8_t>{'O', 'p', 'u', 's', 'T', 'a', 'g', 's', 0, 0, 0, 0, 0, 0, 0, 0};
      pages.push_back(Page{.granulePosition = 0,
                           .pageSequence = 1,
                           .lacingValues = ao::test::ogg::lacingFor({tags.size()}),
                           .payload = tags});

      // Code 3 with no second byte cannot state how many frames it holds.
      auto const stunted = std::vector<std::uint8_t>{0x1B};
      pages.push_back(Page{.headerType = ogg::kEndOfStreamFlag,
                           .granulePosition = 4800,
                           .pageSequence = 2,
                           .lacingValues = ao::test::ogg::lacingFor({stunted.size()}),
                           .payload = stunted});

      CHECK(timelineError(makeStream(pages)) == Error::Code::CorruptData);
    }
  }
} // namespace ao::media::opus::test
