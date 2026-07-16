// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/source/SmartListEvaluatorTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("SmartListEvaluator - reloading one list does not reset sibling lists sharing a source",
            "[runtime][unit][smart-list][incremental]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto first = libraryFixture.addTrack(makeSmartListSpec("first", 2020, std::chrono::seconds{180}));
    auto second = libraryFixture.addTrack(makeSmartListSpec("second", 2022, std::chrono::seconds{260}));

    auto sourcePtr = makeMutableTrackSource({first, second});

    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto hotList = SmartListSource{TrackSourceLease{sourcePtr}, engine};
    auto coldList = SmartListSource{TrackSourceLease{sourcePtr}, engine};
    hotList.setExpression("$year >= 2021");
    hotList.reload();
    coldList.setExpression("@duration >= 4m");
    coldList.reload();

    auto hotSpy = TrackSourceBatchSpy{hotList};
    auto coldSpy = TrackSourceBatchSpy{coldList};

    hotList.setExpression("$year >= 2022");
    hotList.reload();

    REQUIRE(hotSpy.batches.size() == 1);
    REQUIRE(hotSpy.batches.front().deltas.size() == 1);
    CHECK(std::holds_alternative<SourceReset>(hotSpy.batches.front().deltas.front()));
    CHECK(coldSpy.batches.empty());
    REQUIRE(hotList.size() == 1);
    CHECK(hotList.trackIdAt(0) == second);
    REQUIRE(coldList.size() == 1);
    CHECK(coldList.trackIdAt(0) == second);
  }

  TEST_CASE("SmartListEvaluator - source insert emits inserted notifications instead of bucket resets",
            "[runtime][unit][smart-list][incremental]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto original = libraryFixture.addTrack(makeSmartListSpec("old", 2020, std::chrono::seconds{180}));

    auto sourcePtr = makeMutableTrackSource({original});
    auto& source = *sourcePtr;

    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto hotList = SmartListSource{TrackSourceLease{sourcePtr}, engine};
    auto coldList = SmartListSource{TrackSourceLease{sourcePtr}, engine};
    hotList.setExpression("$year >= 2021");
    hotList.reload();
    coldList.setExpression("@duration >= 4m");
    coldList.reload();

    auto hotSpy = TrackSourceBatchSpy{hotList};
    auto coldSpy = TrackSourceBatchSpy{coldList};

    auto inserted = libraryFixture.addTrack(makeSmartListSpec("new", 2023, std::chrono::seconds{250}));
    source.insert(inserted, 1);

    REQUIRE(hotSpy.batches.size() == 1);
    REQUIRE(hotSpy.batches.front().deltas.size() == 1);
    auto const& hotInsert = std::get<SourceInsertRange>(hotSpy.batches.front().deltas.front());
    CHECK(hotInsert.start == 0);
    CHECK(hotInsert.trackIds == std::vector{inserted});
    REQUIRE(coldSpy.batches.size() == 1);
    REQUIRE(coldSpy.batches.front().deltas.size() == 1);
    auto const& coldInsert = std::get<SourceInsertRange>(coldSpy.batches.front().deltas.front());
    CHECK(coldInsert.start == 0);
    CHECK(coldInsert.trackIds == std::vector{inserted});
    REQUIRE(hotList.size() == 1);
    CHECK(hotList.trackIdAt(0) == inserted);
    REQUIRE(coldList.size() == 1);
    CHECK(coldList.trackIdAt(0) == inserted);
  }

  TEST_CASE("SmartListEvaluator - source update diffs membership transitions incrementally",
            "[runtime][unit][smart-list][incremental]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto trackId = libraryFixture.addTrack(makeSmartListSpec("track", 2020));

    auto sourcePtr = makeMutableTrackSource({trackId});
    auto& source = *sourcePtr;

    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto filtered = SmartListSource{TrackSourceLease{sourcePtr}, engine};
    filtered.setExpression("$year >= 2021");
    filtered.reload();

    auto spy = TrackSourceBatchSpy{filtered};

    libraryFixture.updateTrack(trackId, [](library::test::TrackSpec& spec) { spec.year = 2022; });
    source.update(trackId);

    REQUIRE(spy.batches.size() == 1);
    auto const& insertion = std::get<SourceInsertRange>(spy.batches.front().deltas.front());
    CHECK(insertion.start == 0);
    CHECK(insertion.trackIds == std::vector{trackId});

    spy.clear();

    libraryFixture.updateTrack(trackId, [](library::test::TrackSpec& spec) { spec.title = "renamed"; });
    source.update(trackId);

    REQUIRE(spy.batches.size() == 1);
    auto const& update = std::get<SourceUpdateRange>(spy.batches.front().deltas.front());
    CHECK(update.start == 0);
    CHECK(update.trackIds == std::vector{trackId});

    spy.clear();

    libraryFixture.updateTrack(trackId, [](library::test::TrackSpec& spec) { spec.year = 2019; });
    source.update(trackId);

    REQUIRE(spy.batches.size() == 1);
    auto const& removal = std::get<SourceRemoveRange>(spy.batches.front().deltas.front());
    CHECK(removal.start == 0);
    CHECK(removal.trackIds == std::vector{trackId});
    CHECK(filtered.size() == 0);
  }

  TEST_CASE("SmartListEvaluator - child filtered lists track parent membership as their source",
            "[runtime][unit][smart-list][incremental]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto legacy = libraryFixture.addTrack(makeSmartListSpec("legacy", 2020, std::chrono::seconds{180}));
    auto modern = libraryFixture.addTrack(makeSmartListSpec("modern", 2022, std::chrono::seconds{250}));

    auto sourcePtr = makeMutableTrackSource({legacy, modern});
    auto& source = *sourcePtr;

    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto parentPtr = std::make_shared<SmartListSource>(TrackSourceLease{sourcePtr}, engine);
    auto& parent = *parentPtr;
    parent.setExpression("$year >= 2021");
    parent.reload();

    auto child = SmartListSource{TrackSourceLease{parentPtr}, engine};
    child.setExpression("@duration >= 4m");
    child.reload();

    REQUIRE(parent.size() == 1);
    CHECK(parent.trackIdAt(0) == modern);
    REQUIRE(child.size() == 1);
    CHECK(child.trackIdAt(0) == modern);

    auto spy = TrackSourceBatchSpy{child};

    auto fresh = libraryFixture.addTrack(makeSmartListSpec("fresh", 2023, std::chrono::seconds{260}));
    source.insert(fresh, 2);

    REQUIRE(spy.batches.size() == 1);
    auto const& childInsert = std::get<SourceInsertRange>(spy.batches.front().deltas.front());
    CHECK(childInsert.start == 1);
    CHECK(childInsert.trackIds == std::vector{fresh});
    REQUIRE(child.size() == 2);
    CHECK(child.trackIdAt(1) == fresh);

    spy.clear();

    libraryFixture.updateTrack(modern, [](library::test::TrackSpec& spec) { spec.year = 2019; });
    source.update(modern);

    REQUIRE(spy.batches.size() == 1);
    auto const& childRemoval = std::get<SourceRemoveRange>(spy.batches.front().deltas.front());
    CHECK(childRemoval.start == 0);
    CHECK(childRemoval.trackIds == std::vector{modern});
    REQUIRE(child.size() == 1);
    CHECK(child.trackIdAt(0) == fresh);
  }

  TEST_CASE("SmartListEvaluator - single mutations on base source are handled correctly",
            "[runtime][unit][smart-list][incremental]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto sourcePtr = makeMutableTrackSource({});
    auto& source = *sourcePtr;

    auto list = SmartListSource{TrackSourceLease{sourcePtr}, engine};
    list.setExpression("$year >= 2020");
    list.reload();

    auto t1 = libraryFixture.addTrack(makeSmartListSpec("Old", 2010));
    auto t2 = libraryFixture.addTrack(makeSmartListSpec("New1", 2021));

    // Single Insert
    source.insert(t2, source.size());
    CHECK(list.size() == 1);

    source.insert(t1, source.size());
    CHECK(list.size() == 1);

    // Single Update
    libraryFixture.updateTrack(t1, [](library::test::TrackSpec& spec) { spec.year = 2022; });
    source.update(t1);
    CHECK(list.size() == 2);

    // Single Remove
    source.remove(t2);
    CHECK(list.size() == 1);
  }

  TEST_CASE("SmartListEvaluator - relays SmartListSource::notifyUpdated to evaluator",
            "[runtime][unit][smart-list][incremental]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto sourcePtr = makeMutableTrackSource({});
    auto& source = *sourcePtr;

    auto list = SmartListSource{TrackSourceLease{sourcePtr}, engine};
    list.setExpression("$year >= 2020");
    list.reload();

    auto t1 = libraryFixture.addTrack(makeSmartListSpec("Track", 2020));
    auto const batchTrackIds = std::array{t1};
    source.batchInsert(batchTrackIds);

    // Mutate via base library, then notify list directly
    libraryFixture.updateTrack(t1, [](library::test::TrackSpec& spec) { spec.year = 2022; });
    list.notifyUpdated(t1);

    CHECK(list.size() == 1);
  }
} // namespace ao::rt::test
