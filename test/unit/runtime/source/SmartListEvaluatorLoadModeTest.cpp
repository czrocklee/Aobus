// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/runtime/source/SmartListEvaluatorTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>

namespace ao::rt::test
{
  TEST_CASE("SmartListEvaluator - load mode optimization supports mixed access profiles",
            "[runtime][unit][source][smart-list][load-mode]")
  {
    auto testLibrary = TestMusicLibrary{};
    auto engine = SmartListEvaluator{testLibrary.library()};
    auto source = MutableTrackSource{};

    auto hotList = SmartListSource{source, testLibrary.library(), engine};
    hotList.setExpression("$year >= 2020"); // Hot metadata

    auto coldList = SmartListSource{source, testLibrary.library(), engine};
    coldList.setExpression("@duration >= 3m"); // Cold property

    auto t1 = testLibrary.addTrack(makeSmartListSpec("Track", 2022, std::chrono::seconds{200}));
    auto const batchArray = std::array{t1};
    source.batchInsert(batchArray);

    hotList.reload();
    coldList.reload();

    CHECK(hotList.size() == 1);
    CHECK(coldList.size() == 1);
  }

  TEST_CASE("SmartListEvaluator - hot and cold access profile optimization uses both readers",
            "[runtime][unit][source][smart-list][load-mode]")
  {
    auto testLibrary = TestMusicLibrary{};
    auto engine = SmartListEvaluator{testLibrary.library()};
    auto source = MutableTrackSource{};

    auto list = SmartListSource{source, testLibrary.library(), engine};
    // Requires both metadata and property reader
    list.setExpression("$year >= 2020 && @duration >= 3m");

    auto t1 = testLibrary.addTrack(makeSmartListSpec("Track", 2022, std::chrono::seconds{200}));
    auto t2 = testLibrary.addTrack(makeSmartListSpec("Bad", 2022, std::chrono::seconds{1}));
    auto const batchArray = std::array{t1, t2};
    source.batchInsert(batchArray);

    list.reload();
    CHECK(list.size() == 1);
    CHECK(list.trackIdAt(0) == t1);
  }
} // namespace ao::rt::test
