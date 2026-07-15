// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/source/SmartListEvaluatorTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/Error.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>

#include <variant>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("SmartListEvaluator - empty expression matches all tracks in upstream order",
            "[runtime][unit][smart-list][expression]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto first = libraryFixture.addTrack(makeSmartListSpec("first", 2020));
    auto second = libraryFixture.addTrack(makeSmartListSpec("second", 2021));

    auto sourcePtr = makeMutableTrackSource({second, first});

    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto filtered = SmartListSource{TrackSourceLease{sourcePtr}, libraryFixture.library(), engine};
    auto spy = TrackSourceBatchSpy{filtered};

    filtered.reload();

    CHECK_FALSE(filtered.hasError());
    REQUIRE(filtered.size() == 2);
    CHECK(filtered.trackIdAt(0) == second);
    CHECK(filtered.trackIdAt(1) == first);
    REQUIRE(spy.batches.size() == 1);
    REQUIRE(spy.batches.front().deltas.size() == 1);
    CHECK(std::holds_alternative<SourceReset>(spy.batches.front().deltas.front()));
  }

  TEST_CASE("SmartListEvaluator - invalid expression remains empty while sibling list receives inserts",
            "[runtime][unit][smart-list][expression]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto first = libraryFixture.addTrack(makeSmartListSpec("first", 2022));

    auto sourcePtr = makeMutableTrackSource({first});
    auto& source = *sourcePtr;

    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto validList = SmartListSource{TrackSourceLease{sourcePtr}, libraryFixture.library(), engine};
    auto invalidList = SmartListSource{TrackSourceLease{sourcePtr}, libraryFixture.library(), engine};
    validList.setExpression("$year >= 2021");
    validList.reload();
    invalidList.setExpression("   ");
    invalidList.reload();

    CHECK_FALSE(validList.hasError());
    CHECK(validList.size() == 1);
    CHECK(invalidList.hasError());
    REQUIRE(invalidList.error().has_value());
    CHECK(invalidList.error()->code == Error::Code::FormatRejected);
    CHECK_FALSE(invalidList.error()->message.empty());
    CHECK(invalidList.size() == 0);

    auto validSpy = TrackSourceBatchSpy{validList};
    auto invalidSpy = TrackSourceBatchSpy{invalidList};

    auto second = libraryFixture.addTrack(makeSmartListSpec("second", 2023));
    source.insert(second, 1);

    REQUIRE(validSpy.batches.size() == 1);
    REQUIRE(validSpy.batches.front().deltas.size() == 1);
    auto const& insertion = std::get<SourceInsertRange>(validSpy.batches.front().deltas.front());
    CHECK(insertion.start == 1);
    CHECK(insertion.trackIds == std::vector{second});
    CHECK(invalidSpy.batches.empty());
    REQUIRE(validList.size() == 2);
    CHECK(validList.trackIdAt(1) == second);
    CHECK(invalidList.size() == 0);
  }

  TEST_CASE("SmartListEvaluator - setting a valid expression clears invalid expression error",
            "[runtime][unit][smart-list][expression]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto first = libraryFixture.addTrack(makeSmartListSpec("first", 2022));

    auto sourcePtr = makeMutableTrackSource({first});

    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto validList = SmartListSource{TrackSourceLease{sourcePtr}, libraryFixture.library(), engine};
    auto invalidList = SmartListSource{TrackSourceLease{sourcePtr}, libraryFixture.library(), engine};
    validList.setExpression("$year >= 2021");
    validList.reload();
    invalidList.setExpression("   ");
    invalidList.reload();

    CHECK_FALSE(validList.hasError());
    CHECK(validList.size() == 1);
    CHECK(invalidList.hasError());
    REQUIRE(invalidList.error().has_value());
    CHECK(invalidList.error()->code == Error::Code::FormatRejected);
    CHECK_FALSE(invalidList.error()->message.empty());
    CHECK(invalidList.size() == 0);

    invalidList.setExpression("$year > 2000");
    invalidList.reload();

    CHECK_FALSE(invalidList.hasError());
    CHECK_FALSE(invalidList.error().has_value());
    CHECK(invalidList.size() == 1); // Only 'first' is in source at this point
  }

  TEST_CASE("SmartListEvaluator - existing plan binds dictionary symbols introduced by a later commit",
            "[runtime][unit][smart-list][regression]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack(makeSmartListSpec("future metadata", 2026));
    auto sourcePtr = makeMutableTrackSource({trackId});
    auto& source = *sourcePtr;

    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto filtered = SmartListSource{TrackSourceLease{sourcePtr}, libraryFixture.library(), engine};
    filtered.setExpression("#future and %rating = '5'");
    filtered.reload();

    CHECK_FALSE(filtered.hasError());
    CHECK(filtered.size() == 0);

    libraryFixture.updateTrack(trackId,
                               [](library::test::TrackSpec& spec)
                               {
                                 spec.tags.emplace_back("future");
                                 spec.customMetadata.emplace_back("rating", "5");
                               });
    source.update(trackId);

    REQUIRE(filtered.size() == 1);
    CHECK(filtered.trackIdAt(0) == trackId);
  }
} // namespace ao::rt::test
