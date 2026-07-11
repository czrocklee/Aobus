// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/source/SmartListEvaluatorTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>
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

    auto list = SmartListSource{TrackSourceLease{sourcePtr}, libraryFixture.library(), engine};
    list.setExpression("$year >= 2020");
    list.reload();

    auto batches = std::vector<TrackSourceDeltaBatch>{};
    auto subscription = list.subscribe([&batches](TrackSourceDeltaBatch const& batch) { batches.push_back(batch); });

    auto t1 = libraryFixture.addTrack(makeSmartListSpec("Old", 2010));
    auto t2 = libraryFixture.addTrack(makeSmartListSpec("New1", 2021));
    auto t3 = libraryFixture.addTrack(makeSmartListSpec("New2", 2022));

    auto const batchTrackIds = std::array{t1, t2, t3};
    source.batchInsert(batchTrackIds);

    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 1);
    auto const& inserted = std::get<SourceInsertRange>(batches.front().deltas.front());
    CHECK(inserted.start == 0);
    CHECK(inserted.trackIds == std::vector{t2, t3});
    CHECK(list.size() == 2);

    batches.clear();
    auto const removeTrackIds = std::array{t2};
    source.batchRemove(removeTrackIds);

    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 1);
    auto const& removed = std::get<SourceRemoveRange>(batches.front().deltas.front());
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

    auto list = SmartListSource{TrackSourceLease{sourcePtr}, libraryFixture.library(), engine};
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

    auto const updateTrackIds = std::array{t1, t2, t3};
    source.batchUpdate(updateTrackIds);

    CHECK(list.size() == 2); // t1 and t3
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
    auto list = SmartListSource{TrackSourceLease{sourcePtr}, libraryFixture.library(), engine};
    list.setExpression("$year >= 2020");
    list.reload();

    CHECK(sourceTrackIds(list) == std::vector{first, second, third});

    auto batches = std::vector<TrackSourceDeltaBatch>{};
    auto subscription = list.subscribe([&batches](TrackSourceDeltaBatch const& batch) { batches.push_back(batch); });

    sourcePtr->replaceWithBatch(std::array{first, third, hidden, second},
                                TrackSourceDeltaBatch{
                                  .deltas =
                                    {
                                      SourceRemoveRange{.start = 1, .trackIds = {hidden, second}},
                                      SourceInsertRange{.start = 2, .trackIds = {hidden, second}},
                                    },
                                });

    CHECK(sourceTrackIds(list) == std::vector{first, third, second});
    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 2);
    auto const& remove = std::get<SourceRemoveRange>(batches.front().deltas[0]);
    CHECK(remove.start == 1);
    CHECK(remove.trackIds == std::vector{second});
    auto const& insert = std::get<SourceInsertRange>(batches.front().deltas[1]);
    CHECK(insert.start == 2);
    CHECK(insert.trackIds == std::vector{second});
  }
} // namespace ao::rt::test
