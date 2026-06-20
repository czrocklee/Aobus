// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TestUtils.h"
#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/source/AllTracksSource.h>
#include <ao/rt/source/TrackSource.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::library;

  struct MockTrackSourceObserver : TrackSourceObserver
  {
    std::vector<std::pair<TrackId, std::size_t>> inserted;
    std::vector<std::pair<TrackId, std::size_t>> removed;
    std::int32_t resets = 0;

    void onInserted(TrackId id, std::size_t index) override { inserted.emplace_back(id, index); }
    void onRemoved(TrackId id, std::size_t index) override { removed.emplace_back(id, index); }
    void onUpdated(TrackId /*id*/, std::size_t /*index*/) override {}
    void onReset() override { resets++; }
  };

  TEST_CASE("AllTracksSource - Operations", "[runtime][unit][AllTracksSource]")
  {
    auto testLib = TestMusicLibrary{};
    auto& store = testLib.library().tracks();

    auto source = AllTracksSource{store};
    auto listener = MockTrackSourceObserver{};
    source.attach(&listener);

    SECTION("reloadFromStore populates and notifies reset")
    {
      auto const t1 = testLib.addTrack("A");
      auto const t2 = testLib.addTrack("B");

      auto const txn = testLib.library().readTransaction();
      source.reloadFromStore(txn);

      REQUIRE(listener.resets == 1);
      REQUIRE(source.size() == 2);

      auto const optI1 = source.indexOf(t1);
      auto const optI2 = source.indexOf(t2);

      REQUIRE(optI1);
      REQUIRE(optI2);
      REQUIRE(source.trackIdAt(*optI1) == t1);
      REQUIRE(source.trackIdAt(*optI2) == t2);
    }

    SECTION("notifyInserted adds item and notifies")
    {
      source.notifyInserted(TrackId{10});
      REQUIRE(listener.inserted.size() == 1);
      REQUIRE(listener.inserted[0] == std::pair{TrackId{10}, std::size_t{0}});
      REQUIRE(source.size() == 1);

      source.notifyInserted(TrackId{20});
      REQUIRE(listener.inserted.size() == 2);
      REQUIRE(listener.inserted[1] == std::pair{TrackId{20}, std::size_t{1}});
      REQUIRE(source.size() == 2);

      // Insert smaller id, should be at index 0
      source.notifyInserted(TrackId{5});
      REQUIRE(listener.inserted.size() == 3);
      REQUIRE(listener.inserted[2] == std::pair{TrackId{5}, std::size_t{0}});

      // Duplicate insert shouldn't trigger
      source.notifyInserted(TrackId{10});
      REQUIRE(listener.inserted.size() == 3);
    }

    SECTION("notifyRemoved removes item and notifies")
    {
      source.notifyInserted(TrackId{10});
      source.notifyInserted(TrackId{20});

      listener.inserted.clear(); // Reset to clear past calls

      source.notifyRemoved(TrackId{10});
      REQUIRE(listener.removed.size() == 1);
      REQUIRE(listener.removed[0] == std::pair{TrackId{10}, std::size_t{0}});
      REQUIRE(source.size() == 1);

      // Non-existent remove shouldn't trigger
      source.notifyRemoved(TrackId{99});
      REQUIRE(listener.removed.size() == 1);

      REQUIRE(source.indexOf(TrackId{10}) == std::nullopt);
    }

    SECTION("clear empties the source and resets")
    {
      source.notifyInserted(TrackId{10});
      source.notifyInserted(TrackId{20});

      source.clear();

      REQUIRE(source.size() == 0);
      REQUIRE(listener.resets == 1);
      REQUIRE(source.indexOf(TrackId{10}) == std::nullopt);
    }
  }
}
