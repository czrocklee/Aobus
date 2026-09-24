// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/ViewServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSource.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("ViewService - workspace starts without live views", "[runtime][unit][view]")
  {
    auto env = ViewServiceFixture{};

    CHECK(env.workspace.snapshot().openViews.empty());
  }

  TEST_CASE("ViewService - createView assigns ids and lists live views", "[runtime][unit][view]")
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
      REQUIRE(views.size() == 1);
      CHECK(views[0] == result);
    }
  }

  TEST_CASE("ViewService - failed creation returns the source error without consuming view state",
            "[runtime][unit][view]")
  {
    auto env = ViewServiceFixture{};
    auto const failedRes = env.workspace.navigate({.target = ListId{kInvalidListId}});

    REQUIRE_FALSE(failedRes);
    CHECK(failedRes.error().code == Error::Code::InvalidInput);
    CHECK(env.workspace.snapshot().openViews.empty());

    auto const created = env.requireView();
    CHECK(created == ViewId{1});
  }

  TEST_CASE("ViewService - workspace close removes the view from its snapshot", "[runtime][unit][view]")
  {
    auto env = ViewServiceFixture{};
    auto const viewId = env.requireView();

    REQUIRE(env.workspace.closeView(viewId));

    auto const views = env.workspace.snapshot().openViews;
    CHECK(views.empty());
  }

  TEST_CASE("ViewService - closed views are absent from state and projection lookups", "[runtime][unit][view]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;
    auto const viewId = env.requireView();

    REQUIRE(env.workspace.closeView(viewId));

    CHECK_THROWS_AS(std::ignore = service.trackListState(viewId), std::out_of_range);

    // The checked lookup reports the same NotFound the other fallible methods use.
    auto const stateRes = service.findTrackListState(viewId);
    REQUIRE_FALSE(stateRes);
    CHECK(stateRes.error().code == Error::Code::NotFound);

    auto const projectionRes = service.findTrackListProjection(viewId);
    REQUIRE_FALSE(projectionRes);
    CHECK(projectionRes.error().code == Error::Code::NotFound);
  }

  TEST_CASE("ViewService - repeated close is a no-op for the same view", "[runtime][unit][view]")
  {
    auto env = ViewServiceFixture{};
    auto const viewId = env.requireView();

    REQUIRE(env.workspace.closeView(viewId));
    REQUIRE(env.workspace.closeView(viewId));

    CHECK(env.workspace.snapshot().openViews.empty());
  }

  TEST_CASE("ViewService - closing a view transitions its source from live to missing", "[runtime][unit][view]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;
    auto const viewId = env.requireView();

    auto const stateRes = service.listSourceState(viewId);
    REQUIRE(stateRes);
    CHECK(*stateRes == TrackSourceState::Live);

    REQUIRE(env.workspace.closeView(viewId));

    auto const missingRes = service.listSourceState(viewId);
    REQUIRE_FALSE(missingRes);
    CHECK(missingRes.error().code == Error::Code::NotFound);
  }

  TEST_CASE("ViewService - destroyed views reject launch-context capture", "[runtime][unit][view]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;
    auto const viewId = env.requireView();

    REQUIRE(env.workspace.closeView(viewId));

    auto const capturedRes = service.capturePlaybackLaunchSpec(viewId);
    REQUIRE_FALSE(capturedRes);
    CHECK(capturedRes.error().code == Error::Code::NotFound);
  }

  TEST_CASE("ViewService - close releases the owned projection", "[runtime][unit][view]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;
    auto const viewId = env.requireView();
    auto projectionWeakPtr = std::weak_ptr<TrackListProjection const>{};

    {
      auto const projectionRes = service.findTrackListProjection(viewId);
      REQUIRE(projectionRes);
      projectionWeakPtr = *projectionRes;
    }

    REQUIRE_FALSE(projectionWeakPtr.expired());

    REQUIRE(env.workspace.closeView(viewId));

    CHECK(projectionWeakPtr.expired());
  }

  TEST_CASE("ViewService - state lookups preserve initial and updated view contents", "[runtime][unit][view]")
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

    REQUIRE(service.setFilter(result, "$title ~ \"Needle\""));
    REQUIRE(service.setSelection(result, {TrackId{11}, TrackId{22}}));
    auto const direct = service.trackListState(result);
    auto const foundRes = service.findTrackListState(result);

    REQUIRE(foundRes);

    for (auto const* updated : {&direct, &*foundRes})
    {
      CHECK(updated->id == result);
      CHECK(updated->listId == kAllTracksListId);
      CHECK(updated->filterExpression == "$title ~ \"Needle\"");
      CHECK_FALSE(updated->optFilterError);
      CHECK(updated->selection == std::vector{TrackId{11}, TrackId{22}});
      CHECK(updated->sortBy == snap.sortBy);
      CHECK(updated->presentation == snap.presentation);
    }
  }

  TEST_CASE("ViewService - findTrackListProjection returns the owned projection", "[runtime][unit][view]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;

    auto const result = env.requireView();
    auto const projectionRes = service.findTrackListProjection(result);
    REQUIRE(projectionRes);
    auto const& projectionPtr = *projectionRes;
    REQUIRE(projectionPtr != nullptr);
    CHECK(projectionPtr->viewId() == result);
    CHECK(projectionPtr->size() == 0);
  }

  TEST_CASE("ViewService - explicit initial order overrides the default presentation order", "[runtime][unit][view]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;
    auto const order = std::vector{TrackSortTerm{.field = TrackSortField::Title, .ascending = false}};
    auto const result = env.requireView({.sortBy = order});

    auto const state = service.trackListState(result);
    CHECK(state.groupBy == TrackGroupKey::None);
    CHECK(state.sortBy == order);
    CHECK(state.presentation.id == kDefaultTrackPresentationId);
    auto const launchSpecRes = service.capturePlaybackLaunchSpec(result);
    REQUIRE(launchSpecRes);
    CHECK(launchSpecRes->order.sortBy == order);
  }

  TEST_CASE("ViewService - projection subscription replays initial reset", "[runtime][unit][view]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;

    auto const result = env.requireView();
    auto const projectionRes = service.findTrackListProjection(result);
    REQUIRE(projectionRes);
    auto const& projectionPtr = *projectionRes;
    REQUIRE(projectionPtr != nullptr);

    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    auto const sub =
      projectionPtr->subscribe([&](TrackListProjectionDeltaBatch const& batch) noexcept { batches.push_back(batch); });

    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 1);
    CHECK(std::holds_alternative<ProjectionReset>(batches.front().deltas.front()));
  }
} // namespace ao::rt::test
