// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/ViewServiceTestSupport.h"
#include <ao/rt/ViewIds.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::rt::test
{
  TEST_CASE("ViewService - selectionDuration sums selected track durations", "[runtime][unit][view][selection]")
  {
    auto env = ViewServiceFixture{};
    auto const trackA =
      env.libraryFixture.addTrack(library::test::TrackSpec{.title = "A", .duration = std::chrono::seconds{200}});
    auto const trackB =
      env.libraryFixture.addTrack(library::test::TrackSpec{.title = "B", .duration = std::chrono::seconds{100}});

    auto service = env.makeService();
    auto const result = env.requireView(service);

    SECTION("an empty selection has zero duration")
    {
      CHECK(service.selectionDuration(result.viewId) == std::chrono::milliseconds{0});
    }

    SECTION("the selection's durations are summed")
    {
      service.setSelection(result.viewId, {trackA, trackB});
      CHECK(service.selectionDuration(result.viewId) == std::chrono::seconds{300});
    }

    SECTION("ids missing from the library are skipped")
    {
      service.setSelection(result.viewId, {trackA, TrackId{9999}});
      CHECK(service.selectionDuration(result.viewId) == std::chrono::seconds{200});
    }

    SECTION("an unknown view has zero duration")
    {
      CHECK(service.selectionDuration(ViewId{999}) == std::chrono::milliseconds{0});
    }
  }
} // namespace ao::rt::test
