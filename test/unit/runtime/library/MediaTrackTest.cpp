// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/MediaTrack.h"

#include "test/unit/audio/AudioFixtureSupport.h"
#include <ao/AudioCodec.h>
#include <ao/PictureType.h>
#include <ao/library/Credits.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

namespace ao::rt::test
{
  static_assert(std::is_move_constructible_v<MediaTrack>);
  static_assert(!std::is_move_assignable_v<MediaTrack>);

  TEST_CASE("MediaTrack - keeps borrowed builder fields alive across moves", "[runtime][unit][media-track]")
  {
    auto res = readMediaTrack(audio::test::requireAudioFixture("basic_metadata.mp3"));
    REQUIRE(res);

    auto mediaTrack = std::move(*res);
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

  TEST_CASE("MediaTrack - maps classical visitor fields into TrackBuilder", "[runtime][integration][media-track]")
  {
    auto res = readMediaTrack(audio::test::requireAudioFixture("classical_metadata.mp3"));
    REQUIRE(res);

    auto const& metadata = res->builder().metadata();
    auto const expected = std::array{
      library::CreditView{.name = "Fixture Conductor", .kind = library::CreditKind::Conductor},
      library::CreditView{.name = "Fixture Ensemble", .kind = library::CreditKind::Ensemble},
      library::CreditView{.name = "Fixture Soloist", .kind = library::CreditKind::Soloist},
    };
    REQUIRE(metadata.credits().size() == expected.size());
    // Media callbacks retain source traversal order; storage groups the kinds.
    CHECK(
      std::ranges::is_permutation(metadata.credits(),
                                  expected,
                                  [](auto const& lhs, auto const& rhs)
                                  { return lhs.name == rhs.name && lhs.kind == rhs.kind && lhs.role == rhs.role; }));

    CHECK(metadata.movement() == "Fixture Movement");
    CHECK(metadata.movementNumber() == 2);
    CHECK(metadata.movementTotal() == 4);
    CHECK(metadata.trackTotal() == 9);
  }

  TEST_CASE("MediaTrack - routes Ensemble and Performer credits into borrowed builder fields across moves",
            "[runtime][integration][media-track]")
  {
    auto res = readMediaTrack(audio::test::requireAudioFixture("classical_fallback.flac"));
    REQUIRE(res);

    auto moved = std::move(*res);
    auto movedAgain = std::move(moved);
    auto const& metadata = movedAgain.builder().metadata();
    // A PERFORMER source never implies Soloist, even if the supplied name says so.
    auto const expected = std::array{
      library::CreditView{.name = "Fixture Fallback Ensemble", .kind = library::CreditKind::Ensemble},
      library::CreditView{.name = "Fixture Fallback Soloist", .kind = library::CreditKind::Performer},
    };
    REQUIRE(metadata.credits().size() == expected.size());
    CHECK(
      std::ranges::is_permutation(metadata.credits(),
                                  expected,
                                  [](auto const& lhs, auto const& rhs)
                                  { return lhs.name == rhs.name && lhs.kind == rhs.kind && lhs.role == rhs.role; }));
  }

  TEST_CASE("MediaTrack - maps picture callbacks into pending cover entries", "[runtime][integration][media-track]")
  {
    auto res = readMediaTrack(audio::test::requireAudioFixture("with_cover.mp3"));
    REQUIRE(res);

    auto const& covers = res->builder().coverArt().entries();
    REQUIRE(covers.size() == 1);
    CHECK(covers.front().type == PictureType::Other);
    auto const* const bytes = std::get_if<std::span<std::byte const>>(&covers.front().source);
    REQUIRE(bytes);
    // Pin the checked-in MP3's APIC payload independently of the runtime adapter.
    CHECK(bytes->size() == 90);
    CHECK(utility::sha256Hex(utility::computeSha256(*bytes)) ==
          "d5fb6495697da5911365ff4dcda27b627fa3dbfdad162e9db3ab280f97274331");
  }
} // namespace ao::rt::test
