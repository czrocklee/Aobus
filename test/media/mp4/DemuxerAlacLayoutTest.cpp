// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestAtoms.h"
#include <ao/Error.h>
#include <ao/media/mp4/Atom.h>
#include <ao/media/mp4/Demuxer.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

namespace ao::media::mp4::test
{
  namespace
  {
    std::span<std::byte const> bytesOf(std::vector<std::uint8_t> const& data)
    {
      return std::as_bytes(std::span<std::uint8_t const>{data});
    }

    std::vector<std::uint8_t> makeCookie()
    {
      // Opaque FullBox/config bytes: media does not interpret codec geometry.
      auto body = std::vector<std::uint8_t>(28, 0);
      body[27] = 0x31;
      return ao::test::mp4::makeAtom("alac", body);
    }

    std::vector<std::uint8_t> makeTrack(std::vector<std::uint8_t> const& stsd)
    {
      auto const table = ao::test::mp4::makeSampleTableAtom(stsd, 4, 2048, 8);
      return ao::test::mp4::makeTrackAtom("soun", table, 48000, 96000);
    }

    std::vector<std::uint8_t> makeFile(std::vector<std::uint8_t> const& tracks)
    {
      auto file = ao::test::mp4::makeAtom("mdat", {0x91, 0x82, 0x73, 0x64});
      file.append_range(ao::test::mp4::makeAtom("moov", tracks));
      return file;
    }

    void checkPacketAndTiming(Demuxer const& demuxer)
    {
      CHECK(demuxer.sampleCount() == 1);
      CHECK(demuxer.timescale() == 48000);
      CHECK(demuxer.duration() == 96000);
      auto const sample = demuxer.sampleInfo(0);
      CHECK(sample.offset == 8);
      CHECK(sample.size == 4);
      CHECK(sample.startTime == 0);
      CHECK(sample.duration == 2048);
      auto const packet = std::to_array<std::byte>({
        std::byte{0x91},
        std::byte{0x82},
        std::byte{0x73},
        std::byte{0x64},
      });
      CHECK(std::ranges::equal(demuxer.samplePayload(0), packet));
      CHECK(demuxer.sampleIndexAtTime(2047) == 0);
      CHECK(demuxer.sampleIndexAtTime(2048) == 1);
    }

    std::vector<std::uint8_t> makeInvalidTail(std::uint32_t variant)
    {
      if (variant == 0)
      {
        return {0, 0, 0, 0xFF, 'c', 'h', 'a', 'n', 0x71};
      }

      if (variant == 1)
      {
        return ao::test::mp4::makeEndOfFileAtom("chan", {0x71, 0x82});
      }

      return {0, 0, 0};
    }
  } // namespace

  TEST_CASE("MP4 Demuxer - borrows bounded selected ALAC entry with chan siblings", "[media][unit][mp4]")
  {
    auto const chanBeforeCookie = GENERATE(false, true);
    auto const extendedChan = GENERATE(false, true);
    CAPTURE(chanBeforeCookie, extendedChan);
    auto const cookie = makeCookie();
    auto const chanBody = std::vector<std::uint8_t>{0x91, 0x72, 0x53, 0x34, 0x15};
    auto const chan =
      extendedChan ? ao::test::mp4::makeExtendedAtom("chan", chanBody) : ao::test::mp4::makeAtom("chan", chanBody);
    auto extensions = chanBeforeCookie ? chan : cookie;
    extensions.append_range(chanBeforeCookie ? cookie : chan);
    auto const entry = ao::test::mp4::makeAudioSampleEntryAtom("alac", extensions);
    auto const file = makeFile(makeTrack(ao::test::mp4::makeStsdAtomFromSampleEntry(entry)));

    auto const res = Demuxer::parse(bytesOf(file), "alac");

    REQUIRE(res);
    CHECK(std::ranges::equal(res->magicCookie(), bytesOf(cookie)));
    auto const optEntry = res->alacSampleEntry();
    REQUIRE(optEntry);
    CHECK(optEntry->type() == "alac");
    CHECK(std::ranges::equal(optEntry->bytes(), bytesOf(entry)));
    auto const entryPosition = std::ranges::search(bytesOf(file), bytesOf(entry));
    REQUIRE_FALSE(entryPosition.empty());
    CHECK(optEntry->bytes().data() == std::ranges::data(entryPosition));
    auto const path = std::to_array<std::string_view>({"alac", "chan"});
    auto const chanRes = findAtom(*optEntry, path);
    REQUIRE(chanRes);
    REQUIRE(*chanRes);
    CHECK(std::ranges::equal((**chanRes).bytes(), bytesOf(chan)));
    CHECK(std::ranges::equal((**chanRes).payload(), bytesOf(chanBody)));
    auto children = optEntry->children();
    auto const firstRes = children.next();
    REQUIRE(firstRes);
    REQUIRE(*firstRes);
    CHECK((**firstRes).type() == (chanBeforeCookie ? "chan" : "alac"));
    auto const secondRes = children.next();
    REQUIRE(secondRes);
    REQUIRE(*secondRes);
    CHECK((**secondRes).type() == (chanBeforeCookie ? "alac" : "chan"));
    auto const endRes = children.next();
    REQUIRE(endRes);
    CHECK_FALSE(*endRes);
    checkPacketAndTiming(*res);
  }

