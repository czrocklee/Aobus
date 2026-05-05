// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <runtime/CommandBus.h>
#include <runtime/CommandTypes.h>
#include <runtime/EventBus.h>
#include <runtime/EventTypes.h>
#include <runtime/ViewRegistry.h>

#include <ao/library/MusicLibrary.h>
#include <ao/model/AllTrackIdsList.h>
#include <ao/model/SmartListEngine.h>

#include <filesystem>
#include <memory>

namespace ao::app::test
{
  namespace
  {
    struct TestEnv final
    {
      std::filesystem::path dir;
      std::unique_ptr<ao::library::MusicLibrary> library;
      std::unique_ptr<ao::model::AllTrackIdsList> allTracks;
      std::unique_ptr<ao::model::SmartListEngine> engine;

      TestEnv()
      {
        auto tmpl = std::filesystem::temp_directory_path() / "ao_vr_test_XXXXXX";
        auto raw = tmpl.string();
        auto* res = ::mkdtemp(raw.data());
        dir = res;

        library = std::make_unique<ao::library::MusicLibrary>(dir);
        allTracks = std::make_unique<ao::model::AllTrackIdsList>(library->tracks());
        engine = std::make_unique<ao::model::SmartListEngine>(*library);
      }

      ~TestEnv()
      {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
      }

      TestEnv(TestEnv const&) = delete;
      TestEnv& operator=(TestEnv const&) = delete;
      TestEnv(TestEnv&&) = delete;
      TestEnv& operator=(TestEnv&&) = delete;

      auto makeRegistry(EventBus& events) const -> ViewRegistry
      {
        return ViewRegistry{*library, *engine, *allTracks, events};
      }
    };
  }

  TEST_CASE("ViewRegistry - listViews starts empty", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto events = EventBus{};
    auto registry = env.makeRegistry(events);

    CHECK(registry.listViews().empty());
  }

  TEST_CASE("ViewRegistry - create view via command", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto events = EventBus{};
    auto registry = env.makeRegistry(events);
    auto bus = CommandBus{};
    registry.registerCommandHandlers(bus);

    SECTION("creating a track list view returns ViewId")
    {
      auto result = bus.execute(CreateTrackListView{
        .initial = {.listId = ao::ListId{}},
        .attached = true,
      });
      REQUIRE(result.has_value());
      CHECK(result->viewId != ViewId{});
    }

    SECTION("creating multiple views returns distinct ViewIds")
    {
      auto r1 = bus.execute(CreateTrackListView{.attached = true});
      auto r2 = bus.execute(CreateTrackListView{.attached = true});

      REQUIRE(r1.has_value());
      REQUIRE(r2.has_value());
      CHECK(r1->viewId != r2->viewId);
    }

    SECTION("created view appears in listViews")
    {
      auto result = bus.execute(CreateTrackListView{.attached = true});
      REQUIRE(result.has_value());

      auto views = registry.listViews();
      CHECK(views.size() == 1);
      CHECK(views[0].id == result->viewId);
      CHECK(views[0].lifecycle == ViewLifecycleState::Attached);
    }

    SECTION("detached view creates in Detached state")
    {
      auto result = bus.execute(CreateTrackListView{
        .initial = {},
        .attached = false,
      });
      REQUIRE(result.has_value());

      auto views = registry.listViews();
      REQUIRE(views.size() == 1);
      CHECK(views[0].lifecycle == ViewLifecycleState::Detached);
    }
  }

  TEST_CASE("ViewRegistry - destroy view", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto events = EventBus{};
    auto registry = env.makeRegistry(events);
    auto bus = CommandBus{};
    registry.registerCommandHandlers(bus);

    auto result = bus.execute(CreateTrackListView{.attached = true});
    REQUIRE(result.has_value());
    auto viewId = result->viewId;

    SECTION("destroying a view removes it from listViews")
    {
      bus.execute(DestroyView{.viewId = viewId});
      auto views = registry.listViews();
      CHECK(views.empty());
    }

    SECTION("destroying non-existent view is safe")
    {
      REQUIRE_NOTHROW(bus.execute(DestroyView{.viewId = ViewId{99999}}));
    }

    SECTION("state after destroy shows Destroyed lifecycle")
    {
      auto& state = registry.trackListState(viewId);
      bus.execute(DestroyView{.viewId = viewId});

      auto snap = state.snapshot();
      CHECK(snap.lifecycle == ViewLifecycleState::Destroyed);
    }

    SECTION("destroy publishes ViewDestroyed event")
    {
      auto received = ViewId{};
      auto sub = events.subscribe<ViewDestroyed>([&](ViewDestroyed const& ev) { received = ev.viewId; });

      bus.execute(DestroyView{.viewId = viewId});
      CHECK(received == viewId);
    }
  }

  TEST_CASE("ViewRegistry - trackListState access", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto events = EventBus{};
    auto registry = env.makeRegistry(events);
    auto bus = CommandBus{};
    registry.registerCommandHandlers(bus);

    auto result = bus.execute(CreateTrackListView{
      .initial = {.filterExpression = "$year > 2000"},
      .attached = true,
    });
    REQUIRE(result.has_value());

    auto& state = registry.trackListState(result->viewId);
    auto snap = state.snapshot();

    CHECK(snap.id == result->viewId);
    CHECK(snap.filterExpression == "$year > 2000");
    CHECK(snap.lifecycle == ViewLifecycleState::Attached);
    CHECK(snap.groupBy == TrackGroupKey::None);
    CHECK(snap.sortBy.empty());
  }

  TEST_CASE("ViewRegistry - trackListProjection access", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto events = EventBus{};
    auto registry = env.makeRegistry(events);
    auto bus = CommandBus{};
    registry.registerCommandHandlers(bus);

    auto result = bus.execute(CreateTrackListView{.attached = true});
    REQUIRE(result.has_value());

    auto projection = registry.trackListProjection(result->viewId);
    REQUIRE(projection != nullptr);
    CHECK(projection->viewId() == result->viewId);
    CHECK(projection->size() == 0);
  }

  TEST_CASE("ViewRegistry - projection subscription", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto events = EventBus{};
    auto registry = env.makeRegistry(events);
    auto bus = CommandBus{};
    registry.registerCommandHandlers(bus);

    auto result = bus.execute(CreateTrackListView{.attached = true});
    REQUIRE(result.has_value());

    auto projection = registry.trackListProjection(result->viewId);
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
