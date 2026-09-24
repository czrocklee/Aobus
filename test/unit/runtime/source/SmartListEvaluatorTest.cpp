// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "runtime/source/SmartListEvaluator.h"

#include "runtime/source/SmartListSource.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/runtime/source/SmartListEvaluatorTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
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
    auto filteredPtr = std::make_unique<SmartListSource>(TrackSourceLease{sourcePtr}, engine);

    sourcePtr = nullptr;
    REQUIRE_FALSE(weakSourcePtr.expired());

    filteredPtr = nullptr;
    CHECK(weakSourcePtr.expired());
  }

  TEST_CASE("SmartListEvaluator - evaluator destruction detaches from sources", "[runtime][unit][source][smart-list]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto first = libraryFixture.addTrack(makeSmartListSpec("first", 2020));
    auto second = libraryFixture.addTrack(makeSmartListSpec("second", 2021));
    auto third = libraryFixture.addTrack(makeSmartListSpec("third", 2022));
    auto sourcePtr = makeMutableTrackSource({first});
    auto enginePtr = std::make_unique<SmartListEvaluator>(libraryFixture.library());
    auto listPtr = std::make_unique<SmartListSource>(TrackSourceLease{sourcePtr}, *enginePtr);
    listPtr->setExpression("$year >= 2020");
    listPtr->reload();
    REQUIRE(sourceTrackIds(*listPtr) == std::vector{first});

    auto spy = TrackSourceBatchSpy{*listPtr};
    sourcePtr->insert(second, 1);

    REQUIRE(spy.batches.size() == 1);
    CHECK(sourceEditScript(spy.batches.front()) ==
          delta::RegularTrackEditScript{.edits = {delta::InsertRange{.start = 1, .trackIds = {second}}}});
    CHECK(sourceTrackIds(*listPtr) == std::vector{first, second});
    spy.clear();

    enginePtr.reset();
    sourcePtr->insert(third, 2);

    CHECK(spy.batches.empty());
    CHECK(sourceTrackIds(*sourcePtr) == std::vector{first, second, third});
    CHECK(listPtr->state() == TrackSourceState::Live);
    CHECK(sourceTrackIds(*listPtr) == std::vector{first, second});

    listPtr.reset();
  }

  TEST_CASE("SmartListEvaluator - upstream invalidation propagates terminally", "[runtime][unit][source][smart-list]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto trackId = libraryFixture.addTrack(makeSmartListSpec("matching", 2022));
    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto sourcePtr = makeMutableTrackSource({trackId});
    auto list = SmartListSource{TrackSourceLease{sourcePtr}, engine};
    list.setExpression("$year >= 2020");
    list.reload();
    REQUIRE(sourceTrackIds(list) == std::vector{trackId});

    auto batches = std::vector<TrackSourceDelta>{};
    auto callbackSizes = std::vector<std::size_t>{};
    auto subscription = list.subscribe(
      [&](TrackSourceDelta const& batch) noexcept
      {
        callbackSizes.push_back(list.size());
        batches.push_back(batch);
      });

    TrackSourceAccess::invalidate(*sourcePtr);
    TrackSourceAccess::invalidate(*sourcePtr);
    sourcePtr->emitReset();

    CHECK(list.state() == TrackSourceState::Invalidated);
    REQUIRE(batches.size() == 1);
    CHECK(std::holds_alternative<SourceInvalidated>(batches.front()));
    CHECK(callbackSizes == std::vector<std::size_t>{0});
    CHECK(list.size() == 0);
  }

  TEST_CASE("SmartListEvaluator - index lookup and source update forwarding work for filtered tracks",
            "[runtime][unit][source][smart-list]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto sourcePtr = makeMutableTrackSource({});
    auto& source = *sourcePtr;

    auto list = SmartListSource{TrackSourceLease{sourcePtr}, engine};
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

    auto spy = TrackSourceBatchSpy{list};

    source.updateByIdentity(t3);

    REQUIRE(spy.batches.size() == 1);
    CHECK(sourceEditScript(spy.batches.front()) ==
          delta::RegularTrackEditScript{.edits = {delta::UpdateRange{.start = 1, .trackIds = {t3}}}});

    // The mutable input source does not publish an update for an absent identity.
    spy.clear();
    source.updateByIdentity(TrackId{999});
    CHECK(spy.batches.empty());
  }
} // namespace ao::rt::test
