// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/runtime/ViewServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
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
  TEST_CASE("ViewService - workspace starts without live views", "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};

    CHECK(env.workspace.snapshot().openViews.empty());
  }

  TEST_CASE("ViewService - createView assigns ids and lists live views", "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};

    SECTION("creating a track list view returns ViewId")
    {
      auto const result = env.requireView();
      CHECK(result != rt::kInvalidViewId);
    }

    SECTION("creating multiple views returns distinct ViewIds")
    {
      auto const r1 = env.requireView();
      auto const r2 = env.requireView();

      CHECK(r1 != r2);
    }

    SECTION("created view appears in the workspace snapshot")
    {
      auto const result = env.requireView();
      auto const views = env.workspace.snapshot().openViews;
      CHECK(views.size() == 1);
      CHECK(views[0] == result);
    }
  }

  TEST_CASE("ViewService - failed creation returns the source error without consuming view state",
            "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};
    auto const failed = env.workspace.navigate({.target = ListId{kInvalidListId}});

    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == Error::Code::InvalidInput);
    CHECK(env.workspace.snapshot().openViews.empty());

    auto const created = env.requireView();
    CHECK(created == ViewId{1});
  }

  TEST_CASE("ViewService - workspace close removes owned view state", "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;

    auto const result = env.requireView();
    auto const viewId = ViewId{result};

    SECTION("closing a view removes it from the workspace")
    {
      REQUIRE(env.workspace.closeView(viewId));
      auto const views = env.workspace.snapshot().openViews;
      CHECK(views.empty());
    }

    SECTION("close removes state and repeated close is a no-op")
    {
      REQUIRE(env.workspace.closeView(viewId));

      CHECK_THROWS_AS(std::ignore = service.trackListState(viewId), std::out_of_range);

      // The checked lookup reports the same NotFound the other fallible methods use.
      auto const found = service.findTrackListState(viewId);
      REQUIRE_FALSE(found);
      CHECK(found.error().code == Error::Code::NotFound);

      REQUIRE(env.workspace.closeView(viewId));
      CHECK(env.workspace.snapshot().openViews.empty());
    }

    SECTION("the checked lookup returns the same state as the precondition form")
    {
      auto const found = service.findTrackListState(viewId);
      REQUIRE(found);
      CHECK(found->id == service.trackListState(viewId).id);
      CHECK(found->listId == service.trackListState(viewId).listId);
    }

    SECTION("the checked projection lookup returns the owned projection")
    {
      auto const found = service.findTrackListProjection(viewId);
      REQUIRE(found);
      CHECK((*found)->viewId() == viewId);
    }

    SECTION("the checked projection lookup reports NotFound after destroy")
    {
      REQUIRE(env.workspace.closeView(viewId));

      auto const found = service.findTrackListProjection(viewId);
      REQUIRE_FALSE(found);
      CHECK(found.error().code == Error::Code::NotFound);
    }

    SECTION("destroyed views reject launch-context capture")
    {
      REQUIRE(env.workspace.closeView(viewId));

      auto const captured = service.capturePlaybackLaunchSpec(viewId);
      REQUIRE_FALSE(captured);
      CHECK(captured.error().code == Error::Code::NotFound);
    }

    SECTION("close releases the owned projection")
    {
      auto projectionWeakPtr = std::weak_ptr<TrackListProjection>{};

      {
        auto const projectionResult = service.findTrackListProjection(viewId);
        REQUIRE(projectionResult);
        projectionWeakPtr = *projectionResult;
      }

      REQUIRE_FALSE(projectionWeakPtr.expired());

      REQUIRE(env.workspace.closeView(viewId));

      CHECK(projectionWeakPtr.expired());
    }
  }

  TEST_CASE("ViewService - trackListState returns created view snapshot", "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;

    auto const result = env.requireView({.filterExpression = "$year > 2000"});
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

  TEST_CASE("ViewService - findTrackListProjection returns the owned projection", "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;

    auto const result = env.requireView();
    auto const projectionResult = service.findTrackListProjection(result);
    REQUIRE(projectionResult);
    auto const& projectionPtr = *projectionResult;
    REQUIRE(projectionPtr != nullptr);
    CHECK(projectionPtr->viewId() == result);
    CHECK(projectionPtr->size() == 0);
  }

  TEST_CASE("ViewService - explicit initial order overrides the default presentation order",
            "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;
    auto const order = std::vector{TrackSortTerm{.field = TrackSortField::Title, .ascending = false}};
    auto const result = env.requireView({.sortBy = order});

    auto const state = service.trackListState(result);
    CHECK(state.groupBy == TrackGroupKey::None);
    CHECK(state.sortBy == order);
    CHECK(state.presentation.id == kDefaultTrackPresentationId);
    auto const launchSpec = service.capturePlaybackLaunchSpec(result);
    REQUIRE(launchSpec);
    CHECK(launchSpec->order.sortBy == order);
  }

  TEST_CASE("ViewService - projection subscription replays initial reset", "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;

    auto const result = env.requireView();
    auto const projectionResult = service.findTrackListProjection(result);
    REQUIRE(projectionResult);
    auto const& projectionPtr = *projectionResult;
    REQUIRE(projectionPtr != nullptr);

    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    auto const sub =
      projectionPtr->subscribe([&](TrackListProjectionDeltaBatch const& batch) noexcept { batches.push_back(batch); });

    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 1);
    CHECK(std::holds_alternative<ProjectionReset>(batches.front().deltas.front()));
  }
} // namespace ao::rt::test
