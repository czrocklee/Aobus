// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <runtime/EventBus.h>
#include <runtime/EventTypes.h>
#include <runtime/ViewService.h>

#include <runtime/AllTracksSource.h>
#include <runtime/ListSourceStore.h>

#include "TestUtils.h"

#include <filesystem>
#include <memory>

namespace ao::app::test
{
  namespace
  {
    struct TestEnv final
    {
      TestMusicLibrary library;
      EventBus events;
      std::unique_ptr<ao::app::ListSourceStore> store;

      TestEnv() { store = std::make_unique<ao::app::ListSourceStore>(library.library(), events); }

      auto makeService(EventBus& events) -> ViewService { return ViewService{library.library(), *store, events}; }
    };
  }

  TEST_CASE("ViewService - listViews starts empty", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto events = EventBus{};
    auto service = env.makeService(events);

    CHECK(service.listViews().empty());
  }

  TEST_CASE("ViewService - create view via direct API", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto events = EventBus{};
    auto service = env.makeService(events);

    SECTION("creating a track list view returns ViewId")
    {
      auto result = service.createView({.listId = ao::ListId{}}, true);
      CHECK(result.viewId != ViewId{});
    }

    SECTION("creating multiple views returns distinct ViewIds")
    {
      auto r1 = service.createView({}, true);
      auto r2 = service.createView({}, true);

      CHECK(r1.viewId != r2.viewId);
    }

    SECTION("created view appears in listViews")
    {
      auto result = service.createView({}, true);
      auto views = service.listViews();
      CHECK(views.size() == 1);
      CHECK(views[0].id == result.viewId);
      CHECK(views[0].lifecycle == ViewLifecycleState::Attached);
    }

    SECTION("detached view creates in Detached state")
    {
      service.createView({}, false);
      auto views = service.listViews();
      REQUIRE(views.size() == 1);
      CHECK(views[0].lifecycle == ViewLifecycleState::Detached);
    }
  }

  TEST_CASE("ViewService - destroy view", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto events = EventBus{};
    auto service = env.makeService(events);

    auto result = service.createView({}, true);
    auto viewId = result.viewId;

    SECTION("destroying a view removes it from listViews")
    {
      service.destroyView(viewId);
      auto views = service.listViews();
      CHECK(views.empty());
    }

    SECTION("destroying non-existent view is safe")
    {
      REQUIRE_NOTHROW(service.destroyView(ViewId{99999}));
    }

    SECTION("state after destroy shows Destroyed lifecycle")
    {
      service.destroyView(viewId);

      auto snap = service.trackListState(viewId);
      CHECK(snap.lifecycle == ViewLifecycleState::Destroyed);
    }

    SECTION("destroy publishes ViewDestroyed event")
    {
      auto received = ViewId{};
      auto sub = events.subscribe<ViewDestroyed>([&](ViewDestroyed const& ev) { received = ev.viewId; });

      service.destroyView(viewId);
      CHECK(received == viewId);
    }
  }

  TEST_CASE("ViewService - trackListState access", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto events = EventBus{};
    auto service = env.makeService(events);

    auto result = service.createView({.filterExpression = "$year > 2000"}, true);
    auto snap = service.trackListState(result.viewId);

    CHECK(snap.id == result.viewId);
    CHECK(snap.filterExpression == "$year > 2000");
    CHECK(snap.lifecycle == ViewLifecycleState::Attached);
    CHECK(snap.groupBy == TrackGroupKey::None);
    CHECK(snap.sortBy.empty());
  }

  TEST_CASE("ViewService - trackListProjection access", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto events = EventBus{};
    auto service = env.makeService(events);

    auto result = service.createView({}, true);
    auto projection = service.trackListProjection(result.viewId);
    REQUIRE(projection != nullptr);
    CHECK(projection->viewId() == result.viewId);
    CHECK(projection->size() == 0);
  }

  TEST_CASE("ViewService - projection subscription", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto events = EventBus{};
    auto service = env.makeService(events);

    auto result = service.createView({}, true);
    auto projection = service.trackListProjection(result.viewId);
    REQUIRE(projection != nullptr);

    auto received = false;
    auto sub = projection->subscribe(
      [&](TrackListProjectionDeltaBatch const& batch)
      {
        CHECK(std::holds_alternative<ProjectionReset>(batch.deltas[0]));
        received = true;
      });

    CHECK(received);
  }
}
