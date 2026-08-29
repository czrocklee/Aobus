// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "runtime/RuntimeOperationProbe.h"
#include "runtime/source/SmartListEvaluator.h"
#include "runtime/source/SmartListSource.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/runtime/source/SmartListEvaluatorTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("SmartListEvaluator - batch operations emit batch notifications", "[runtime][unit][smart-list][batch]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto sourcePtr = makeMutableTrackSource({});
    auto& source = *sourcePtr;

    auto list = SmartListSource{TrackSourceLease{sourcePtr}, engine};
    list.setExpression("$year >= 2020");
    list.reload();

    auto batches = std::vector<TrackSourceDelta>{};
    auto subscription =
      list.subscribe([&batches](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    auto t1 = libraryFixture.addTrack(makeSmartListSpec("Old", 2010));
    auto t2 = libraryFixture.addTrack(makeSmartListSpec("New1", 2021));
    auto t3 = libraryFixture.addTrack(makeSmartListSpec("New2", 2022));

    auto const batchTrackIds = std::array{t1, t2, t3};
    source.batchInsert(batchTrackIds);

    REQUIRE(batches.size() == 1);
    REQUIRE(sourceEditScript(batches.front()).edits.size() == 1);
    auto const& inserted = std::get<delta::InsertRange>(sourceEditScript(batches.front()).edits.front());
    CHECK(inserted.start == 0);
    CHECK(inserted.trackIds == std::vector{t2, t3});
    CHECK(list.size() == 2);

    batches.clear();
    auto const removeTrackIds = std::array{t2};
    source.batchRemove(removeTrackIds);

    REQUIRE(batches.size() == 1);
    REQUIRE(sourceEditScript(batches.front()).edits.size() == 1);
    auto const& removed = std::get<delta::RemoveRange>(sourceEditScript(batches.front()).edits.front());
    CHECK(removed.start == 0);
    CHECK(removed.trackIds == std::vector{t2});
    CHECK(list.size() == 1);
  }

  TEST_CASE("SmartListEvaluator - batch updates combine membership transitions", "[runtime][unit][smart-list][batch]")
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
    auto t3 = libraryFixture.addTrack(makeSmartListSpec("New2", 2022));

    auto const batchTrackIds = std::array{t1, t2, t3};
    source.batchInsert(batchTrackIds);
    CHECK(list.size() == 2);

    auto const removeTrackIds = std::array{t1};
    source.batchRemove(removeTrackIds);
    CHECK(list.size() == 2); // t2 and t3

    // Trigger batch update that causes transitions
    // t2 matches -> won't match (removed)
    // t1 doesn't match -> will match (inserted)
    // t3 matches -> still matches (updated)
    libraryFixture.updateTrack(t2, [](library::test::TrackSpec& spec) { spec.year = 2010; }); // Remove
    source.insert(t1, source.size()); // re-add t1 to source so it can be updated
    libraryFixture.updateTrack(t1, [](library::test::TrackSpec& spec) { spec.year = 2025; });       // Insert
    libraryFixture.updateTrack(t3, [](library::test::TrackSpec& spec) { spec.title = "Updated"; }); // Update

    auto spy = TrackSourceBatchSpy{list};
    auto const operationCountsBefore = ::ao::rt::detail::RuntimeOperationProbe::counts(engine);
    auto const updateTrackIds = std::array{t1, t2, t3};
    source.batchUpdate(updateTrackIds);

    CHECK(sourceTrackIds(list) == std::vector{t3, t1});
    REQUIRE(spy.batches.size() == 1);
    auto const& script = sourceEditScript(spy.batches.front());
    REQUIRE(script.edits.size() == 3);
    CHECK(std::get<delta::RemoveRange>(script.edits[0]) == delta::RemoveRange{.start = 0, .trackIds = {t2}});
    CHECK(std::get<delta::InsertRange>(script.edits[1]) == delta::InsertRange{.start = 1, .trackIds = {t1}});
    CHECK(std::get<delta::UpdateRange>(script.edits[2]) == delta::UpdateRange{.start = 0, .trackIds = {t3}});
    auto const operationCountsAfter = ::ao::rt::detail::RuntimeOperationProbe::counts(engine);
    CHECK(operationCountsAfter.upstreamIndexRebuilds == operationCountsBefore.upstreamIndexRebuilds);
    CHECK(operationCountsAfter.membershipIndexRebuilds == operationCountsBefore.membershipIndexRebuilds);
  }

  TEST_CASE("SmartListEvaluator - filtered membership is an atomic stable subsequence of upstream order",
            "[runtime][unit][smart-list][batch]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack(makeSmartListSpec("first", 2024));
    auto const hidden = libraryFixture.addTrack(makeSmartListSpec("hidden", 2010));
    auto const second = libraryFixture.addTrack(makeSmartListSpec("second", 2024));
    auto const third = libraryFixture.addTrack(makeSmartListSpec("third", 2024));
    auto sourcePtr = makeMutableTrackSource({first, hidden, second, third});
    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto list = SmartListSource{TrackSourceLease{sourcePtr}, engine};
    list.setExpression("$year >= 2020");
    list.reload();

    CHECK(sourceTrackIds(list) == std::vector{first, second, third});

    auto batches = std::vector<TrackSourceDelta>{};
    auto subscription =
      list.subscribe([&batches](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    sourcePtr->replaceWithBatch(std::array{first, third, hidden, second},
                                delta::RegularTrackEditScript{
                                  .edits = {delta::RemoveRange{.start = 1, .trackIds = {hidden, second}},
                                            delta::InsertRange{.start = 2, .trackIds = {hidden, second}}},
                                });

    CHECK(sourceTrackIds(list) == std::vector{first, third, second});
    REQUIRE(batches.size() == 1);
    REQUIRE(sourceEditScript(batches.front()).edits.size() == 2);
    auto const& remove = std::get<delta::RemoveRange>(sourceEditScript(batches.front()).edits[0]);
    CHECK(remove.start == 1);
    CHECK(remove.trackIds == std::vector{second});
    auto const& insert = std::get<delta::InsertRange>(sourceEditScript(batches.front()).edits[1]);
    CHECK(insert.start == 2);
    CHECK(insert.trackIds == std::vector{second});
  }

  TEST_CASE("SmartListEvaluator - update-only batches preserve upstream order across several membership changes",
            "[runtime][regression][smart-list][batch]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const t0 = libraryFixture.addTrack(makeSmartListSpec("t0", 2024));
    auto const t1 = libraryFixture.addTrack(makeSmartListSpec("t1", 2010));
    auto const t2 = libraryFixture.addTrack(makeSmartListSpec("t2", 2024));
    auto const t3 = libraryFixture.addTrack(makeSmartListSpec("t3", 2010));
    auto const t4 = libraryFixture.addTrack(makeSmartListSpec("t4", 2024));
    auto const t5 = libraryFixture.addTrack(makeSmartListSpec("t5", 2010));
    auto const t6 = libraryFixture.addTrack(makeSmartListSpec("t6", 2024));
    auto const t7 = libraryFixture.addTrack(makeSmartListSpec("t7", 2010));
    auto sourcePtr = makeMutableTrackSource({t0, t1, t2, t3, t4, t5, t6, t7});
    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto list = SmartListSource{TrackSourceLease{sourcePtr}, engine};
    list.setExpression("$year >= 2020");
    list.reload();
    auto const previousMembers = sourceTrackIds(list);
    REQUIRE(previousMembers == std::vector{t0, t2, t4, t6});

    libraryFixture.updateTrack(t0, [](library::test::TrackSpec& spec) { spec.year = 2010; });
    libraryFixture.updateTrack(t1, [](library::test::TrackSpec& spec) { spec.year = 2024; });
    libraryFixture.updateTrack(t2, [](library::test::TrackSpec& spec) { spec.title = "updated t2"; });
    libraryFixture.updateTrack(t4, [](library::test::TrackSpec& spec) { spec.year = 2010; });
    libraryFixture.updateTrack(t5, [](library::test::TrackSpec& spec) { spec.year = 2024; });
    libraryFixture.updateTrack(t6, [](library::test::TrackSpec& spec) { spec.title = "updated t6"; });

    auto spy = TrackSourceBatchSpy{list};
    auto const operationCountsBefore = ::ao::rt::detail::RuntimeOperationProbe::counts(engine);
    sourcePtr->batchUpdate(std::array{t0, t1, t2, t4, t5, t6});

    auto const expectedMembers = std::vector{t1, t2, t5, t6};
    CHECK(sourceTrackIds(list) == expectedMembers);
    REQUIRE(spy.batches.size() == 1);
    auto const appliedRes = delta::apply(previousMembers, sourceEditScript(spy.batches.front()));
    REQUIRE(appliedRes);
    CHECK(*appliedRes == expectedMembers);
    auto const operationCountsAfter = ::ao::rt::detail::RuntimeOperationProbe::counts(engine);
    CHECK(operationCountsAfter.upstreamIndexRebuilds == operationCountsBefore.upstreamIndexRebuilds);
    CHECK(operationCountsAfter.membershipIndexRebuilds == operationCountsBefore.membershipIndexRebuilds);
  }
} // namespace ao::rt::test
