// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/source/AllTracksSource.h>
#include <ao/rt/source/TrackSourceDelta.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::library;

  TEST_CASE("AllTracksSource - reload and track change notifications update source state",
            "[runtime][unit][source][all-tracks]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto& store = libraryFixture.library().tracks();

    auto source = AllTracksSource{store};
    auto listener = TrackSourceBatchSpy{source};

    SECTION("reloadFromStore populates and notifies reset")
    {
      auto const t1 = libraryFixture.addTrack("A");
      auto const t2 = libraryFixture.addTrack("B");

      auto const transaction = libraryFixture.library().readTransaction();
      source.reloadFromStore(transaction);

      REQUIRE(listener.batches.size() == 1);
      CHECK(std::holds_alternative<SourceReset>(listener.batches.front().deltas.front()));
      CHECK(source.size() == 2);

      auto const optI1 = source.indexOf(t1);
      auto const optI2 = source.indexOf(t2);

      REQUIRE(optI1);
      REQUIRE(optI2);
      CHECK(source.trackIdAt(*optI1) == t1);
      CHECK(source.trackIdAt(*optI2) == t2);
    }

    SECTION("notifyInserted adds item and notifies")
    {
      source.notifyInserted(TrackId{10});
      REQUIRE(listener.batches.size() == 1);
      auto const& firstInsert = std::get<SourceInsertRange>(listener.batches[0].deltas.front());
      CHECK(firstInsert.start == 0);
      CHECK(firstInsert.trackIds == std::vector{TrackId{10}});
      CHECK(source.size() == 1);

      source.notifyInserted(TrackId{20});
      REQUIRE(listener.batches.size() == 2);
      auto const& secondInsert = std::get<SourceInsertRange>(listener.batches[1].deltas.front());
      CHECK(secondInsert.start == 1);
      CHECK(secondInsert.trackIds == std::vector{TrackId{20}});
      CHECK(source.size() == 2);

      // Insert smaller id, should be at index 0
      source.notifyInserted(TrackId{5});
      REQUIRE(listener.batches.size() == 3);
      auto const& frontInsert = std::get<SourceInsertRange>(listener.batches[2].deltas.front());
      CHECK(frontInsert.start == 0);
      CHECK(frontInsert.trackIds == std::vector{TrackId{5}});

      // Duplicate insert shouldn't trigger
      source.notifyInserted(TrackId{10});
      CHECK(listener.batches.size() == 3);
    }

    SECTION("notifyRemoved removes item and notifies")
    {
      source.notifyInserted(TrackId{10});
      source.notifyInserted(TrackId{20});

      listener.clear();

      source.notifyRemoved(TrackId{10});
      REQUIRE(listener.batches.size() == 1);
      auto const& removal = std::get<SourceRemoveRange>(listener.batches.front().deltas.front());
      CHECK(removal.start == 0);
      CHECK(removal.trackIds == std::vector{TrackId{10}});
      CHECK(source.size() == 1);

      // Non-existent remove shouldn't trigger
      source.notifyRemoved(TrackId{99});
      CHECK(listener.batches.size() == 1);

      CHECK(source.indexOf(TrackId{10}) == std::nullopt);
    }

    SECTION("clear empties the source and resets")
    {
      source.notifyInserted(TrackId{10});
      source.notifyInserted(TrackId{20});
      listener.clear();

      source.clear();

      CHECK(source.size() == 0);
      REQUIRE(listener.batches.size() == 1);
      CHECK(std::holds_alternative<SourceReset>(listener.batches.front().deltas.front()));
      CHECK(source.indexOf(TrackId{10}) == std::nullopt);
    }
  }

  TEST_CASE("AllTracksSource - one collection transaction publishes one sequential batch",
            "[runtime][unit][source][all-tracks]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto source = AllTracksSource{libraryFixture.library().tracks()};
    auto batches = std::vector<TrackSourceDeltaBatch>{};
    auto subscription = source.subscribe([&](TrackSourceDeltaBatch const& batch) { batches.push_back(batch); });
    auto const initialInsertions = std::array{TrackId{30}, TrackId{10}, TrackId{20}};

    source.applyCollectionChange(initialInsertions, {});

    REQUIRE(batches.size() == 1);
    REQUIRE(batches[0].deltas.size() == 1);
    auto const& initial = std::get<SourceInsertRange>(batches[0].deltas[0]);
    CHECK(initial.start == 0);
    CHECK(initial.trackIds == std::vector{TrackId{10}, TrackId{20}, TrackId{30}});

    auto const inserted = std::array{TrackId{25}, TrackId{15}};
    auto const removed = std::array{TrackId{20}};
    source.applyCollectionChange(inserted, removed);

    REQUIRE(batches.size() == 2);
    REQUIRE(batches[1].deltas.size() == 2);
    auto const& removal = std::get<SourceRemoveRange>(batches[1].deltas[0]);
    CHECK(removal.start == 1);
    CHECK(removal.trackIds == std::vector{TrackId{20}});
    auto const& insertion = std::get<SourceInsertRange>(batches[1].deltas[1]);
    CHECK(insertion.start == 1);
    CHECK(insertion.trackIds == std::vector{TrackId{15}, TrackId{25}});
    REQUIRE(source.size() == 4);
    CHECK(source.trackIdAt(0) == TrackId{10});
    CHECK(source.trackIdAt(1) == TrackId{15});
    CHECK(source.trackIdAt(2) == TrackId{25});
    CHECK(source.trackIdAt(3) == TrackId{30});
  }
} // namespace ao::rt::test
