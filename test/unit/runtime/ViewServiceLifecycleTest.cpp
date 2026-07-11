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

  TEST_CASE("ViewService - createView assigns ids and lifecycle state", "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();

    SECTION("creating a track list view returns ViewId")
    {
      auto const result = env.requireView(service);
      CHECK(result.viewId != rt::kInvalidViewId);
    }

    SECTION("creating multiple views returns distinct ViewIds")
    {
      auto const r1 = env.requireView(service);
      auto const r2 = env.requireView(service);

      CHECK(r1.viewId != r2.viewId);
    }

    SECTION("created view appears in listViews")
    {
      auto const result = env.requireView(service);
      auto const views = service.listViews();
      CHECK(views.size() == 1);
      CHECK(views[0].id == result.viewId);
      CHECK(views[0].lifecycle == ViewLifecycleState::Attached);
    }

    SECTION("detached view creates in Detached state")
    {
      std::ignore = env.requireView(service, {}, false);
      auto const views = service.listViews();
      REQUIRE(views.size() == 1);
      CHECK(views[0].lifecycle == ViewLifecycleState::Detached);
    }
  }

  TEST_CASE("ViewService - failed creation returns the source error without consuming view state",
            "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();

    auto const failed = service.createView({.listId = kInvalidListId}, true);

    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == Error::Code::InvalidInput);
    CHECK(service.listViews().empty());

    auto const created = env.requireView(service);
    CHECK(created.viewId == ViewId{1});
  }

  TEST_CASE("ViewService - destroyView removes state and publishes destruction", "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();

    auto const result = env.requireView(service);
    auto const viewId = ViewId{result.viewId};

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

    SECTION("state after destroy shows Destroyed lifecycle")
    {
      REQUIRE(service.destroyView(viewId));

      auto const snap = service.trackListState(viewId);
      CHECK(snap.lifecycle == ViewLifecycleState::Destroyed);
      auto const repeated = service.destroyView(viewId);
      REQUIRE_FALSE(repeated);
      CHECK(repeated.error().code == Error::Code::InvalidState);
    }

    SECTION("destroyed views reject launch-context capture")
    {
      REQUIRE(service.destroyView(viewId));

      auto const captured = service.capturePlaybackLaunchContext(viewId);
      REQUIRE_FALSE(captured);
      CHECK(captured.error().code == Error::Code::InvalidState);
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
      auto projectionPtr = service.trackListProjection(viewId);
      REQUIRE(projectionPtr != nullptr);

      REQUIRE(service.destroyView(viewId));

      CHECK(service.trackListProjection(viewId) == nullptr);
      CHECK(projectionPtr->viewId() == viewId);
    }
  }

  TEST_CASE("ViewService - trackListState returns created view snapshot", "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();

    auto const result = env.requireView(service, {.filterExpression = "$year > 2000"});
    auto const snap = service.trackListState(result.viewId);

    CHECK(snap.id == result.viewId);
    CHECK(snap.listId == kAllTracksListId);
    CHECK(snap.filterExpression == "$year > 2000");
    CHECK(snap.lifecycle == ViewLifecycleState::Attached);
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
    auto const projectionPtr = service.trackListProjection(result.viewId);
    REQUIRE(projectionPtr != nullptr);
    CHECK(projectionPtr->viewId() == result.viewId);
    CHECK(projectionPtr->size() == 0);
  }

  TEST_CASE("ViewService - explicit initial order overrides the default presentation order",
            "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();
    auto const order = std::vector{TrackSortTerm{.field = TrackSortField::Title, .ascending = false}};
    auto const result = env.requireView(service, {.sortBy = order});

    auto const state = service.trackListState(result.viewId);
    CHECK(state.groupBy == TrackGroupKey::None);
    CHECK(state.sortBy == order);
    CHECK(state.presentation.id.empty());
    auto const context = service.capturePlaybackLaunchContext(result.viewId);
    REQUIRE(context);
    CHECK(context->order.sortBy == order);
  }

  TEST_CASE("ViewService - projection subscription replays initial reset", "[runtime][unit][view][lifecycle]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();

    auto const result = env.requireView(service);
    auto const projectionPtr = service.trackListProjection(result.viewId);
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
