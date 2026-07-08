// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/source/SmartListEvaluatorTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>

namespace ao::rt::test
{
  TEST_CASE("SmartListEvaluator - reloading one list does not reset sibling lists sharing a source",
            "[runtime][unit][smart-list][incremental]")
  {
    auto testLibrary = TestMusicLibrary{};
    auto first = testLibrary.addTrack(makeSmartListSpec("first", 2020, std::chrono::seconds{180}));
    auto second = testLibrary.addTrack(makeSmartListSpec("second", 2022, std::chrono::seconds{260}));

    auto source = MutableTrackSource{};
    source.addInitial(first);
    source.addInitial(second);

    auto engine = SmartListEvaluator{testLibrary.library()};
    auto hotList = SmartListSource{source, testLibrary.library(), engine};
    auto coldList = SmartListSource{source, testLibrary.library(), engine};
    hotList.setExpression("$year >= 2021");
    hotList.reload();
    coldList.setExpression("@duration >= 4m");
    coldList.reload();

    auto hotSpy = TrackSourceObserverSpy{};
    auto coldSpy = TrackSourceObserverSpy{};
    hotList.attach(&hotSpy);
    coldList.attach(&coldSpy);

    hotList.setExpression("$year >= 2022");
    hotList.reload();

    REQUIRE(hotSpy.events.size() == 1);
    CHECK(hotSpy.events[0].kind == TrackSourceObserverSpy::EventKind::Reset);
    CHECK(coldSpy.events.empty());
    REQUIRE(hotList.size() == 1);
    CHECK(hotList.trackIdAt(0) == second);
    REQUIRE(coldList.size() == 1);
    CHECK(coldList.trackIdAt(0) == second);

    hotList.detach(&hotSpy);
    coldList.detach(&coldSpy);
  }

  TEST_CASE("SmartListEvaluator - source insert emits inserted notifications instead of bucket resets",
            "[runtime][unit][smart-list][incremental]")
  {
    auto testLibrary = TestMusicLibrary{};
    auto original = testLibrary.addTrack(makeSmartListSpec("old", 2020, std::chrono::seconds{180}));

    auto source = MutableTrackSource{};
    source.addInitial(original);

    auto engine = SmartListEvaluator{testLibrary.library()};
    auto hotList = SmartListSource{source, testLibrary.library(), engine};
    auto coldList = SmartListSource{source, testLibrary.library(), engine};
    hotList.setExpression("$year >= 2021");
    hotList.reload();
    coldList.setExpression("@duration >= 4m");
    coldList.reload();

    auto hotSpy = TrackSourceObserverSpy{};
    auto coldSpy = TrackSourceObserverSpy{};
    hotList.attach(&hotSpy);
    coldList.attach(&coldSpy);

    auto inserted = testLibrary.addTrack(makeSmartListSpec("new", 2023, std::chrono::seconds{250}));
    source.insert(inserted, 1);

    REQUIRE(hotSpy.events.size() == 1);
    CHECK(hotSpy.events[0].kind == TrackSourceObserverSpy::EventKind::Inserted);
    CHECK(hotSpy.events[0].id == inserted);
    CHECK(hotSpy.events[0].index == 0);
    REQUIRE(coldSpy.events.size() == 1);
    CHECK(coldSpy.events[0].kind == TrackSourceObserverSpy::EventKind::Inserted);
    CHECK(coldSpy.events[0].id == inserted);
    CHECK(coldSpy.events[0].index == 0);
    REQUIRE(hotList.size() == 1);
    CHECK(hotList.trackIdAt(0) == inserted);
    REQUIRE(coldList.size() == 1);
    CHECK(coldList.trackIdAt(0) == inserted);

    hotList.detach(&hotSpy);
    coldList.detach(&coldSpy);
  }

  TEST_CASE("SmartListEvaluator - source update diffs membership transitions incrementally",
            "[runtime][unit][smart-list][incremental]")
  {
    auto testLibrary = TestMusicLibrary{};
    auto trackId = testLibrary.addTrack(makeSmartListSpec("track", 2020));

    auto source = MutableTrackSource{};
    source.addInitial(trackId);

    auto engine = SmartListEvaluator{testLibrary.library()};
    auto filtered = SmartListSource{source, testLibrary.library(), engine};
    filtered.setExpression("$year >= 2021");
    filtered.reload();

    auto spy = TrackSourceObserverSpy{};
    filtered.attach(&spy);

    testLibrary.updateTrack(trackId, [](library::test::TrackSpec& spec) { spec.year = 2022; });
    source.update(trackId);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::Inserted);
    CHECK(spy.events[0].id == trackId);
    CHECK(spy.events[0].index == 0);

