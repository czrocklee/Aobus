// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "runtime/source/SmartListEvaluator.h"
#include "runtime/source/SmartListSource.h"
#include "test/library/TrackTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include "test/runtime/source/SmartListEvaluatorTestSupport.h"
#include "test/runtime/source/TrackSourceTestSupport.h"
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("SmartListEvaluator - mixed-profile siblings keep distinct ordered membership after an invalid sibling")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack(makeSmartListSpec("first", 2024, std::chrono::seconds{60}));
    auto const second = libraryFixture.addTrack(makeSmartListSpec("second", 2010, std::chrono::seconds{250}));
    auto const both = libraryFixture.addTrack(makeSmartListSpec("both", 2024, std::chrono::seconds{250}));
    auto sourcePtr = makeMutableTrackSource({both, second, first});
    auto engine = SmartListEvaluator{libraryFixture.library()};

    auto invalidList = SmartListSource{TrackSourceLease{sourcePtr}, engine};
    auto hotList = SmartListSource{TrackSourceLease{sourcePtr}, engine};
    auto coldList = SmartListSource{TrackSourceLease{sourcePtr}, engine};
    invalidList.setExpression("   ");
    hotList.setExpression("$year >= 2020");
    coldList.setExpression("@duration >= 3m");

    hotList.reload();

    CHECK(invalidList.hasError());
    CHECK(invalidList.size() == 0);
    CHECK(sourceTrackIds(hotList) == std::vector{both, first});
    CHECK(sourceTrackIds(coldList) == std::vector{both, second});

    libraryFixture.updateTrack(first,
                               [](library::test::TrackSpec& spec)
                               {
                                 spec.year = 2010;
                                 spec.duration = std::chrono::seconds{250};
                               });
    libraryFixture.updateTrack(second,
                               [](library::test::TrackSpec& spec)
                               {
                                 spec.year = 2024;
                                 spec.duration = std::chrono::seconds{60};
                               });
    sourcePtr->batchUpdate(std::array{first, second});

    CHECK(invalidList.hasError());
    CHECK(invalidList.size() == 0);
    CHECK(sourceTrackIds(hotList) == std::vector{both, second});
    CHECK(sourceTrackIds(coldList) == std::vector{both, first});
  }

  TEST_CASE("SmartListEvaluator - a compound metadata and property predicate filters membership")
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
