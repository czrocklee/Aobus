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
      CHECK(service.selectionDuration(result) == std::chrono::milliseconds{0});
    }

    SECTION("the selection's durations are summed")
    {
      REQUIRE(service.setSelection(result, {trackA, trackB}));
      CHECK(service.selectionDuration(result) == std::chrono::seconds{300});
    }

    SECTION("ids missing from the library are skipped")
    {
      REQUIRE(service.setSelection(result, {trackA, TrackId{9999}}));
      CHECK(service.selectionDuration(result) == std::chrono::seconds{200});
    }

    SECTION("an unknown view has zero duration")
    {
      CHECK(service.selectionDuration(ViewId{999}) == std::chrono::milliseconds{0});
    }
  }

  TEST_CASE("ViewService - selection mutation reports missing views", "[runtime][unit][view][selection]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();
    auto const missing = service.setSelection(ViewId{999}, {});
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == Error::Code::NotFound);

    auto const view = env.requireView(service);
    REQUIRE(service.destroyView(view));
    auto const removed = service.setSelection(view, {});
    REQUIRE_FALSE(removed);
    CHECK(removed.error().code == Error::Code::NotFound);
  }
} // namespace ao::rt::test