    spy.clear();

    testLibrary.updateTrack(trackId, [](library::test::TrackSpec& spec) { spec.title = "renamed"; });
    source.update(trackId);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::Updated);
    CHECK(spy.events[0].id == trackId);
    CHECK(spy.events[0].index == 0);

    spy.clear();

    testLibrary.updateTrack(trackId, [](library::test::TrackSpec& spec) { spec.year = 2019; });
    source.update(trackId);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::Removed);
    CHECK(spy.events[0].id == trackId);
    CHECK(spy.events[0].index == 0);
    CHECK(filtered.size() == 0);

    filtered.detach(&spy);
  }

  TEST_CASE("SmartListEvaluator - child filtered lists track parent membership as their source",
            "[runtime][unit][smart-list][incremental]")
  {
    auto testLibrary = TestMusicLibrary{};
    auto legacy = testLibrary.addTrack(makeSmartListSpec("legacy", 2020, std::chrono::seconds{180}));
    auto modern = testLibrary.addTrack(makeSmartListSpec("modern", 2022, std::chrono::seconds{250}));

    auto source = MutableTrackSource{};
    source.addInitial(legacy);
    source.addInitial(modern);

    auto engine = SmartListEvaluator{testLibrary.library()};
    auto parent = SmartListSource{source, testLibrary.library(), engine};
    parent.setExpression("$year >= 2021");
    parent.reload();

    auto child = SmartListSource{parent, testLibrary.library(), engine};
    child.setExpression("@duration >= 4m");
    child.reload();

    REQUIRE(parent.size() == 1);
    CHECK(parent.trackIdAt(0) == modern);
    REQUIRE(child.size() == 1);
    CHECK(child.trackIdAt(0) == modern);

    auto spy = TrackSourceObserverSpy{};
    child.attach(&spy);

    auto fresh = testLibrary.addTrack(makeSmartListSpec("fresh", 2023, std::chrono::seconds{260}));
    source.insert(fresh, 2);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::Inserted);
    CHECK(spy.events[0].id == fresh);
    CHECK(spy.events[0].index == 1);
    REQUIRE(child.size() == 2);
    CHECK(child.trackIdAt(1) == fresh);

    spy.clear();

    testLibrary.updateTrack(modern, [](library::test::TrackSpec& spec) { spec.year = 2019; });
    source.update(modern);

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::Removed);
    CHECK(spy.events[0].id == modern);
    CHECK(spy.events[0].index == 0);
    REQUIRE(child.size() == 1);
    CHECK(child.trackIdAt(0) == fresh);

    child.detach(&spy);
  }

  TEST_CASE("SmartListEvaluator - single mutations on base source are handled correctly",
            "[runtime][unit][smart-list][incremental]")
  {
    auto testLibrary = TestMusicLibrary{};
    auto engine = SmartListEvaluator{testLibrary.library()};
    auto source = MutableTrackSource{};

    auto list = SmartListSource{source, testLibrary.library(), engine};
    list.setExpression("$year >= 2020");
    list.reload();

    auto t1 = testLibrary.addTrack(makeSmartListSpec("Old", 2010));
    auto t2 = testLibrary.addTrack(makeSmartListSpec("New1", 2021));

    // Single Insert
    source.insert(t2, source.size());
    CHECK(list.size() == 1);

    source.insert(t1, source.size());
    CHECK(list.size() == 1);

    // Single Update
    testLibrary.updateTrack(t1, [](library::test::TrackSpec& spec) { spec.year = 2022; });
    source.update(t1);
    CHECK(list.size() == 2);

    // Single Remove
    source.remove(t2);
    CHECK(list.size() == 1);
  }

  TEST_CASE("SmartListEvaluator - relays SmartListSource::notifyUpdated to evaluator",
            "[runtime][unit][smart-list][incremental]")
  {
    auto testLibrary = TestMusicLibrary{};
    auto engine = SmartListEvaluator{testLibrary.library()};
    auto source = MutableTrackSource{};

    auto list = SmartListSource{source, testLibrary.library(), engine};
    list.setExpression("$year >= 2020");
    list.reload();

    auto t1 = testLibrary.addTrack(makeSmartListSpec("Track", 2020));
    auto const batchArray = std::array{t1};
    source.batchInsert(batchArray);

    // Mutate via base library, then notify list directly
    testLibrary.updateTrack(t1, [](library::test::TrackSpec& spec) { spec.year = 2022; });
    list.notifyUpdated(t1);

    CHECK(list.size() == 1);
  }
} // namespace ao::rt::test
