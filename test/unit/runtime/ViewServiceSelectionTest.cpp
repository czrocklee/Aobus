// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/ViewServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/ViewIds.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::rt::test
{
  TEST_CASE("ViewService - selectionDuration sums selected track durations", "[runtime][unit][view][selection]")
  {
    auto env = ViewServiceFixture{};
    // Seed exact stored durations; authoring fixtures derive them from their audio file.
    auto const trackA = library::test::addTrackWithUniqueFixtureUri(
      env.libraryFixture.library(),
      library::test::TrackSpec{.title = "A", .duration = std::chrono::milliseconds{1234}});
    auto const trackB = library::test::addTrackWithUniqueFixtureUri(
      env.libraryFixture.library(),
      library::test::TrackSpec{.title = "B", .duration = std::chrono::milliseconds{5678}});

    auto& service = env.service;
    auto const result = env.requireView();

    SECTION("an empty selection has zero duration")
    {
      CHECK(service.selectionDuration(result) == std::chrono::milliseconds{0});
    }

    SECTION("the selection's durations are summed")
    {
      REQUIRE(service.setSelection(result, {trackA, trackB}));
      CHECK(service.selectionDuration(result) == std::chrono::milliseconds{6912});
    }

    SECTION("ids missing from the library are skipped")
    {
      REQUIRE(service.setSelection(result, {trackA, TrackId{9999}}));
      CHECK(service.selectionDuration(result) == std::chrono::milliseconds{1234});
    }

    SECTION("an unknown view has zero duration")
    {
      CHECK(service.selectionDuration(ViewId{999}) == std::chrono::milliseconds{0});
    }
  }

  TEST_CASE("ViewService - selection mutation reports missing views", "[runtime][unit][view][selection]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;
    auto const missingRes = service.setSelection(ViewId{999}, {});
    REQUIRE_FALSE(missingRes);
    CHECK(missingRes.error().code == Error::Code::NotFound);

    auto const view = env.requireView();
    REQUIRE(env.workspace.closeView(view));
    auto const removedRes = service.setSelection(view, {});
    REQUIRE_FALSE(removedRes);
    CHECK(removedRes.error().code == Error::Code::NotFound);
  }
} // namespace ao::rt::test
