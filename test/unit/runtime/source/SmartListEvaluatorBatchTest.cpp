// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/source/SmartListEvaluatorTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

namespace ao::rt::test
{
  TEST_CASE("SmartListEvaluator - batch operations emit batch notifications",
            "[runtime][unit][source][smart-list][batch]")
  {
    auto testLibrary = TestMusicLibrary{};
    auto engine = SmartListEvaluator{testLibrary.library()};
    auto source = MutableTrackSource{};

    auto list = SmartListSource{source, testLibrary.library(), engine};
    list.setExpression("$year >= 2020");
    list.reload();

    auto spy = TrackSourceObserverSpy{};
    list.attach(&spy);

    auto t1 = testLibrary.addTrack(makeSmartListSpec("Old", 2010));
    auto t2 = testLibrary.addTrack(makeSmartListSpec("New1", 2021));
    auto t3 = testLibrary.addTrack(makeSmartListSpec("New2", 2022));

    auto const batchArray = std::array{t1, t2, t3};
    source.batchInsert(batchArray);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::BatchInserted);
    // t1 should be filtered out
    CHECK(spy.events[0].batchIds.size() == 2);
    CHECK(std::ranges::contains(spy.events[0].batchIds, t2));
    CHECK(std::ranges::contains(spy.events[0].batchIds, t3));
    CHECK(list.size() == 2);

    spy.clear();
    auto const removeArray = std::array{t2};
    source.batchRemove(removeArray);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::BatchRemoved);
    REQUIRE(spy.events[0].batchIds.size() == 1);
    CHECK(spy.events[0].batchIds[0] == t2);
    CHECK(list.size() == 1);

    list.detach(&spy);
  }

  TEST_CASE("SmartListEvaluator - batch mutations trigger flat_set optimizations",
            "[runtime][unit][source][smart-list][batch]")
  {
    auto testLibrary = TestMusicLibrary{};
    auto engine = SmartListEvaluator{testLibrary.library()};
    auto source = MutableTrackSource{};

    auto list = SmartListSource{source, testLibrary.library(), engine};
    list.setExpression("$year >= 2020");
    list.reload();

    auto t1 = testLibrary.addTrack(makeSmartListSpec("Old", 2010));
    auto t2 = testLibrary.addTrack(makeSmartListSpec("New1", 2021));
    auto t3 = testLibrary.addTrack(makeSmartListSpec("New2", 2022));

    auto const batchArray = std::array{t1, t2, t3};
    source.batchInsert(batchArray);
    CHECK(list.size() == 2);

    auto const removeArray = std::array{t1};
    source.batchRemove(removeArray);
    CHECK(list.size() == 2); // t2 and t3

    // Trigger batch update that causes transitions
    // t2 matches -> won't match (removed)
    // t1 doesn't match -> will match (inserted)
    // t3 matches -> still matches (updated)
    testLibrary.updateTrack(t2, [](library::test::TrackSpec& spec) { spec.year = 2010; }); // Remove
    source.insert(t1, source.size()); // re-add t1 to source so it can be updated
    testLibrary.updateTrack(t1, [](library::test::TrackSpec& spec) { spec.year = 2025; });       // Insert
    testLibrary.updateTrack(t3, [](library::test::TrackSpec& spec) { spec.title = "Updated"; }); // Update

    auto const updateArray = std::array{t1, t2, t3};
    source.batchUpdate(updateArray);

    CHECK(list.size() == 2); // t1 and t3
  }
} // namespace ao::rt::test
