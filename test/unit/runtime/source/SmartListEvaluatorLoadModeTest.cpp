// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/runtime/source/SmartListEvaluatorTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>

namespace ao::rt::test
{
  TEST_CASE("SmartListEvaluator - load mode optimization supports mixed access profiles",
            "[runtime][unit][smart-list][load-mode]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto sourcePtr = makeMutableTrackSource({});
    auto& source = *sourcePtr;

    auto hotList = SmartListSource{TrackSourceLease{sourcePtr}, libraryFixture.library(), engine};
    hotList.setExpression("$year >= 2020"); // Hot metadata

    auto coldList = SmartListSource{TrackSourceLease{sourcePtr}, libraryFixture.library(), engine};
    coldList.setExpression("@duration >= 3m"); // Cold property

    auto t1 = libraryFixture.addTrack(makeSmartListSpec("Track", 2022, std::chrono::seconds{200}));
    auto const batchTrackIds = std::array{t1};
    source.batchInsert(batchTrackIds);

    hotList.reload();
    coldList.reload();

    CHECK(hotList.size() == 1);
    CHECK(coldList.size() == 1);
  }

  TEST_CASE("SmartListEvaluator - hot and cold access profile optimization uses both readers",
            "[runtime][unit][smart-list][load-mode]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto sourcePtr = makeMutableTrackSource({});
    auto& source = *sourcePtr;

    auto list = SmartListSource{TrackSourceLease{sourcePtr}, libraryFixture.library(), engine};
    // Requires both metadata and property reader
    list.setExpression("$year >= 2020 && @duration >= 3m");

    auto t1 = libraryFixture.addTrack(makeSmartListSpec("Track", 2022, std::chrono::seconds{200}));
    auto t2 = libraryFixture.addTrack(makeSmartListSpec("Bad", 2022, std::chrono::seconds{1}));
    auto const batchTrackIds = std::array{t1, t2};
    source.batchInsert(batchTrackIds);

    list.reload();
    CHECK(list.size() == 1);
    CHECK(list.trackIdAt(0) == t1);
  }
} // namespace ao::rt::test