  TEST_CASE("MP4 Demuxer - distinguishes cookie-contained chan from sample-entry sibling", "[media][unit][mp4]")
  {
    auto const contained = GENERATE(false, true);
    auto const extendedCookie = GENERATE(false, true);
    CAPTURE(contained, extendedCookie);
    auto const chan = ao::test::mp4::makeAtom("chan", {0x31, 0x52, 0x73});
    auto cookie = makeCookie();

    if (contained)
    {
      auto body = std::vector<std::uint8_t>{cookie.begin() + 8, cookie.end()};
      body.append_range(chan);
      cookie = ao::test::mp4::makeAtom("alac", body);
    }

    if (extendedCookie)
    {
      cookie = ao::test::mp4::makeExtendedFromCompactAtom(cookie);
    }

    auto extensions = cookie;

    if (!contained)
    {
      extensions.append_range(chan);
    }

    auto const entry = ao::test::mp4::makeAudioSampleEntryAtom("alac", extensions);
    auto const file = makeFile(makeTrack(ao::test::mp4::makeStsdAtomFromSampleEntry(entry)));
    auto const res = Demuxer::parse(bytesOf(file), "alac");

    REQUIRE(res);
    CHECK(std::ranges::equal(res->magicCookie(), bytesOf(cookie)));
    auto const optEntry = res->alacSampleEntry();
    REQUIRE(optEntry);
    CHECK(std::ranges::equal(optEntry->bytes(), bytesOf(entry)));
    auto const path = std::to_array<std::string_view>({"alac", "chan"});
    auto const chanRes = findAtom(*optEntry, path);
    REQUIRE(chanRes);
    CHECK(chanRes->has_value() == !contained);

    if (contained)
    {
      CHECK(std::ranges::equal(res->magicCookie().last(chan.size()), bytesOf(chan)));
    }
    else
    {
      REQUIRE(*chanRes);
      CHECK(std::ranges::equal((**chanRes).bytes(), bytesOf(chan)));
    }

    checkPacketAndTiming(*res);
  }

  TEST_CASE("MP4 Demuxer - leaves malformed ALAC tails opaque after required cookie", "[media][unit][mp4]")
  {
    auto const variant = GENERATE(0U, 1U, 2U);
    CAPTURE(variant);
    auto const cookie = makeCookie();
    auto const tail = makeInvalidTail(variant);
    auto extensions = cookie;
    extensions.append_range(tail);
    auto const entry = ao::test::mp4::makeAudioSampleEntryAtom("alac", extensions);
    auto const file = makeFile(makeTrack(ao::test::mp4::makeStsdAtomFromSampleEntry(entry)));

    auto const res = Demuxer::parse(bytesOf(file), "alac");

    REQUIRE(res);
    CHECK(std::ranges::equal(res->magicCookie(), bytesOf(cookie)));
    auto const optEntry = res->alacSampleEntry();
    REQUIRE(optEntry);
    CHECK(std::ranges::equal(optEntry->bytes(), bytesOf(entry)));
    CHECK(std::ranges::equal(optEntry->bytes().last(tail.size()), bytesOf(tail)));
    auto cursor = optEntry->children();
    auto const cookieRes = cursor.next();
    REQUIRE(cookieRes);
    REQUIRE(*cookieRes);
    CHECK(std::ranges::equal((**cookieRes).bytes(), bytesOf(cookie)));
    CHECK_FALSE(cursor.next());
    checkPacketAndTiming(*res);
  }

