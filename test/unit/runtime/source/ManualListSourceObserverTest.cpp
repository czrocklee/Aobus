// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/source/ManualListSourceTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/ManualListSource.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    std::vector<TrackId> storedTrackIdsOf(ManualListSource const& source)
    {
      auto const trackIds = source.storedTrackIds();
      return {trackIds.begin(), trackIds.end()};
    }
  } // namespace

  TEST_CASE("ManualListSource - parent hide and re-entry preserve manual intent and position",
            "[runtime][unit][source][manual-list]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{1}, TrackId{2}, TrackId{3}});
    auto view = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
    auto source = ManualListSource{view.view(), TrackSourceLease{parentPtr}};
    auto batches = std::vector<TrackSourceDelta>{};
    [[maybe_unused]] auto subscription =
      source.subscribe([&batches](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    parentPtr->remove(TrackId{2});

    auto const hiddenEffective = std::vector{TrackId{1}, TrackId{3}};
    REQUIRE(batches.size() == 1);
    REQUIRE(sourceEditScript(batches[0]).edits.size() == 1);
    auto const& removal = std::get<delta::RemoveRange>(sourceEditScript(batches[0]).edits.front());
    CHECK(removal.start == 1);
    CHECK(removal.trackIds == std::vector{TrackId{2}});
    CHECK(sourceTrackIds(source) == hiddenEffective);

    parentPtr->insert(TrackId{2}, 0);

    auto const expectedStored = std::vector{TrackId{1}, TrackId{2}, TrackId{3}};
    REQUIRE(batches.size() == 2);
    REQUIRE(sourceEditScript(batches[1]).edits.size() == 1);
    auto const& insertion = std::get<delta::InsertRange>(sourceEditScript(batches[1]).edits.front());
    CHECK(insertion.start == 1);
    CHECK(insertion.trackIds == std::vector{TrackId{2}});
    CHECK(sourceTrackIds(source) == expectedStored);
    CHECK(storedTrackIdsOf(source) == expectedStored);
  }

  TEST_CASE("ManualListSource - quick filter remains a stable subsequence of stored manual order",
            "[runtime][unit][source][manual-list]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack(library::test::makeTrackSpec("First", 2024));
    auto const hidden = libraryFixture.addTrack(library::test::makeTrackSpec("Hidden", 2010));
    auto const second = libraryFixture.addTrack(library::test::makeTrackSpec("Second", 2024));
    auto const third = libraryFixture.addTrack(library::test::makeTrackSpec("Third", 2024));
    auto parentPtr = makeMutableTrackSource({third, hidden, first, second});
    auto view = ListViewOwner{{second, hidden, third, first}};
    auto manualPtr = std::make_shared<ManualListSource>(view.view(), TrackSourceLease{parentPtr});
    auto evaluator = SmartListEvaluator{libraryFixture.library()};
    auto quickFilter = SmartListSource{TrackSourceLease{manualPtr}, evaluator};
    quickFilter.setExpression("$year >= 2020");
    quickFilter.reload();

    CHECK(sourceTrackIds(*manualPtr) == std::vector{second, hidden, third, first});
    CHECK(sourceTrackIds(quickFilter) == std::vector{second, third, first});
    auto quickFilterSpy = TrackSourceBatchSpy{quickFilter};

    manualPtr->applyManualEditScript(delta::RegularTrackEditScript{
      .edits = {delta::RemoveRange{.start = 2, .trackIds = {third}},
                delta::InsertRange{.start = 0, .trackIds = {third}}},
    });

    CHECK(sourceTrackIds(*manualPtr) == std::vector{third, second, hidden, first});
    CHECK(sourceTrackIds(quickFilter) == std::vector{third, second, first});
    REQUIRE(quickFilterSpy.batches.size() == 1);
    REQUIRE(sourceEditScript(quickFilterSpy.batches.front()).edits.size() == 2);
    auto const& removal = std::get<delta::RemoveRange>(sourceEditScript(quickFilterSpy.batches.front()).edits[0]);
    auto const& insertion = std::get<delta::InsertRange>(sourceEditScript(quickFilterSpy.batches.front()).edits[1]);
    CHECK(removal.start == 1);
    CHECK(removal.trackIds == std::vector{third});
    CHECK(insertion.start == 0);
    CHECK(insertion.trackIds == std::vector{third});
  }

  TEST_CASE("ManualListSource - ignores parent reorder when membership is unchanged",
            "[runtime][unit][source][manual-list]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{1}, TrackId{2}, TrackId{3}});
    auto view = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
    auto source = ManualListSource{view.view(), TrackSourceLease{parentPtr}};
    auto batches = std::vector<TrackSourceDelta>{};
    [[maybe_unused]] auto subscription =
      source.subscribe([&batches](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    parentPtr->replaceWithBatch(std::vector{TrackId{3}, TrackId{1}, TrackId{2}},
                                delta::RegularTrackEditScript{
                                  .edits = {delta::RemoveRange{.start = 2, .trackIds = {TrackId{3}}},
                                            delta::InsertRange{.start = 0, .trackIds = {TrackId{3}}}},
                                });

    auto const expected = std::vector{TrackId{1}, TrackId{2}, TrackId{3}};
    CHECK(sourceTrackIds(source) == expected);
    CHECK(batches.empty());
  }

  TEST_CASE("ManualListSource - translates one mixed parent batch into one child batch",
            "[runtime][unit][source][manual-list]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{1}, TrackId{2}, TrackId{3}});
    auto view = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}}};
    auto source = ManualListSource{view.view(), TrackSourceLease{parentPtr}};
    auto batches = std::vector<TrackSourceDelta>{};
    [[maybe_unused]] auto subscription =
      source.subscribe([&batches](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    parentPtr->replaceWithBatch(std::vector{TrackId{1}, TrackId{3}, TrackId{4}},
                                delta::RegularTrackEditScript{
                                  .edits = {delta::RemoveRange{.start = 1, .trackIds = {TrackId{2}}},
                                            delta::InsertRange{.start = 2, .trackIds = {TrackId{4}}}},
                                });

    auto const expected = std::vector{TrackId{1}, TrackId{3}, TrackId{4}};
    REQUIRE(batches.size() == 1);
    REQUIRE(sourceEditScript(batches.front()).edits.size() == 2);
    auto const& removal = std::get<delta::RemoveRange>(sourceEditScript(batches.front()).edits[0]);
    auto const& insertion = std::get<delta::InsertRange>(sourceEditScript(batches.front()).edits[1]);
    CHECK(removal.start == 1);
    CHECK(removal.trackIds == std::vector{TrackId{2}});
    CHECK(insertion.start == 2);
    CHECK(insertion.trackIds == std::vector{TrackId{4}});
    CHECK(sourceTrackIds(source) == expected);
  }

  TEST_CASE("ManualListSource - coalesces parent updates in effective manual coordinates",
            "[runtime][unit][source][manual-list]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}});
    auto view = ListViewOwner{{TrackId{1}, TrackId{3}}};
    auto source = ManualListSource{view.view(), TrackSourceLease{parentPtr}};
    auto batches = std::vector<TrackSourceDelta>{};
    [[maybe_unused]] auto subscription =
      source.subscribe([&batches](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    parentPtr->publishBatch(delta::RegularTrackEditScript{
      .edits = {delta::UpdateRange{
        .start = 0,
        .trackIds = {TrackId{1}, TrackId{2}, TrackId{3}},
      }},
    });

    REQUIRE(batches.size() == 1);
    REQUIRE(sourceEditScript(batches.front()).edits.size() == 1);
    auto const& update = std::get<delta::UpdateRange>(sourceEditScript(batches.front()).edits.front());
    auto const expectedUpdated = std::vector{TrackId{1}, TrackId{3}};
    CHECK(update.start == 0);
    CHECK(update.trackIds == expectedUpdated);
  }

  TEST_CASE("ManualListSource - hidden parent changes do not publish a child batch",
            "[runtime][unit][source][manual-list]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{1}, TrackId{9}});
    auto view = ListViewOwner{{TrackId{1}, TrackId{2}}};
    auto source = ManualListSource{view.view(), TrackSourceLease{parentPtr}};
    auto batches = std::vector<TrackSourceDelta>{};
    [[maybe_unused]] auto subscription =
      source.subscribe([&batches](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    parentPtr->append(TrackId{8});
    parentPtr->update(TrackId{9});
    parentPtr->remove(TrackId{8});

    CHECK(sourceTrackIds(source) == std::vector{TrackId{1}});
    CHECK(batches.empty());
  }

  TEST_CASE("ManualListSource - parent reset rebuilds effective state without discarding stored intent",
            "[runtime][unit][source][manual-list]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{1}, TrackId{2}, TrackId{3}});
    auto view = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
    auto source = ManualListSource{view.view(), TrackSourceLease{parentPtr}};
    auto batches = std::vector<TrackSourceDelta>{};
    [[maybe_unused]] auto subscription =
      source.subscribe([&batches](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    parentPtr->reset(std::vector{TrackId{3}, TrackId{1}});

    auto const expectedStored = std::vector{TrackId{1}, TrackId{2}, TrackId{3}};
    auto const expectedEffective = std::vector{TrackId{1}, TrackId{3}};
    REQUIRE(batches.size() == 1);
    CHECK(std::holds_alternative<SourceReset>(batches.front()));
    CHECK(sourceTrackIds(source) == expectedEffective);
    CHECK(storedTrackIdsOf(source) == expectedStored);
  }

  TEST_CASE("ManualListSource - parent invalidation is terminal and emitted once",
            "[runtime][unit][source][manual-list]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{1}, TrackId{2}});
    auto view = ListViewOwner{{TrackId{1}, TrackId{2}}};
    auto source = ManualListSource{view.view(), TrackSourceLease{parentPtr}};
    auto batches = std::vector<TrackSourceDelta>{};
    [[maybe_unused]] auto subscription =
      source.subscribe([&batches](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    parentPtr->invalidate();
    parentPtr->invalidate();

    REQUIRE(batches.size() == 1);
    CHECK(std::holds_alternative<SourceInvalidated>(batches.front()));
    CHECK(source.state() == TrackSourceState::Invalidated);
  }

  TEST_CASE("ManualListSource - invalidation propagates through a leased manual chain",
            "[runtime][unit][source][manual-list]")
  {
    auto rootPtr = makeMutableTrackSource({TrackId{1}, TrackId{2}, TrackId{3}});
    auto innerView = ListViewOwner{{TrackId{1}, TrackId{2}}};
    auto innerPtr = std::make_shared<ManualListSource>(innerView.view(), TrackSourceLease{rootPtr});
    auto outerView = ListViewOwner{{TrackId{2}}};
    auto outer = ManualListSource{outerView.view(), TrackSourceLease{innerPtr}};
    auto innerBatches = std::vector<TrackSourceDelta>{};
    auto outerBatches = std::vector<TrackSourceDelta>{};
    [[maybe_unused]] auto innerSubscription =
      innerPtr->subscribe([&innerBatches](TrackSourceDelta const& batch) noexcept { innerBatches.push_back(batch); });
    [[maybe_unused]] auto outerSubscription =
      outer.subscribe([&outerBatches](TrackSourceDelta const& batch) noexcept { outerBatches.push_back(batch); });

    rootPtr->invalidate();

    CHECK(innerPtr->state() == TrackSourceState::Invalidated);
    CHECK(outer.state() == TrackSourceState::Invalidated);
    REQUIRE(innerBatches.size() == 1);
    REQUIRE(outerBatches.size() == 1);
    CHECK(std::holds_alternative<SourceInvalidated>(innerBatches.front()));
    CHECK(std::holds_alternative<SourceInvalidated>(outerBatches.front()));
  }

  TEST_CASE("ManualListSource - lease pins its parent for the subscription lifetime",
            "[runtime][unit][source][manual-list]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{1}});
    auto weakParentPtr = std::weak_ptr<MutableTrackSource>{parentPtr};
    auto view = ListViewOwner{{TrackId{1}}};

    {
      auto source = ManualListSource{view.view(), TrackSourceLease{parentPtr}};
      parentPtr = nullptr;

      CHECK_FALSE(weakParentPtr.expired());
      CHECK(sourceTrackIds(source) == std::vector{TrackId{1}});
    }

    CHECK(weakParentPtr.expired());
  }
} // namespace ao::rt::test
