// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/runtime/source/SmartListEvaluatorTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSource.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <optional>

namespace ao::rt::test
{
  TEST_CASE("SmartListEvaluator - source destruction is handled gracefully", "[runtime][unit][source][smart-list]")
  {
    auto testLibrary = TestMusicLibrary{};
    auto engine = SmartListEvaluator{testLibrary.library()};

    auto sourcePtr = std::make_unique<MutableTrackSource>();
    auto filteredPtr = std::make_unique<SmartListSource>(*sourcePtr, testLibrary.library(), engine);

    auto spy = TrackSourceObserverSpy{};
    filteredPtr->attach(&spy);

    // Destroy source while filtered list is still alive
    sourcePtr = nullptr;

    // Filtered list should be notified of reset (source is gone)
    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::Reset);

    // Now destroy filtered list - this calls unregisterList
    // This should not crash despite source being gone!
    filteredPtr.reset();
  }

  TEST_CASE("SmartListEvaluator - evaluator destruction detaches from sources", "[runtime][unit][source][smart-list]")
  {
    auto testLibrary = TestMusicLibrary{};
    auto source = MutableTrackSource{};
    auto enginePtr = std::make_unique<SmartListEvaluator>(testLibrary.library());
    auto listPtr = std::make_unique<SmartListSource>(source, testLibrary.library(), *enginePtr);

    // Destroy engine BEFORE source and list
    // Note: list won't be able to reload anymore but we just want to hit the destructor
    enginePtr.reset();
    listPtr.reset();
  }

  TEST_CASE("SmartListEvaluator - index lookup and source update forwarding work for filtered tracks",
            "[runtime][unit][source][smart-list]")
  {
    auto testLibrary = TestMusicLibrary{};
    auto engine = SmartListEvaluator{testLibrary.library()};
    auto source = MutableTrackSource{};

    auto list = SmartListSource{source, testLibrary.library(), engine};
    list.setExpression("$year >= 2020");
    list.reload();

    auto t1 = testLibrary.addTrack(makeSmartListSpec("Track1", 2020));
    auto t2 = testLibrary.addTrack(makeSmartListSpec("Track2", 2010));
    auto t3 = testLibrary.addTrack(makeSmartListSpec("Track3", 2021));

    source.batchInsert(std::to_array<TrackId>({t1, t2, t3}));

    // Test SmartListSource::indexOf
    CHECK(list.indexOf(t1) == 0);                      // Present
    CHECK(list.indexOf(t3) == 1);                      // Present (flat_set sorted by ID)
    CHECK(list.indexOf(t2) == std::nullopt);           // Filtered out
    CHECK(list.indexOf(TrackId{999}) == std::nullopt); // Non-existent

    // Test TrackSource::notifyUpdated(TrackId id) without index
    auto spy = TrackSourceObserverSpy{};
    source.attach(&spy);

    // Call the base class method directly
    source.TrackSource::notifyUpdated(t3); // Calls TrackSource::notifyUpdated(id)

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::Updated);
    CHECK(spy.events[0].id == t3);

    // t2 is not in source's indexOf (wait, it IS in source, just not in list)
    // Let's call it on a non-existent track
    spy.clear();
    source.TrackSource::notifyUpdated(TrackId{999});
    CHECK(spy.events.empty()); // Should not notify since it's not in the source

    source.detach(&spy);

    // Destructor for TrackSource is implicitly covered when MutableTrackSource is destroyed
  }
} // namespace ao::rt::test
