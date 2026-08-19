// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/media/ogg/Demuxer.h>

#include "TestOgg.h"
#include <ao/Error.h>
#include <ao/media/ogg/PageLayout.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace ao::media::ogg::test
{
  namespace
  {
    using ao::test::ogg::lacingFor;
    using ao::test::ogg::makeStream;
    using ao::test::ogg::Page;
    using ao::test::ogg::payloadFor;

    bool allBytesEqual(std::span<std::byte const> bytes, std::uint8_t const marker)
    {
      return std::ranges::all_of(bytes, [marker](std::byte const byte) { return byte == std::byte{marker}; });
    }
  } // namespace

  static_assert(!std::is_constructible_v<Demuxer, std::span<std::byte const>>);
  static_assert(std::is_nothrow_move_constructible_v<Demuxer>);
  static_assert(!std::is_move_assignable_v<Demuxer>);

  TEST_CASE("Ogg Demuxer - rejects content that cannot start a logical bitstream", "[media][unit][ogg][error]")
  {
    SECTION("Empty input is corrupt")
    {
      auto const result = Demuxer::parse({});
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("A missing capture pattern is corrupt")
    {
      auto const bytes = ao::test::ogg::toBytes(std::vector<std::uint8_t>(64, 0x41));
      auto const result = Demuxer::parse(bytes);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("A truncated first header is corrupt")
    {
      auto const page = std::array{Page{.headerType = kBeginOfStreamFlag, .lacingValues = lacingFor({4})}};
      auto const stream = makeStream(page);
      auto const result = Demuxer::parse(std::span{stream}.first(20));
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("An unsupported bitstream version is corrupt")
    {
      auto const pages = std::array{Page{
        .headerType = kBeginOfStreamFlag, .version = 1, .lacingValues = lacingFor({4}), .payload = payloadFor({4})}};
      auto const stream = makeStream(pages);
      auto const result = Demuxer::parse(stream);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("A first page that does not begin a bitstream is corrupt")
    {
      auto const pages = std::array{Page{.lacingValues = lacingFor({4}), .payload = payloadFor({4})}};
      auto const stream = makeStream(pages);
      auto const result = Demuxer::parse(stream);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }
  }

  TEST_CASE("Ogg Demuxer - a page cut short by the end of the file ends the bitstream", "[media][unit][ogg]")
  {
    auto const pages = std::array{Page{.headerType = kBeginOfStreamFlag,
                                       .granulePosition = 480,
                                       .lacingValues = lacingFor({4}),
                                       .payload = payloadFor({4})},
                                  Page{.headerType = kEndOfStreamFlag,
                                       .granulePosition = 960,
                                       .pageSequence = 1,
                                       .lacingValues = lacingFor({64}),
                                       .payload = payloadFor({64}, 2)}};
    auto const stream = makeStream(pages);

    // Cutting into the second page's payload must keep the first page's packet
    // rather than reject the whole file, the way an interrupted download reads.
    auto const demuxerRes = Demuxer::parse(std::span{stream}.first(stream.size() - 8));
    REQUIRE(demuxerRes);

    REQUIRE(demuxerRes->packetCount() == 1);
    CHECK(demuxerRes->packet(0).bytes.size() == 4);
    CHECK(demuxerRes->finalGranulePosition() == 480);
    CHECK(demuxerRes->hasIncompleteTail());
  }

  TEST_CASE("Ogg Demuxer - rejects a continuation flag that contradicts the packet in progress",
            "[media][unit][ogg][error]")
  {
    SECTION("A continuation without an open packet is corrupt")
    {
      auto const pages =
        std::array{Page{.headerType = kBeginOfStreamFlag, .lacingValues = lacingFor({4}), .payload = payloadFor({4})},
                   Page{.headerType = kContinuedPacketFlag,
                        .pageSequence = 1,
                        .lacingValues = lacingFor({4}),
                        .payload = payloadFor({4})}};
      auto const stream = makeStream(pages);
      auto const result = Demuxer::parse(stream);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("An open packet without a continuation flag is corrupt")
    {
      auto const pages = std::array{
        Page{.headerType = kBeginOfStreamFlag, .lacingValues = {255}, .payload = std::vector<std::uint8_t>(255, 1)},
        Page{.pageSequence = 1, .lacingValues = lacingFor({4}), .payload = payloadFor({4})}};
      auto const stream = makeStream(pages);
      auto const result = Demuxer::parse(stream);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }
  }

  TEST_CASE("Ogg Demuxer - rejects begin-of-stream on a later selected page", "[media][unit][ogg][error]")
  {
    auto const pages =
      std::array{Page{.headerType = kBeginOfStreamFlag, .lacingValues = lacingFor({4}), .payload = payloadFor({4})},
                 Page{.headerType = kBeginOfStreamFlag | kEndOfStreamFlag,
                      .granulePosition = 960,
                      .pageSequence = 1,
                      .lacingValues = lacingFor({5}),
                      .payload = payloadFor({5})}};

    auto const stream = makeStream(pages);
    auto const result = Demuxer::parse(stream);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::CorruptData);
  }

  TEST_CASE("Ogg Demuxer - recovers the packets of a single page", "[media][unit][ogg]")
  {
    auto const pages = std::array{Page{.headerType = kBeginOfStreamFlag | kEndOfStreamFlag,
                                       .granulePosition = 960,
                                       .lacingValues = lacingFor({4, 8, 2}),
                                       .payload = payloadFor({4, 8, 2})}};
    auto const stream = makeStream(pages);
    auto const demuxerRes = Demuxer::parse(stream);
    REQUIRE(demuxerRes);

    auto const& demuxer = *demuxerRes;
    REQUIRE(demuxer.packetCount() == 3);
    CHECK(demuxer.serialNumber() == 1);
    CHECK(demuxer.packet(0).bytes.size() == 4);
    CHECK(demuxer.packet(1).bytes.size() == 8);
    CHECK(demuxer.packet(2).bytes.size() == 2);
    CHECK(allBytesEqual(demuxer.packet(0).bytes, 1));
    CHECK(allBytesEqual(demuxer.packet(1).bytes, 2));
    CHECK(allBytesEqual(demuxer.packet(2).bytes, 3));

    // Only the last packet completed on a page carries that page's granule.
    CHECK(demuxer.packet(0).granulePosition == Demuxer::kUnsetGranulePosition);
    CHECK(demuxer.packet(1).granulePosition == Demuxer::kUnsetGranulePosition);
    CHECK(demuxer.packet(2).granulePosition == 960);
    CHECK(demuxer.finalGranulePosition() == 960);
    CHECK_FALSE(demuxer.hasIncompleteTail());
  }

  TEST_CASE("Ogg Demuxer - preserves a zero-length packet", "[media][unit][ogg]")
  {
    auto const pages = std::array{Page{.headerType = kBeginOfStreamFlag | kEndOfStreamFlag,
                                       .granulePosition = 480,
                                       .lacingValues = lacingFor({4, 0, 4}),
                                       .payload = payloadFor({4, 0, 4})}};
    auto const stream = makeStream(pages);
    auto const demuxerRes = Demuxer::parse(stream);
    REQUIRE(demuxerRes);

    REQUIRE(demuxerRes->packetCount() == 3);
    CHECK(demuxerRes->packet(1).bytes.empty());
    CHECK(demuxerRes->packet(2).bytes.size() == 4);
  }

  TEST_CASE("Ogg Demuxer - terminates a packet whose size is a multiple of the lacing limit", "[media][unit][ogg]")
  {
    auto const pages = std::array{Page{.headerType = kBeginOfStreamFlag | kEndOfStreamFlag,
                                       .granulePosition = 480,
                                       .lacingValues = lacingFor({255}),
                                       .payload = payloadFor({255})}};
    auto const stream = makeStream(pages);
    auto const demuxerRes = Demuxer::parse(stream);
    REQUIRE(demuxerRes);

    REQUIRE(demuxerRes->packetCount() == 1);
    CHECK(demuxerRes->packet(0).bytes.size() == 255);
    CHECK_FALSE(demuxerRes->hasIncompleteTail());
  }

  TEST_CASE("Ogg Demuxer - reassembles a packet that spans pages", "[media][unit][ogg]")
  {
    SECTION("Two pages")
    {
      auto const pages = std::array{
        Page{.headerType = kBeginOfStreamFlag, .lacingValues = {255}, .payload = std::vector<std::uint8_t>(255, 7)},
        Page{.headerType = kContinuedPacketFlag | kEndOfStreamFlag,
             .granulePosition = 960,
             .pageSequence = 1,
             .lacingValues = lacingFor({10}),
             .payload = std::vector<std::uint8_t>(10, 7)}};
      auto const stream = makeStream(pages);
      auto const demuxerRes = Demuxer::parse(stream);
      REQUIRE(demuxerRes);

      REQUIRE(demuxerRes->packetCount() == 1);
      CHECK(demuxerRes->packet(0).bytes.size() == 265);
      CHECK(allBytesEqual(demuxerRes->packet(0).bytes, 7));
      CHECK(demuxerRes->packet(0).granulePosition == 960);
      CHECK_FALSE(demuxerRes->hasIncompleteTail());
    }

    SECTION("A zero-segment page keeps the packet open")
    {
      auto const pages = std::array{
        Page{.headerType = kBeginOfStreamFlag, .lacingValues = {255}, .payload = std::vector<std::uint8_t>(255, 7)},
        Page{.headerType = kContinuedPacketFlag, .pageSequence = 1},
        Page{.headerType = kContinuedPacketFlag | kEndOfStreamFlag,
             .granulePosition = 960,
             .pageSequence = 2,
             .lacingValues = lacingFor({5}),
             .payload = std::vector<std::uint8_t>(5, 7)}};
      auto const stream = makeStream(pages);
      auto const demuxerRes = Demuxer::parse(stream);
      REQUIRE(demuxerRes);

      REQUIRE(demuxerRes->packetCount() == 1);
      CHECK(demuxerRes->packet(0).bytes.size() == 260);
      CHECK(allBytesEqual(demuxerRes->packet(0).bytes, 7));
    }

    SECTION("A spanning packet keeps its page-local neighbours contiguous")
    {
      auto openingLacing = lacingFor({4});
      openingLacing.push_back(255);
      auto openingPayload = payloadFor({4});
      openingPayload.insert(openingPayload.end(), 255, 9);

      auto const pages =
        std::array{Page{.headerType = kBeginOfStreamFlag, .lacingValues = openingLacing, .payload = openingPayload},
                   Page{.headerType = kContinuedPacketFlag | kEndOfStreamFlag,
                        .granulePosition = 960,
                        .pageSequence = 1,
                        .lacingValues = lacingFor({6, 3}),
                        .payload = payloadFor({6, 3}, 9)}};
      auto const stream = makeStream(pages);
      auto const demuxerRes = Demuxer::parse(stream);
      REQUIRE(demuxerRes);

      REQUIRE(demuxerRes->packetCount() == 3);
      CHECK(demuxerRes->packet(0).bytes.size() == 4);
      CHECK(allBytesEqual(demuxerRes->packet(0).bytes, 1));
      CHECK(demuxerRes->packet(1).bytes.size() == 261);
      CHECK(allBytesEqual(demuxerRes->packet(1).bytes, 9));
      CHECK(demuxerRes->packet(2).bytes.size() == 3);
      CHECK(allBytesEqual(demuxerRes->packet(2).bytes, 10));
      CHECK(demuxerRes->packet(2).granulePosition == 960);
    }
  }

  TEST_CASE("Ogg Demuxer - rejects a gap in the page sequence", "[media][unit][ogg]")
  {
    SECTION("A missing page between two complete packets is corrupt")
    {
      auto const pages = std::array{Page{.headerType = kBeginOfStreamFlag,
                                         .granulePosition = 480,
                                         .lacingValues = lacingFor({4}),
                                         .payload = payloadFor({4})},
                                    Page{.headerType = kEndOfStreamFlag,
                                         .granulePosition = 1440,
                                         .pageSequence = 2,
                                         .lacingValues = lacingFor({5}),
                                         .payload = payloadFor({5})}};
      auto const stream = makeStream(pages);
      auto const demuxerRes = Demuxer::parse(stream);
      REQUIRE_FALSE(demuxerRes);
      CHECK(demuxerRes.error().code == Error::Code::CorruptData);
    }

    SECTION("A missing page inside a spanning packet is corrupt")
    {
      auto const pages =
        std::array{Page{.headerType = kBeginOfStreamFlag, .lacingValues = {255}, .payload = payloadFor({255})},
                   Page{.headerType = kContinuedPacketFlag | kEndOfStreamFlag,
                        .granulePosition = 480,
                        .pageSequence = 3,
                        .lacingValues = lacingFor({6}),
                        .payload = payloadFor({6})}};
      auto const stream = makeStream(pages);
      auto const demuxerRes = Demuxer::parse(stream);
      REQUIRE_FALSE(demuxerRes);
      CHECK(demuxerRes.error().code == Error::Code::CorruptData);
    }

    SECTION("A sequence wrapping past the 32-bit maximum is continuous")
    {
      auto const pages = std::array{Page{.headerType = kBeginOfStreamFlag,
                                         .granulePosition = 480,
                                         .pageSequence = 0xFFFFFFFFU,
                                         .lacingValues = lacingFor({4}),
                                         .payload = payloadFor({4})},
                                    Page{.headerType = kEndOfStreamFlag,
                                         .granulePosition = 960,
                                         .pageSequence = 0,
                                         .lacingValues = lacingFor({5}),
                                         .payload = payloadFor({5})}};
      auto const stream = makeStream(pages);
      auto const demuxerRes = Demuxer::parse(stream);
      REQUIRE(demuxerRes);
      CHECK(demuxerRes->packetCount() == 2);
    }

    SECTION("An interleaved foreign serial does not break continuity")
    {
      auto const pages = std::array{Page{.headerType = kBeginOfStreamFlag,
                                         .granulePosition = 480,
                                         .lacingValues = lacingFor({4}),
                                         .payload = payloadFor({4})},
                                    Page{.headerType = kBeginOfStreamFlag,
                                         .serialNumber = 2,
                                         .pageSequence = 7,
                                         .lacingValues = lacingFor({6}),
                                         .payload = payloadFor({6})},
                                    Page{.headerType = kEndOfStreamFlag,
                                         .granulePosition = 960,
                                         .pageSequence = 1,
                                         .lacingValues = lacingFor({5}),
                                         .payload = payloadFor({5})}};
      auto const stream = makeStream(pages);
      auto const demuxerRes = Demuxer::parse(stream);
      REQUIRE(demuxerRes);
      CHECK(demuxerRes->packetCount() == 2);
    }
  }

  TEST_CASE("Ogg Demuxer - groups the packets completing on each page", "[media][unit][ogg]")
  {
    auto const pages = std::array{Page{.headerType = kBeginOfStreamFlag,
                                       .granulePosition = 960,
                                       .lacingValues = lacingFor({4, 5}),
                                       .payload = payloadFor({4, 5})},
                                  Page{.headerType = kEndOfStreamFlag,
                                       .granulePosition = 1920,
                                       .pageSequence = 1,
                                       .lacingValues = lacingFor({6}),
                                       .payload = payloadFor({6})}};
    auto const stream = makeStream(pages);
    auto const demuxerRes = Demuxer::parse(stream);
    REQUIRE(demuxerRes);

    REQUIRE(demuxerRes->pageGroupCount() == 2);

    auto const first = demuxerRes->pageGroup(0);
    CHECK(first.firstPacketIndex == 0);
    CHECK(first.packetCount == 2);
    CHECK(first.granulePosition == 960);
    CHECK_FALSE(first.endsOnEndOfStreamPage);

    auto const last = demuxerRes->pageGroup(1);
    CHECK(last.firstPacketIndex == 2);
    CHECK(last.packetCount == 1);
    CHECK(last.granulePosition == 1920);
    CHECK(last.endsOnEndOfStreamPage);
  }

  TEST_CASE("Ogg Demuxer - exposes only the first logical bitstream", "[media][unit][ogg]")
  {
    SECTION("Pages of a multiplexed serial are skipped")
    {
      auto const pages =
        std::array{Page{.headerType = kBeginOfStreamFlag, .lacingValues = lacingFor({4}), .payload = payloadFor({4})},
                   Page{.headerType = kBeginOfStreamFlag,
                        .serialNumber = 2,
                        .lacingValues = lacingFor({6}),
                        .payload = payloadFor({6})},
                   Page{.headerType = kEndOfStreamFlag,
                        .granulePosition = 480,
                        .pageSequence = 1,
                        .lacingValues = lacingFor({5}),
                        .payload = payloadFor({5})}};
      auto const stream = makeStream(pages);
      auto const demuxerRes = Demuxer::parse(stream);
      REQUIRE(demuxerRes);

      CHECK(demuxerRes->serialNumber() == 1);
      REQUIRE(demuxerRes->packetCount() == 2);
      CHECK(demuxerRes->packet(0).bytes.size() == 4);
      CHECK(demuxerRes->packet(1).bytes.size() == 5);
    }

    SECTION("A chained second link is not demuxed")
    {
      auto const pages = std::array{Page{.headerType = kBeginOfStreamFlag | kEndOfStreamFlag,
                                         .granulePosition = 480,
                                         .lacingValues = lacingFor({4}),
                                         .payload = payloadFor({4})},
                                    Page{.headerType = kBeginOfStreamFlag | kEndOfStreamFlag,
                                         .granulePosition = 480,
                                         .serialNumber = 2,
                                         .lacingValues = lacingFor({6}),
                                         .payload = payloadFor({6})}};
      auto const stream = makeStream(pages);
      auto const demuxerRes = Demuxer::parse(stream);
      REQUIRE(demuxerRes);

      REQUIRE(demuxerRes->packetCount() == 1);
      CHECK(demuxerRes->packet(0).bytes.size() == 4);
      CHECK_FALSE(demuxerRes->hasIncompleteTail());
    }
  }

  TEST_CASE("Ogg Demuxer - reports an incomplete ending", "[media][unit][ogg]")
  {
    SECTION("A stream without an end-of-stream page is incomplete")
    {
      auto const pages = std::array{Page{.headerType = kBeginOfStreamFlag,
                                         .granulePosition = 480,
                                         .lacingValues = lacingFor({4}),
                                         .payload = payloadFor({4})}};
      auto const stream = makeStream(pages);
      auto const demuxerRes = Demuxer::parse(stream);
      REQUIRE(demuxerRes);

      CHECK(demuxerRes->packetCount() == 1);
      CHECK(demuxerRes->hasIncompleteTail());
    }

    SECTION("An unterminated trailing packet is dropped and reported")
    {
      auto trailingLacing = lacingFor({4});
      trailingLacing.push_back(255);
      auto trailingPayload = payloadFor({4});
      trailingPayload.insert(trailingPayload.end(), 255, 9);

      auto const pages = std::array{Page{.headerType = kBeginOfStreamFlag | kEndOfStreamFlag,
                                         .granulePosition = 480,
                                         .lacingValues = trailingLacing,
                                         .payload = trailingPayload}};
      auto const stream = makeStream(pages);
      auto const demuxerRes = Demuxer::parse(stream);
      REQUIRE(demuxerRes);

      REQUIRE(demuxerRes->packetCount() == 1);
      CHECK(demuxerRes->packet(0).bytes.size() == 4);
      CHECK(demuxerRes->hasIncompleteTail());
    }

    SECTION("Trailing bytes that do not begin a page end demuxing cleanly")
    {
      auto const pages = std::array{Page{.headerType = kBeginOfStreamFlag,
                                         .granulePosition = 480,
                                         .lacingValues = lacingFor({4}),
                                         .payload = payloadFor({4})}};
      auto stream = makeStream(pages);
      auto const tag = ao::test::ogg::toBytes(std::vector<std::uint8_t>(32, 0x54));
      stream.insert(stream.end(), tag.begin(), tag.end());

      auto const demuxerRes = Demuxer::parse(stream);
      REQUIRE(demuxerRes);
      CHECK(demuxerRes->packetCount() == 1);
    }
  }

  TEST_CASE("Ogg Demuxer - names the position a restart resumes from even without a page granule", "[media][unit][ogg]")
  {
    // The middle page completes a packet while declaring no granule position,
    // which the Ogg specification does not allow. The restart position must
    // still come from the last page that did declare one.
    auto const pages = std::array{Page{.headerType = kBeginOfStreamFlag,
                                       .granulePosition = 1000,
                                       .lacingValues = lacingFor({4, 4}),
                                       .payload = payloadFor({4, 4})},
                                  Page{.pageSequence = 1, .lacingValues = lacingFor({4}), .payload = payloadFor({4})},
                                  Page{.headerType = kEndOfStreamFlag,
                                       .granulePosition = 3000,
                                       .pageSequence = 2,
                                       .lacingValues = lacingFor({4}),
                                       .payload = payloadFor({4})}};
    auto const stream = makeStream(pages);
    auto const demuxerRes = Demuxer::parse(stream);
    REQUIRE(demuxerRes);

    REQUIRE(demuxerRes->packetCount() == 4);
    CHECK(demuxerRes->packet(2).granulePosition == Demuxer::kUnsetGranulePosition);

    auto const restart = demuxerRes->restartAtGranule(2000);
    CHECK(restart.packetIndex == 3);
    CHECK(restart.granulePosition == 1000);
  }

  TEST_CASE("Ogg Demuxer - maps a granule position to its page restart point", "[media][unit][ogg]")
  {
    auto const pages = std::array{
      Page{.headerType = kBeginOfStreamFlag,
           .granulePosition = 1000,
           .lacingValues = lacingFor({4, 4, 4}),
           .payload = payloadFor({4, 4, 4})},
      Page{
        .granulePosition = 2000, .pageSequence = 1, .lacingValues = lacingFor({4, 4}), .payload = payloadFor({4, 4})},
      Page{.headerType = kEndOfStreamFlag,
           .granulePosition = 3000,
           .pageSequence = 2,
           .lacingValues = lacingFor({4}),
           .payload = payloadFor({4})}};
    auto const stream = makeStream(pages);
    auto const demuxerRes = Demuxer::parse(stream);
    REQUIRE(demuxerRes);

    auto const& demuxer = *demuxerRes;
    REQUIRE(demuxer.packetCount() == 6);
    CHECK(demuxer.finalGranulePosition() == 3000);

    struct Expectation final
    {
      std::int64_t requested = 0;
      std::size_t packetIndex = 0;
      std::int64_t resumesFrom = 0;
    };

    auto const expectations = std::array{Expectation{.requested = 0, .packetIndex = 0, .resumesFrom = 0},
                                         Expectation{.requested = 1000, .packetIndex = 0, .resumesFrom = 0},
                                         Expectation{.requested = 1001, .packetIndex = 3, .resumesFrom = 1000},
                                         Expectation{.requested = 2000, .packetIndex = 3, .resumesFrom = 1000},
                                         Expectation{.requested = 2001, .packetIndex = 5, .resumesFrom = 2000},
                                         Expectation{.requested = 3000, .packetIndex = 5, .resumesFrom = 2000}};

    for (auto const& expectation : expectations)
    {
      CAPTURE(expectation.requested);
      auto const restart = demuxer.restartAtGranule(expectation.requested);
      CHECK(restart.packetIndex == expectation.packetIndex);
      CHECK(restart.granulePosition == expectation.resumesFrom);
    }

    auto const beyondEnd = demuxer.restartAtGranule(3001);
    CHECK(beyondEnd.packetIndex == demuxer.packetCount());
    CHECK(beyondEnd.granulePosition == demuxer.finalGranulePosition());
  }
} // namespace ao::media::ogg::test
