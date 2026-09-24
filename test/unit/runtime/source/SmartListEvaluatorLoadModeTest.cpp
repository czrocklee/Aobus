// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "runtime/source/SmartListEvaluator.h"
#include "runtime/source/SmartListSource.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/runtime/source/SmartListEvaluatorTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("SmartListEvaluator - one reload evaluates pending metadata and property predicates",
            "[runtime][unit][smart-list][membership]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto sourcePtr = makeMutableTrackSource({});
    auto& source = *sourcePtr;

    auto metadataList = SmartListSource{TrackSourceLease{sourcePtr}, engine};
    metadataList.setExpression("$year >= 2020");

    auto propertyList = SmartListSource{TrackSourceLease{sourcePtr}, engine};
    propertyList.setExpression("@duration >= 3m");

    auto t1 = libraryFixture.addTrack(makeSmartListSpec("Track", 2022, std::chrono::seconds{200}));
    auto const batchTrackIds = std::array{t1};
    source.batchInsert(batchTrackIds);

    metadataList.reload();

    CHECK(sourceTrackIds(metadataList) == std::vector{t1});
    CHECK(sourceTrackIds(propertyList) == std::vector{t1});
  }

  TEST_CASE("SmartListEvaluator - a compound metadata and property predicate filters membership",
            "[runtime][unit][smart-list][membership]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto engine = SmartListEvaluator{libraryFixture.library()};
    auto sourcePtr = makeMutableTrackSource({});
    auto& source = *sourcePtr;

    auto list = SmartListSource{TrackSourceLease{sourcePtr}, engine};
    list.setExpression("$year >= 2020 && @duration >= 3m");

    auto t1 = libraryFixture.addTrack(makeSmartListSpec("Track", 2022, std::chrono::seconds{200}));
    auto t2 = libraryFixture.addTrack(makeSmartListSpec("Bad", 2022, std::chrono::seconds{1}));
    auto const batchTrackIds = std::array{t1, t2};
    source.batchInsert(batchTrackIds);

    list.reload();
    CHECK(sourceTrackIds(list) == std::vector{t1});
  }
} // namespace ao::rt::test