  TEST_CASE("MP4 Demuxer - rejects malformed ALAC prefix before required cookie", "[media][unit][mp4]")
  {
    auto const variant = GENERATE(0U, 1U, 2U);
    CAPTURE(variant);
    auto extensions = makeInvalidTail(variant);
    extensions.append_range(makeCookie());
    auto const file = makeFile(makeTrack(ao::test::mp4::makeStsdAtom("alac", extensions)));

    auto const res = Demuxer::parse(bytesOf(file), "alac");

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::FormatRejected);
  }

  TEST_CASE("MP4 Demuxer - ALAC evidence stays with selected track", "[media][unit][mp4]")
  {
    auto const cookie = makeCookie();
    auto const chan = ao::test::mp4::makeAtom("chan", {0x41, 0x62, 0x83});
    auto unrelatedExtensions = cookie;
    unrelatedExtensions.append_range(chan);
    auto const unrelatedAlac = makeTrack(ao::test::mp4::makeStsdAtom("alac", unrelatedExtensions));
    auto const esds = ao::test::mp4::makeAtom("esds", {0, 0, 0, 0, 0x05, 0x02, 0x12, 0x10});
    auto aacExtensions = esds;
    aacExtensions.append_range(chan);
    auto tracks = makeTrack(ao::test::mp4::makeStsdAtom("mp4a", aacExtensions));
    auto const video = ao::test::mp4::makeTrackAtom(
      "vide", ao::test::mp4::makeSampleTableAtom(ao::test::mp4::makeStsdAtom("alac", unrelatedExtensions)));
    tracks.append_range(video);
    auto const selectedEntry = ao::test::mp4::makeAudioSampleEntryAtom("alac", cookie);
    tracks.append_range(makeTrack(ao::test::mp4::makeStsdAtomFromSampleEntry(selectedEntry)));
    tracks.append_range(unrelatedAlac);
    auto const file = makeFile(tracks);

    auto const res = Demuxer::parse(bytesOf(file), "alac");

    REQUIRE(res);
    CHECK(std::ranges::equal(res->magicCookie(), bytesOf(cookie)));
    auto const optEntry = res->alacSampleEntry();
    REQUIRE(optEntry);
    CHECK(std::ranges::equal(optEntry->bytes(), bytesOf(selectedEntry)));
    auto const path = std::to_array<std::string_view>({"alac", "chan"});
    auto const chanRes = findAtom(*optEntry, path);
    REQUIRE(chanRes);
    CHECK_FALSE(*chanRes);
    checkPacketAndTiming(*res);
  }

  TEST_CASE("MP4 Demuxer - AAC retains exact ASC without borrowing ALAC evidence", "[media][unit][mp4]")
  {
    auto const chan = ao::test::mp4::makeAtom("chan", {0x81, 0x62, 0x43});
    auto alacExtensions = makeCookie();
    alacExtensions.append_range(chan);
    auto tracks = makeTrack(ao::test::mp4::makeStsdAtom("alac", alacExtensions));
    auto aacExtensions = ao::test::mp4::makeAtom("esds", {0, 0, 0, 0, 0x05, 0x02, 0x12, 0x10});
    aacExtensions.append_range(chan);
    tracks.append_range(makeTrack(ao::test::mp4::makeStsdAtom("mp4a", aacExtensions)));
    auto const file = makeFile(tracks);

    auto const res = Demuxer::parse(bytesOf(file), "mp4a");

    REQUIRE(res);
    auto const asc = std::to_array<std::byte>({std::byte{0x12}, std::byte{0x10}});
    CHECK(std::ranges::equal(res->magicCookie(), asc));
    CHECK_FALSE(res->alacSampleEntry());
    checkPacketAndTiming(*res);
  }

  TEST_CASE("MP4 Demuxer - withholds entry view when required cookie comes from later description",
            "[media][unit][mp4]")
  {
    auto const cookie = makeCookie();
    auto const firstEntry = ao::test::mp4::makeAudioSampleEntryAtom("alac", ao::test::mp4::makeAtom("chan", {0x71}));
    auto const laterEntry = ao::test::mp4::makeAudioSampleEntryAtom("alac", cookie);
    auto stsd = std::vector<std::uint8_t>{};

    SECTION("Count one permits an extra physical entry in the existing cookie lookup")
    {
      auto body = std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 1};
      body.append_range(firstEntry);
      body.append_range(laterEntry);
      stsd = ao::test::mp4::makeAtom("stsd", body);
    }

    SECTION("A later stsd in the same table supplies the existing cookie lookup")
    {
      stsd = ao::test::mp4::makeStsdAtomFromSampleEntry(firstEntry);
      stsd.append_range(ao::test::mp4::makeStsdAtomFromSampleEntry(laterEntry));
    }

    auto const file = makeFile(makeTrack(stsd));
    auto const res = Demuxer::parse(bytesOf(file), "alac");

    REQUIRE(res);
    CHECK(std::ranges::equal(res->magicCookie(), bytesOf(cookie)));
    CHECK_FALSE(res->alacSampleEntry());
    checkPacketAndTiming(*res);
  }
} // namespace ao::media::mp4::test
