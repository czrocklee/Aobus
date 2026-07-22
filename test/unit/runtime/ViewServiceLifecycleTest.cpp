// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/runtime/ViewServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("ViewService - listViews starts empty", "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();

    CHECK(service.listViews().empty());
  }

  TEST_CASE("ViewService - createView assigns ids and lists live views", "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();

    SECTION("creating a track list view returns ViewId")
    {
      auto const result = env.requireView(service);
      CHECK(result != rt::kInvalidViewId);
    }

    SECTION("creating multiple views returns distinct ViewIds")
    {
      auto const r1 = env.requireView(service);
      auto const r2 = env.requireView(service);

      CHECK(r1 != r2);
    }

    SECTION("created view appears in listViews")
    {
      auto const result = env.requireView(service);
      auto const views = service.listViews();
      CHECK(views.size() == 1);
      CHECK(views[0] == result);
    }
  }

  TEST_CASE("ViewService - failed creation returns the source error without consuming view state",
            "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();

    auto const failed = service.createView({.listId = kInvalidListId});

    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == Error::Code::InvalidInput);
    CHECK(service.listViews().empty());

    auto const created = env.requireView(service);
    CHECK(created == ViewId{1});
  }

  TEST_CASE("ViewService - destroyView removes state and publishes destruction", "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();

    auto const result = env.requireView(service);
    auto const viewId = ViewId{result};

    SECTION("destroying a view removes it from listViews")
    {
      REQUIRE(service.destroyView(viewId));
      auto const views = service.listViews();
      CHECK(views.empty());
    }

    SECTION("destroying non-existent view reports not found")
    {
      auto const missing = service.destroyView(ViewId{99999});
      REQUIRE_FALSE(missing);
      CHECK(missing.error().code == Error::Code::NotFound);
    }

    SECTION("destroy removes state and repeated destroy reports not found")
    {
      REQUIRE(service.destroyView(viewId));

      CHECK_THROWS_AS(std::ignore = service.trackListState(viewId), std::out_of_range);
      auto const repeated = service.destroyView(viewId);
      REQUIRE_FALSE(repeated);
      CHECK(repeated.error().code == Error::Code::NotFound);
    }

    SECTION("destroyed views reject launch-context capture")
    {
      REQUIRE(service.destroyView(viewId));

      auto const captured = service.capturePlaybackLaunchSpec(viewId);
      REQUIRE_FALSE(captured);
      CHECK(captured.error().code == Error::Code::NotFound);
    }

    SECTION("destroy publishes ViewDestroyed event")
    {
      auto received = kInvalidViewId;
      auto const sub = service.onDestroyed([&](auto viewId) { received = viewId; });

      REQUIRE(service.destroyView(viewId));
      CHECK(received == viewId);
    }

    SECTION("destroy releases the owned projection")
    {
      auto const projectionWeakPtr = std::weak_ptr<TrackListProjection>{service.trackListProjection(viewId)};
      REQUIRE_FALSE(projectionWeakPtr.expired());

      REQUIRE(service.destroyView(viewId));

      CHECK(projectionWeakPtr.expired());
      CHECK_THROWS_AS(std::ignore = service.trackListProjection(viewId), std::out_of_range);
    }
  }

  TEST_CASE("ViewService - trackListState returns created view snapshot", "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();

    auto const result = env.requireView(service, {.filterExpression = "$year > 2000"});
    auto const snap = service.trackListState(result);

    CHECK(snap.id == result);
    CHECK(snap.listId == kAllTracksListId);
    CHECK(snap.filterExpression == "$year > 2000");
    CHECK(snap.groupBy == TrackGroupKey::None);

    auto const expectedNone = std::vector{TrackSortField::AlbumArtist,
                                          TrackSortField::Album,
                                          TrackSortField::DiscNumber,
                                          TrackSortField::TrackNumber,
                                          TrackSortField::Title};
    REQUIRE(snap.sortBy.size() == expectedNone.size());

    for (std::size_t i = 0; i < expectedNone.size(); ++i)
    {
      CHECK(snap.sortBy[i].field == expectedNone[i]);
      CHECK(snap.sortBy[i].ascending == true);
    }
  }

  TEST_CASE("ViewService - trackListProjection returns the owned projection", "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();

    auto const result = env.requireView(service);
    auto const projectionPtr = service.trackListProjection(result);
    REQUIRE(projectionPtr != nullptr);
    CHECK(projectionPtr->viewId() == result);
    CHECK(projectionPtr->size() == 0);
  }

  TEST_CASE("ViewService - explicit initial order overrides the default presentation order",
            "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();
    auto const order = std::vector{TrackSortTerm{.field = TrackSortField::Title, .ascending = false}};
    auto const result = env.requireView(service, {.sortBy = order});

    auto const state = service.trackListState(result);
    CHECK(state.groupBy == TrackGroupKey::None);
    CHECK(state.sortBy == order);
    CHECK(state.presentation.id.empty());
    auto const launchSpec = service.capturePlaybackLaunchSpec(result);
    REQUIRE(launchSpec);
    CHECK(launchSpec->order.sortBy == order);
  }

  TEST_CASE("ViewService - projection subscription replays initial reset", "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();

    auto const result = env.requireView(service);
    auto const projectionPtr = service.trackListProjection(result);
    REQUIRE(projectionPtr != nullptr);

    bool received = false;
    auto const sub = projectionPtr->subscribe(
      [&](TrackListProjectionDeltaBatch const& batch)
      {
        CHECK(std::holds_alternative<ProjectionReset>(batch.deltas[0]));
        received = true;
      });

    CHECK(received);
  }
} // namespace ao::rt::test
