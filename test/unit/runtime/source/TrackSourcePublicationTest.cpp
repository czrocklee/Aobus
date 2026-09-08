// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/LibraryWriteLane.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/runtime/library/LibraryWriteLaneTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/library/TrackWriter.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <expected>
#include <string_view>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("Track sources - mixed commit publishes final membership once before phase two",
            "[runtime][regression][source][concurrency]")
  {
    auto fixture = MusicLibraryFixture{};
    auto& storage = fixture.library();
    auto const a = fixture.addTrack(library::test::TrackSpec{.title = "A", .duration = std::chrono::minutes{4}});
    auto const c = fixture.addTrack(library::test::TrackSpec{.title = "C", .duration = std::chrono::minutes{1}});
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto changes = makeLibraryChanges(executor, storage);
    auto lane = LibraryWriteLane{runtime.callbackExecutor(), library::test::requireWritableLibrary(storage), changes};
    auto cache = TrackSourceCache{storage, changes};
    cache.reloadAllTracks();
    auto all = ao::test::requireValue(cache.acquire(kAllTracksListId));
    auto smart = ao::test::requireValue(
      cache.acquire(SourceSpec{.baseListId = kAllTracksListId, .filterExpression = "@duration >= 3m"}));
    REQUIRE(sourceTrackIds(all.source()) == std::vector{a, c});
    REQUIRE(sourceTrackIds(smart.source()) == std::vector{a});

    auto allBatches = TrackSourceBatchSpy{all.source()};
    auto smartBatches = TrackSourceBatchSpy{smart.source()};
    auto allSnapshots = std::vector<std::vector<TrackId>>{};
    auto smartSnapshots = std::vector<std::vector<TrackId>>{};
    auto phases = std::vector<std::string_view>{};
    auto allSubscription = all->subscribe(
      [&](TrackSourceDelta const&) noexcept
      {
        allSnapshots.push_back(sourceTrackIds(all.source()));
        phases.emplace_back("source");
      });
    auto smartSubscription = smart->subscribe(
      [&](TrackSourceDelta const&) noexcept
      {
        smartSnapshots.push_back(sourceTrackIds(smart.source()));
        phases.emplace_back("source");
      });
    auto published = std::vector<LibraryChangeSet>{};
    auto phaseTwoAll = std::vector<TrackId>{};
    auto phaseTwoSmart = std::vector<TrackId>{};
    auto changedSubscription = changes.onChanged(
      [&](LibraryChangeSet const& changeSet) noexcept
      {
        published.push_back(changeSet);
        phaseTwoAll = sourceTrackIds(all.source());
        phaseTwoSmart = sourceTrackIds(smart.source());
        phases.emplace_back("phase two");
      });

    auto result = runQueuedTask(
      runtime,
      executor,
      executeInteractiveMutation(
        lane.captureSubmission(),
        [&storage, a, c](library::LibraryWrite& write) -> Result<OperationOutcome<TrackId>>
        {
          auto writer = write.tracks();
          auto const optView = writer.get(a);

          if (!optView)
          {
            return std::unexpected{Error{.code = Error::Code::NotFound, .message = "Missing fixture track"}};
          }

          auto builder = library::TrackBuilder::fromCompleteView(*optView, storage.dictionary());
          builder.property().duration(std::chrono::minutes{1});

          if (auto updateRes = writer.update(a, builder); !updateRes)
          {
            return std::unexpected{updateRes.error()};
          }

          if (auto removeRes = writer.remove(c); !removeRes)
          {
            return std::unexpected{removeRes.error()};
          }

          auto const b = library::test::addTrackWithUniqueFixtureUri(
            storage, write, library::test::TrackSpec{.title = "B", .duration = std::chrono::minutes{4}});
          return Changed<TrackId>{
            .value = b,
            .changeSet = LibraryChangeSet{.tracksInserted = {b}, .tracksDeleted = {c}, .tracksMutated = {a}}};
        }));
    phases.emplace_back("completed");

    REQUIRE(result);
    REQUIRE(result->optCommittedRevision);
    auto const b = result->value;
    CHECK(allSnapshots == std::vector<std::vector<TrackId>>{{a, b}});
    CHECK(smartSnapshots == std::vector<std::vector<TrackId>>{{b}});
    CHECK(phases == std::vector<std::string_view>{"source", "source", "phase two", "completed"});
    CHECK(phaseTwoAll == std::vector{a, b});
    CHECK(phaseTwoSmart == std::vector{b});
    REQUIRE(published.size() == 1);
    CHECK(published.front().libraryRevision == *result->optCommittedRevision);
    CHECK(lane.availability().state == LibraryAuthoringState::Available);
    auto const transaction = storage.readTransaction();
    auto const reader = storage.tracks().reader(transaction);
    auto const optA = reader.get(a);
    auto const optB = reader.get(b);
    REQUIRE(optA);
    REQUIRE(optB);
    CHECK(optA->property().duration() == std::chrono::minutes{1});
    CHECK(optB->property().duration() == std::chrono::minutes{4});
    CHECK_FALSE(reader.get(c));
    REQUIRE(allBatches.batches.size() == 1);
    CHECK(sourceEditScript(allBatches.batches.front()) ==
          delta::RegularTrackEditScript{
            {delta::RemoveRange{1, {c}}, delta::InsertRange{1, {b}}, delta::UpdateRange{0, {a}}}});
    REQUIRE(smartBatches.batches.size() == 1);
    CHECK(sourceEditScript(smartBatches.batches.front()) ==
          delta::RegularTrackEditScript{{delta::RemoveRange{0, {a}}, delta::InsertRange{0, {b}}}});
  }
} // namespace ao::rt::test
