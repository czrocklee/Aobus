// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/MediaTrack.h"

#include "test/unit/audio/AudioFixtureSupport.h"
#include <ao/AudioCodec.h>
#include <ao/PictureType.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <type_traits>
#include <utility>

namespace ao::rt::test
{
  static_assert(std::is_move_constructible_v<MediaTrack>);
  static_assert(!std::is_move_assignable_v<MediaTrack>);

  TEST_CASE("MediaTrack - keeps borrowed builder fields alive across moves", "[runtime][unit][media-track]")
  {
    auto result = readMediaTrack(audio::test::requireAudioFixture("basic_metadata.mp3"));
    REQUIRE(result);

    auto mediaTrack = std::move(*result);
    auto movedAgain = std::move(mediaTrack);

    auto const& metadata = movedAgain.builder().metadata();
    CHECK(metadata.title() == "Test Title");
    CHECK(metadata.artist() == "Test Artist");
    CHECK(metadata.album() == "Test Album");
    CHECK(metadata.composer() == "Test Composer");
    CHECK(metadata.genre() == "Rock");
    CHECK(metadata.work() == "Symphony No. 5");
    CHECK(metadata.year() == 2024);
    CHECK(metadata.trackNumber() == 1);

    auto const& property = movedAgain.builder().property();
    CHECK(property.codec() == AudioCodec::Mp3);
    CHECK(property.duration() > std::chrono::milliseconds{0});
    CHECK(property.bitrate().raw() > 0);
    CHECK(property.sampleRate() == 44100);
    CHECK(property.channels() == 2);
    CHECK(property.bitDepth() == 16);
    CHECK(movedAgain.file().audioPayload());
  }

  TEST_CASE("MediaTrack - maps classical visitor fields into TrackBuilder", "[runtime][unit][media-track]")
  {
    auto result = readMediaTrack(audio::test::requireAudioFixture("classical_metadata.mp3"));
    REQUIRE(result);

    auto const& metadata = result->builder().metadata();
    CHECK(metadata.conductor() == "Fixture Conductor");
    CHECK(metadata.ensemble() == "Fixture Ensemble");
    CHECK(metadata.movement() == "Fixture Movement");
    CHECK(metadata.soloist() == "Fixture Soloist");
    CHECK(metadata.movementNumber() == 2);
    CHECK(metadata.movementTotal() == 4);
    CHECK(metadata.trackTotal() == 9);
  }

  TEST_CASE("MediaTrack - maps picture callbacks into pending cover entries", "[runtime][unit][media-track]")
  {
    auto result = readMediaTrack(audio::test::requireAudioFixture("with_cover.mp3"));
    REQUIRE(result);

    auto const& covers = result->builder().coverArt().entries();
    REQUIRE(covers.size() == 1);
    CHECK(covers.front().type == PictureType::Other);
  }
} // namespace ao::rt::test
