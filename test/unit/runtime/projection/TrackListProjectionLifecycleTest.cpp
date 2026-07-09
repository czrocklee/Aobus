// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/projection/TrackListProjectionTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <variant>

namespace ao::rt::test
{
  TEST_CASE("TrackListProjection - initialized projection exposes source rows and view metadata",
            "[runtime][unit][projection]")
  {
    auto env = TrackListProjectionFixture{};
    auto const id1 = env.libraryFixture.addTrack(library::test::makeTrackSpec("Track A", 2020));
    auto const id2 = env.libraryFixture.addTrack(library::test::makeTrackSpec("Track B", 2021));
    env.setupFiltered({{id1, id2}});

    auto proj = env.createProjection(ViewId{1});

    SECTION("size matches source")
    {
      CHECK(proj.size() == 2);
    }

    SECTION("trackIdAt returns correct tracks in ID order")
    {
      CHECK(proj.trackIdAt(0) == id1);
      CHECK(proj.trackIdAt(1) == id2);
    }

    SECTION("viewId and revision")
    {
      CHECK(proj.viewId() == ViewId{1});
      CHECK(proj.revision() == 0);
    }

    SECTION("trackIdAt out of bounds returns invalid")
    {
      CHECK(proj.trackIdAt(999) == kInvalidTrackId);
    }

    SECTION("indexOf returns correct positions")
    {
      CHECK(proj.indexOf(id1) == 0);
      CHECK(proj.indexOf(id2) == 1);
      CHECK_FALSE(proj.indexOf(TrackId{999}).has_value());
    }
  }

  TEST_CASE("TrackListProjection - subscribe immediately publishes reset", "[runtime][unit][projection]")
  {
    auto env = TrackListProjectionFixture{};
    auto const id1 = env.libraryFixture.addTrack(library::test::makeTrackSpec("Track", 2020));
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    bool receivedReset = false;
    auto const sub = proj.subscribe(
      [&](TrackListProjectionDeltaBatch const& batch)
      {
        REQUIRE(batch.deltas.size() == 1);
        CHECK(std::holds_alternative<ProjectionReset>(batch.deltas[0]));
        receivedReset = true;
      });

    CHECK(receivedReset);
  }

  TEST_CASE("TrackListProjection - subscribe replays reset to each subscriber", "[runtime][unit][projection]")
  {
    auto env = TrackListProjectionFixture{};
    auto const id1 = env.libraryFixture.addTrack(library::test::makeTrackSpec("Track", 2020));
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    std::int32_t count = 0;
    auto const sub1 = proj.subscribe([&](TrackListProjectionDeltaBatch const&) { ++count; });
    auto const sub2 = proj.subscribe([&](TrackListProjectionDeltaBatch const&) { ++count; });
    CHECK(count == 2);
  }

  TEST_CASE("TrackListProjection - subscription reset does not emit additional callbacks",
            "[runtime][unit][projection]")
  {
    auto env = TrackListProjectionFixture{};
    auto const id1 = env.libraryFixture.addTrack(library::test::makeTrackSpec("Track", 2020));
    env.setupFiltered({{id1}});

    auto proj = env.createProjection(ViewId{1});
    std::int32_t count = 0;
    auto sub = proj.subscribe([&](TrackListProjectionDeltaBatch const&) { ++count; });
    auto const initial = count;
    sub.reset();
    CHECK(count == initial);
  }

  TEST_CASE("TrackListProjection - empty source exposes no rows and invalid lookup fallback",
            "[runtime][unit][projection]")
  {
    auto env = TrackListProjectionFixture{};
    env.setupFiltered({});

    auto proj = env.createProjection(ViewId{1});
    CHECK(proj.size() == 0);
    CHECK(proj.trackIdAt(0) == kInvalidTrackId);
    CHECK(proj.trackIdAt(999) == kInvalidTrackId);
  }
} // namespace ao::rt::test
