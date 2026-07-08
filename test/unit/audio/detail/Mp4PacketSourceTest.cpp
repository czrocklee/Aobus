// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/detail/Mp4PacketSource.h"

#include "test/unit/TestUtils.h"
#include "test/unit/media/mp4/TestAtoms.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

namespace ao::audio::detail::test
{
  namespace
  {
    std::vector<std::uint8_t> makeMp4(std::uint32_t timescale = 44100)
    {
      auto data = std::vector<std::uint8_t>{};
      auto const payload = std::vector<std::uint8_t>{1, 2, 3, 4};
      ao::test::mp4::addAtom(data, "mdat", payload);

      auto const config = ao::test::mp4::makeAtom("alac", {9, 8, 7});
      auto const track =
        ao::test::mp4::makeCompleteAudioTrackAtom("alac", config, timescale, 88200, payload.size(), 1024, 8);
      auto const moov = ao::test::mp4::makeAtom("moov", track);
      data.insert(data.end(), moov.begin(), moov.end());
      return data;
    }
  } // namespace

  TEST_CASE("Mp4PacketSource - exposes selected track packets and timing", "[audio][unit][detail][mp4]")
  {
    auto const temp = ao::test::TempFile{makeMp4(), ".m4a"};
    auto source = Mp4PacketSource{};

    REQUIRE(source.open(temp.path, "alac"));
    CHECK(source.isOpen());
    CHECK_FALSE(source.isAtEnd());
    CHECK(source.sampleIndex() == 0);
    CHECK(source.packet().size() == 4);
    CHECK(source.magicCookie().size() == 11);
    CHECK(source.timescale() == 44100);
    CHECK(source.duration() == std::chrono::seconds{2});
    CHECK(source.firstFrameIndex(44100, 4096) == 0);

    source.advance();
    CHECK(source.isAtEnd());
    CHECK(source.packet().empty());
    CHECK(source.sampleInfo().size == 0);

    source.close();
    CHECK_FALSE(source.isOpen());
    CHECK(source.isAtEnd());
    CHECK(!source.seek(std::chrono::milliseconds{10}));
  }

  TEST_CASE("Mp4PacketSource - handles selection failures and timescale fallback", "[audio][unit][detail][mp4]")
  {
    SECTION("Wrong sample entry closes the source")
    {
      auto const temp = ao::test::TempFile{makeMp4(), ".m4a"};
      auto source = Mp4PacketSource{};

      CHECK(!source.open(temp.path, "mp4a"));
      CHECK_FALSE(source.isOpen());
      CHECK(source.isAtEnd());
    }

    SECTION("Zero media timescale uses the codec fallback")
    {
      auto const temp = ao::test::TempFile{makeMp4(0), ".m4a"};
      auto source = Mp4PacketSource{};

      REQUIRE(source.open(temp.path, "alac"));
      CHECK(source.timescale() == 0);
      CHECK(source.timescale(44100) == 44100);
      CHECK(source.duration() == std::chrono::milliseconds{0});
      CHECK(source.duration(44100) == std::chrono::seconds{2});
      CHECK(!source.seek(std::chrono::seconds{1}));
      CHECK(source.seek(std::chrono::seconds{1}, 44100));
      CHECK(source.isAtEnd());
    }
  }
} // namespace ao::audio::detail::test
