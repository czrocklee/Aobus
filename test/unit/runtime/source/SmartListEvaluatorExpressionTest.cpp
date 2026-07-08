// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/runtime/source/SmartListEvaluatorTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/Error.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::rt::test
{
  TEST_CASE("SmartListEvaluator - empty expression matches all tracks and maintains ID order",
            "[runtime][unit][smart-list][expression]")
  {
    auto testLibrary = TestMusicLibrary{};
    auto first = testLibrary.addTrack(makeSmartListSpec("first", 2020));
    auto second = testLibrary.addTrack(makeSmartListSpec("second", 2021));

    auto source = MutableTrackSource{};
    // Source is [second, first]
    source.addInitial(second);
    source.addInitial(first);

    auto engine = SmartListEvaluator{testLibrary.library()};
    auto filtered = SmartListSource{source, testLibrary.library(), engine};
    auto spy = TrackSourceObserverSpy{};
    filtered.attach(&spy);

    filtered.reload();

    CHECK_FALSE(filtered.hasError());
    REQUIRE(filtered.size() == 2);
    // flat_set maintains ID order: [first, second] since first(1) < second(2)
    CHECK(filtered.trackIdAt(0) == first);
    CHECK(filtered.trackIdAt(1) == second);
    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].kind == TrackSourceObserverSpy::EventKind::Reset);

    filtered.detach(&spy);
  }

  TEST_CASE("SmartListEvaluator - invalid expression remains empty while sibling list receives inserts",
            "[runtime][unit][smart-list][expression]")
  {
    auto testLibrary = TestMusicLibrary{};
    auto first = testLibrary.addTrack(makeSmartListSpec("first", 2022));

    auto source = MutableTrackSource{};
    source.addInitial(first);

    auto engine = SmartListEvaluator{testLibrary.library()};
    auto validList = SmartListSource{source, testLibrary.library(), engine};
    auto invalidList = SmartListSource{source, testLibrary.library(), engine};
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

    auto validSpy = TrackSourceObserverSpy{};
    auto invalidSpy = TrackSourceObserverSpy{};
    validList.attach(&validSpy);
    invalidList.attach(&invalidSpy);

    auto second = testLibrary.addTrack(makeSmartListSpec("second", 2023));
    source.insert(second, 1);

    REQUIRE(validSpy.events.size() == 1);
    CHECK(validSpy.events[0].kind == TrackSourceObserverSpy::EventKind::Inserted);
    CHECK(validSpy.events[0].id == second);
    CHECK(invalidSpy.events.empty());
    REQUIRE(validList.size() == 2);
    CHECK(validList.trackIdAt(1) == second);
    CHECK(invalidList.size() == 0);

    validList.detach(&validSpy);
    invalidList.detach(&invalidSpy);
  }

  TEST_CASE("SmartListEvaluator - setting a valid expression clears invalid expression error",
            "[runtime][unit][smart-list][expression]")
  {
    auto testLibrary = TestMusicLibrary{};
    auto first = testLibrary.addTrack(makeSmartListSpec("first", 2022));

    auto source = MutableTrackSource{};
    source.addInitial(first);

    auto engine = SmartListEvaluator{testLibrary.library()};
    auto validList = SmartListSource{source, testLibrary.library(), engine};
    auto invalidList = SmartListSource{source, testLibrary.library(), engine};
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
} // namespace ao::rt::test
