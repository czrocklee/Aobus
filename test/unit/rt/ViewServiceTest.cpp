// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <runtime/ViewService.h>

#include <runtime/AllTracksSource.h>
#include <runtime/CorePrimitives.h>
#include <runtime/LibraryMutationService.h>
#include <runtime/ListSourceStore.h>

#include "TestUtils.h"

#include <filesystem>
#include <memory>

namespace ao::rt::test
{
  namespace
  {
    class MockControlExecutor final : public IControlExecutor
    {
    public:
      bool isCurrent() const noexcept override { return true; }
      void dispatch(std::move_only_function<void()> task) override { task(); }
    };

    struct TestEnv final
    {
      TestMusicLibrary library;
      MockControlExecutor executor;
      LibraryMutationService mutation;
      std::unique_ptr<ao::rt::ListSourceStore> store;

      TestEnv()
        : mutation{executor, library.library()}
        , store{std::make_unique<ao::rt::ListSourceStore>(library.library(), mutation)}
      {
      }

      ViewService makeService() { return ViewService{library.library(), *store}; }
    };
  }

  TEST_CASE("ViewService - listViews starts empty", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

    CHECK(service.listViews().empty());
  }

  TEST_CASE("ViewService - create view via direct API", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

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
    auto service = env.makeService();

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
      auto sub = service.onDestroyed([&](auto viewId) { received = viewId; });

      service.destroyView(viewId);
      CHECK(received == viewId);
    }
  }

  TEST_CASE("ViewService - trackListState access", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

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
    auto service = env.makeService();

    auto result = service.createView({}, true);
    auto projection = service.trackListProjection(result.viewId);
    REQUIRE(projection != nullptr);
    CHECK(projection->viewId() == result.viewId);
    CHECK(projection->size() == 0);
  }

  TEST_CASE("ViewService - projection subscription", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

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

  TEST_CASE("ViewService - createView with groupBy applies effective sort", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

    auto result = service.createView({.groupBy = TrackGroupKey::Artist}, true);
    auto snap = service.trackListState(result.viewId);

    CHECK(snap.groupBy == TrackGroupKey::Artist);

    std::vector<TrackSortField> const expected = {TrackSortField::Artist,
                                                  TrackSortField::Album,
                                                  TrackSortField::DiscNumber,
                                                  TrackSortField::TrackNumber,
                                                  TrackSortField::Title};
    REQUIRE(snap.sortBy.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
      CHECK(snap.sortBy[i].field == expected[i]);
      CHECK(snap.sortBy[i].ascending == true);
    }
  }

  TEST_CASE("ViewService - createView with Album groupBy", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

    auto result = service.createView({.groupBy = TrackGroupKey::Album}, true);
    auto snap = service.trackListState(result.viewId);

    CHECK(snap.groupBy == TrackGroupKey::Album);

    std::vector<TrackSortField> const expected = {TrackSortField::AlbumArtist,
                                                  TrackSortField::Album,
                                                  TrackSortField::DiscNumber,
                                                  TrackSortField::TrackNumber,
                                                  TrackSortField::Title};
    REQUIRE(snap.sortBy.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
      CHECK(snap.sortBy[i].field == expected[i]);
    }
  }

  TEST_CASE("ViewService - setGrouping updates state and projection", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

    auto result = service.createView({}, true);
    auto viewId = result.viewId;

    service.setGrouping(viewId, TrackGroupKey::Genre);
    auto snap = service.trackListState(viewId);

    CHECK(snap.groupBy == TrackGroupKey::Genre);

    std::vector<TrackSortField> const expected = {TrackSortField::Genre,
                                                  TrackSortField::Artist,
                                                  TrackSortField::Album,
                                                  TrackSortField::DiscNumber,
                                                  TrackSortField::TrackNumber,
                                                  TrackSortField::Title};
    REQUIRE(snap.sortBy.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
      CHECK(snap.sortBy[i].field == expected[i]);
    }
  }

  TEST_CASE("ViewService - setGrouping no-ops on same value", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

    auto result = service.createView({.groupBy = TrackGroupKey::Year}, true);
    auto snap = service.trackListState(result.viewId);
    auto const revBefore = snap.revision;

    service.setGrouping(result.viewId, TrackGroupKey::Year);
    auto snapAfter = service.trackListState(result.viewId);

    CHECK(snapAfter.revision == revBefore);
    CHECK(snapAfter.groupBy == TrackGroupKey::Year);
  }

  TEST_CASE("ViewService - setGrouping publishes ViewGroupingChanged", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

    auto result = service.createView({}, true);

    auto received = TrackGroupKey::None;
    auto sub = service.onGroupingChanged([&](auto const& ev) { received = ev.groupBy; });

    service.setGrouping(result.viewId, TrackGroupKey::Composer);
    CHECK(received == TrackGroupKey::Composer);
  }

  TEST_CASE("ViewService - setGrouping no-ops does not publish event", "[app][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

    auto result = service.createView({}, true);

    auto callCount = 0;
    auto sub = service.onGroupingChanged([&](auto const&) { ++callCount; });

    // First change should publish
    service.setGrouping(result.viewId, TrackGroupKey::Artist);
    CHECK(callCount == 1);

    // No-op should not publish again
    service.setGrouping(result.viewId, TrackGroupKey::Artist);
    CHECK(callCount == 1);

    // Different change should publish
    service.setGrouping(result.viewId, TrackGroupKey::Album);
    CHECK(callCount == 2);
  }
}
