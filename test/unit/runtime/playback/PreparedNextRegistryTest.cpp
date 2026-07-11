// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PreparedNextRegistry.h"

#include "runtime/playback/ProjectionAnchor.h"
#include <ao/CoreIds.h>
#include <ao/rt/PreparedPlayback.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ao::rt::test
{
  namespace
  {
    constexpr auto kFirstToken = PreparedNextToken{1};
    constexpr auto kSecondToken = PreparedNextToken{2};
    constexpr auto kThirdToken = PreparedNextToken{3};
    constexpr auto kUnknownToken = PreparedNextToken{999};
    constexpr auto kFirstTrack = TrackId{10};
    constexpr auto kSecondTrack = TrackId{20};
    constexpr auto kThirdTrack = TrackId{30};
    constexpr std::uint64_t kFirstGeneration = 10;
    constexpr std::uint64_t kSecondGeneration = 20;
    constexpr std::uint64_t kThirdGeneration = 30;

    TrackListProjectionDeltaBatch insertBatch(std::size_t const start, std::size_t const count)
    {
      return TrackListProjectionDeltaBatch{
        .deltas = {ProjectionInsertRange{TrackRowRange{.start = start, .count = count}}},
      };
    }
  } // namespace

  TEST_CASE("PreparedNextRegistry - activating a replacement retires the previous commitment",
            "[runtime][unit][playback-cursor]")
  {
    auto registry = PreparedNextRegistry{};
    registry.activate(kFirstToken, kFirstGeneration, ProjectionAnchor::bound(kFirstTrack, 1, 4));

    registry.activate(kSecondToken, kSecondGeneration, ProjectionAnchor::gap(kSecondTrack, 3, 4));

    CHECK(registry.activeToken() == kSecondToken);
    CHECK(registry.isRetired(kFirstToken));
    CHECK_FALSE(registry.isRetired(kSecondToken));
    CHECK(registry.contains(kFirstToken));
    CHECK(registry.contains(kSecondToken));
    CHECK(registry.size() == 2);
    CHECK(registry.retiredCount() == 1);
  }

  TEST_CASE("PreparedNextRegistry - complete batches maintain independent Bound and Gap anchors",
            "[runtime][unit][playback-cursor]")
  {
    auto registry = PreparedNextRegistry{};
    registry.activate(kFirstToken, kFirstGeneration, ProjectionAnchor::bound(kFirstTrack, 1, 4));
    registry.activate(kSecondToken, kSecondGeneration, ProjectionAnchor::gap(kSecondTrack, 3, 4));

    registry.applyBatch(insertBatch(1, 2),
                        6,
                        [](TrackId const trackId) -> std::optional<std::size_t>
                        { return trackId == kFirstTrack ? std::optional<std::size_t>{3} : std::nullopt; });

    auto const optFirstAnchor = registry.anchorFor(kFirstToken);
    REQUIRE(optFirstAnchor);
    CHECK(optFirstAnchor->state() == ProjectionAnchor::State::Bound);
    CHECK(optFirstAnchor->anchorIndex() == 3);

    auto const optSecondAnchor = registry.anchorFor(kSecondToken);
    REQUIRE(optSecondAnchor);
    CHECK(optSecondAnchor->state() == ProjectionAnchor::State::Gap);
    CHECK(optSecondAnchor->anchorIndex() == 5);
  }

  TEST_CASE("PreparedNextRegistry - exact disarm acknowledgement forgets active and retired tokens",
            "[runtime][unit][playback-cursor]")
  {
    auto registry = PreparedNextRegistry{};
    registry.activate(kFirstToken, kFirstGeneration, ProjectionAnchor::bound(kFirstTrack, 0, 2));
    CHECK(registry.acknowledgeDisarm(kFirstToken));
    CHECK(registry.size() == 0);

    registry.activate(kFirstToken, kFirstGeneration, ProjectionAnchor::bound(kFirstTrack, 0, 2));
    registry.activate(kSecondToken, kSecondGeneration, ProjectionAnchor::bound(kSecondTrack, 1, 2));
    REQUIRE(registry.isRetired(kFirstToken));

    CHECK(registry.acknowledgeDisarm(kFirstToken));
    CHECK_FALSE(registry.contains(kFirstToken));
    CHECK(registry.activeToken() == kSecondToken);
    CHECK_FALSE(registry.acknowledgeDisarm(kUnknownToken));
  }

  TEST_CASE("PreparedNextRegistry - retired winner closes the window while unknown token changes nothing",
            "[runtime][unit][playback-cursor]")
  {
    auto registry = PreparedNextRegistry{};
    registry.activate(kFirstToken, kFirstGeneration, ProjectionAnchor::bound(kFirstTrack, 0, 2));
    registry.activate(kSecondToken, kSecondGeneration, ProjectionAnchor::bound(kSecondTrack, 1, 2));

    auto const optUnknown = registry.resolveWinner(kUnknownToken);
    CHECK_FALSE(optUnknown);
    CHECK(registry.size() == 2);
    CHECK(registry.activeToken() == kSecondToken);

    auto const optWinner = registry.resolveWinner(kFirstToken);
    REQUIRE(optWinner);
    CHECK(optWinner->trackId() == kFirstTrack);
    CHECK(optWinner->state() == ProjectionAnchor::State::Bound);
    CHECK(optWinner->anchorIndex() == 0);
    CHECK(registry.size() == 0);
    CHECK_FALSE(registry.activeToken());
  }

  TEST_CASE("PreparedNextRegistry - runtime barrier clears only strictly older issuance generations",
            "[runtime][unit][playback-cursor]")
  {
    auto registry = PreparedNextRegistry{};
    registry.activate(kFirstToken, kFirstGeneration, ProjectionAnchor::bound(kFirstTrack, 0, 3));
    registry.activate(kSecondToken, kSecondGeneration, ProjectionAnchor::bound(kSecondTrack, 1, 3));

    registry.clearCoveredByBarrier(PreparedCancellationBarrier{.generation = kSecondGeneration});

    CHECK_FALSE(registry.contains(kFirstToken));
    CHECK(registry.activeToken() == kSecondToken);
    registry.activate(kThirdToken, kThirdGeneration, ProjectionAnchor::bound(kThirdTrack, 2, 3));

    registry.clearCoveredByBarrier(PreparedCancellationBarrier{.generation = kThirdGeneration});

    CHECK_FALSE(registry.contains(kSecondToken));
    CHECK(registry.activeToken() == kThirdToken);
    CHECK(registry.size() == 1);
  }

  TEST_CASE("PreparedNextRegistry - equal-generation replacement survives an older barrier",
            "[runtime][unit][playback-cursor]")
  {
    auto registry = PreparedNextRegistry{};
    registry.activate(kFirstToken, kFirstGeneration, ProjectionAnchor::bound(kFirstTrack, 0, 2));

    registry.clear();
    registry.activate(kSecondToken, kSecondGeneration, ProjectionAnchor::bound(kSecondTrack, 1, 2));
    registry.clearCoveredByBarrier(PreparedCancellationBarrier{.generation = kSecondGeneration});

    CHECK_FALSE(registry.contains(kFirstToken));
    CHECK(registry.activeToken() == kSecondToken);
    CHECK_FALSE(registry.resolveWinner(kFirstToken));
  }

  TEST_CASE("PreparedNextRegistry - invalidation retains an unacknowledged race winner",
            "[runtime][unit][playback-cursor]")
  {
    auto registry = PreparedNextRegistry{};
    registry.activate(kFirstToken, kFirstGeneration, ProjectionAnchor::gap(kFirstTrack, 2, 3));

    registry.invalidate();

    CHECK_FALSE(registry.activeToken());
    CHECK(registry.isRetired(kFirstToken));
    auto const optWinner = registry.resolveWinner(kFirstToken);
    REQUIRE(optWinner);
    CHECK(optWinner->trackId() == kFirstTrack);
    CHECK(optWinner->state() == ProjectionAnchor::State::Gap);
    CHECK(optWinner->anchorIndex() == 2);
    CHECK(registry.size() == 0);
  }

  TEST_CASE("PreparedNextRegistry - invalidation forgets a token with exact disarm proof",
            "[runtime][unit][playback-cursor]")
  {
    auto registry = PreparedNextRegistry{};
    registry.activate(kFirstToken, kFirstGeneration, ProjectionAnchor::bound(kFirstTrack, 0, 1));

    registry.invalidate(kFirstToken);

    CHECK_FALSE(registry.activeToken());
    CHECK_FALSE(registry.contains(kFirstToken));
    CHECK(registry.size() == 0);
  }
} // namespace ao::rt::test
