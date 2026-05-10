// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <memory>

#include <runtime/AppSession.h>
#include <runtime/CorePrimitives.h>
#include <runtime/EventBus.h>
#include <runtime/EventTypes.h>
#include <runtime/ISessionPersistence.h>
#include <runtime/PlaybackService.h>
#include <runtime/StateTypes.h>
#include <runtime/ViewService.h>
#include <runtime/WorkspaceService.h>

#include "TestUtils.h"

namespace ao::app::test
{
  class MockExecutor final : public IControlExecutor
  {
  public:
    bool isCurrent() const noexcept override { return true; }
    void dispatch(std::move_only_function<void()> task) override { task(); }
  };

  class MockPersistence final : public ISessionPersistence
  {
  public:
    std::optional<SessionSnapshot> loadSnapshot() override { return snapshot; }
    void saveSnapshot(SessionSnapshot const& s) override { snapshot = s; }

    std::optional<SessionSnapshot> snapshot;
  };

  TEST_CASE("Headless Shell - Navigation and Layout Management", "[app][runtime][headless]")
  {
    auto tempDir = TempDir{};
    auto executor = std::make_shared<MockExecutor>();
    auto persistence = std::make_shared<MockPersistence>();

    AppSession session(
      AppSessionDependencies{.executor = executor, .libraryRoot = tempDir.path(), .persistence = persistence});

    SECTION("Initial layout is empty")
    {
      auto const layout = session.workspace().layoutState();
      CHECK(layout.openViews.empty());
      CHECK(layout.activeViewId == ViewId{});
    }

    SECTION("Navigate to list ID creates a view and marks it active")
    {
      auto const listId = ao::ListId{42};
      session.workspace().navigateTo(listId);

      auto const layout = session.workspace().layoutState();
      REQUIRE(layout.openViews.size() == 1);
      CHECK(layout.activeViewId == layout.openViews.front());

      auto const viewId = layout.activeViewId;
      auto const viewState = session.views().trackListState(viewId);
      CHECK(viewState.listId == listId);
    }

    SECTION("Closing a view updates the layout")
    {
      session.workspace().navigateTo(ao::ListId{1});
      session.workspace().navigateTo(ao::ListId{2});

      auto layout1 = session.workspace().layoutState();
      REQUIRE(layout1.openViews.size() == 2);
      auto const viewToClose = layout1.openViews.front();
      auto const remainingView = layout1.openViews.back();

      session.workspace().closeView(viewToClose);

      auto const layout2 = session.workspace().layoutState();
      CHECK(layout2.openViews.size() == 1);
      CHECK(layout2.openViews.front() == remainingView);
      CHECK(layout2.activeViewId == remainingView);
    }

    SECTION("Session persistence works across instances")
    {
      // Setup state in first session
      session.workspace().navigateTo(ao::ListId{10});
      session.workspace().navigateTo(ao::ListId{20});
      session.workspace().saveSession();

      REQUIRE(persistence->snapshot.has_value());
      CHECK(persistence->snapshot->openViews.size() == 2);

      // Create new session with same persistence
      AppSession session2(
        AppSessionDependencies{.executor = executor, .libraryRoot = tempDir.path(), .persistence = persistence});

      session2.workspace().restoreSession();

      auto const layout = session2.workspace().layoutState();
      CHECK(layout.openViews.size() == 2);
      CHECK(layout.activeViewId != ViewId{});
    }

    SECTION("Session persistence preserves groupBy across instances")
    {
      // Setup grouped view in first session
      session.workspace().navigateTo(ao::ListId{10});
      auto const viewId = session.workspace().layoutState().activeViewId;
      session.views().setGrouping(viewId, TrackGroupKey::Artist);

      auto const savedState = session.views().trackListState(viewId);
      CHECK(savedState.groupBy == TrackGroupKey::Artist);
      CHECK_FALSE(savedState.sortBy.empty());

      session.workspace().saveSession();

      REQUIRE(persistence->snapshot.has_value());
      REQUIRE(persistence->snapshot->openViews.size() == 1);
      CHECK(persistence->snapshot->openViews[0].groupBy == TrackGroupKey::Artist);

      // Restore in new session
      AppSession session2(
        AppSessionDependencies{.executor = executor, .libraryRoot = tempDir.path(), .persistence = persistence});

      session2.workspace().restoreSession();

      auto const layout2 = session2.workspace().layoutState();
      REQUIRE(layout2.openViews.size() == 1);
      auto const restoredState = session2.views().trackListState(layout2.openViews[0]);
      CHECK(restoredState.groupBy == TrackGroupKey::Artist);
      CHECK_FALSE(restoredState.sortBy.empty());
    }

    SECTION("Session persistence preserves groupBy=None")
    {
      // Ungrouped view
      session.workspace().navigateTo(ao::ListId{10});
      session.workspace().saveSession();

      AppSession session2(
        AppSessionDependencies{.executor = executor, .libraryRoot = tempDir.path(), .persistence = persistence});

      session2.workspace().restoreSession();

      auto const layout2 = session2.workspace().layoutState();
      REQUIRE(layout2.openViews.size() == 1);
      auto const restoredState = session2.views().trackListState(layout2.openViews[0]);
      CHECK(restoredState.groupBy == TrackGroupKey::None);
    }
  }
}