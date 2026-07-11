// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/runtime/source/SmartListEvaluatorTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("SmartListEvaluator - a smart-list lease pins its upstream source", "[runtime][unit][source][smart-list]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto sourcePtr = makeMutableTrackSource({});
    auto weakSourcePtr = std::weak_ptr<MutableTrackSource>{sourcePtr};
    auto filteredPtr = std::make_unique<SmartListSource>(TrackSourceLease{sourcePtr}, libraryFixture.library(), engine);

    sourcePtr = nullptr;
    REQUIRE_FALSE(weakSourcePtr.expired());

    filteredPtr = nullptr;
    CHECK(weakSourcePtr.expired());
  }

  TEST_CASE("SmartListEvaluator - evaluator destruction detaches from sources", "[runtime][unit][source][smart-list]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto sourcePtr = makeMutableTrackSource({});
    auto enginePtr = std::make_unique<SmartListEvaluator>(libraryFixture.library());
    auto listPtr = std::make_unique<SmartListSource>(TrackSourceLease{sourcePtr}, libraryFixture.library(), *enginePtr);

    // Destroy engine BEFORE source and list
    // Note: list won't be able to reload anymore but we just want to hit the destructor
    enginePtr.reset();
    listPtr.reset();
  }

  TEST_CASE("SmartListEvaluator - upstream invalidation propagates terminally", "[runtime][unit][source][smart-list]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto sourcePtr = makeMutableTrackSource({});
    auto list = SmartListSource{TrackSourceLease{sourcePtr}, libraryFixture.library(), engine};
    list.reload();

    auto batches = std::vector<TrackSourceDeltaBatch>{};
    auto subscription = list.subscribe([&batches](TrackSourceDeltaBatch const& batch) { batches.push_back(batch); });

    sourcePtr->invalidate();

    CHECK(list.state() == TrackSourceState::Invalidated);
    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 1);
    CHECK(std::holds_alternative<SourceInvalidated>(batches.front().deltas.front()));

    sourcePtr->emitReset();
    CHECK(batches.size() == 1);
  }

  TEST_CASE("SmartListEvaluator - index lookup and source update forwarding work for filtered tracks",
            "[runtime][unit][source][smart-list]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto sourcePtr = makeMutableTrackSource({});
    auto& source = *sourcePtr;

    auto list = SmartListSource{TrackSourceLease{sourcePtr}, libraryFixture.library(), engine};
    list.setExpression("$year >= 2020");
    list.reload();

    auto t1 = libraryFixture.addTrack(makeSmartListSpec("Track1", 2020));
    auto t2 = libraryFixture.addTrack(makeSmartListSpec("Track2", 2010));
    auto t3 = libraryFixture.addTrack(makeSmartListSpec("Track3", 2021));

    source.batchInsert(std::array{t1, t2, t3});

    // Test SmartListSource::indexOf
    CHECK(list.indexOf(t1) == 0);                      // Present
    CHECK(list.indexOf(t3) == 1);                      // Present in upstream-relative order
    CHECK(list.indexOf(t2) == std::nullopt);           // Filtered out
    CHECK(list.indexOf(TrackId{999}) == std::nullopt); // Non-existent

    // Test TrackSource::notifyUpdated(TrackId id) without index
    auto spy = TrackSourceBatchSpy{source};

    // Call the base class method directly
    source.TrackSource::notifyUpdated(t3); // Calls TrackSource::notifyUpdated(id)

    REQUIRE(spy.batches.size() == 1);
    REQUIRE(spy.batches.front().deltas.size() == 1);
    auto const& update = std::get<SourceUpdateRange>(spy.batches.front().deltas.front());
    CHECK(update.start == 2);
    CHECK(update.trackIds == std::vector{t3});

    // t2 is not in source's indexOf (wait, it IS in source, just not in list)
    // Let's call it on a non-existent track
    spy.clear();
    source.TrackSource::notifyUpdated(TrackId{999});
    CHECK(spy.batches.empty()); // Should not notify since it's not in the source

    // Destructor for TrackSource is implicitly covered when MutableTrackSource is destroyed
  }
} // namespace ao::rt::test
