// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include "test/unit/lmdb/TestUtils.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <functional>
#include <memory>

namespace ao::rt::test
{
  using namespace ao::lmdb::test;
  using ao::rt::IControlExecutor;

  class MockExecutor final : public IControlExecutor
  {
  public:
    bool isCurrent() const noexcept override { return true; }
    void dispatch(std::move_only_function<void()> task) override { task(); }
    void defer(std::move_only_function<void()> task) override { task(); }
  };

  TEST_CASE("Headless Shell - Navigation and Layout Management", "[app][runtime][headless]")
  {
    auto tempDir = TempDir{};
    auto workspaceConfigStore = std::make_shared<ConfigStore>(std::filesystem::path{tempDir.path()} / "workspace.yaml");

    auto runtime = AppRuntime{
      AppRuntimeDependencies{.executor = std::make_unique<MockExecutor>(),
                                 .musicRoot = tempDir.path(),
                                 .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
                                 .workspaceConfigStore = workspaceConfigStore}};

    SECTION("Initial layout is empty")
    {
      auto const layout = runtime.workspace().layoutState();
      CHECK(layout.openViews.empty());
      CHECK(layout.activeViewId == kInvalidViewId);
    }

    SECTION("Navigate to list ID creates a view and marks it active")
    {
      auto const listId = ListId{42};
      runtime.workspace().navigateTo(listId);

      auto const layout = runtime.workspace().layoutState();
      REQUIRE(layout.openViews.size() == 1);
      CHECK(layout.activeViewId == layout.openViews.front());

      auto const viewId = layout.activeViewId;
      auto const viewState = runtime.views().trackListState(viewId);
      CHECK(viewState.listId == listId);
    }

    SECTION("Closing a view updates the layout")
    {
      runtime.workspace().navigateTo(ListId{1});
      runtime.workspace().navigateTo(ListId{2});

      auto layout1 = runtime.workspace().layoutState();
      REQUIRE(layout1.openViews.size() == 2);
      auto const viewToClose = layout1.openViews.front();
      auto const remainingView = layout1.openViews.back();

      runtime.workspace().closeView(viewToClose);

      auto const layout2 = runtime.workspace().layoutState();
      CHECK(layout2.openViews.size() == 1);
      CHECK(layout2.openViews.front() == remainingView);
      CHECK(layout2.activeViewId == remainingView);
    }

    SECTION("Session persistence works across instances")
    {
      // Setup state in first runtime
      runtime.workspace().navigateTo(ListId{10});
      runtime.workspace().navigateTo(ListId{20});
      runtime.workspace().saveSession(runtime.configStore());

      auto loaded = SessionState{};
      workspaceConfigStore->load("workspace", loaded);
      CHECK(loaded.openViews.size() == 2);

      // Create new runtime with same persistence
      auto session2 = AppRuntime{
        AppRuntimeDependencies{.executor = std::make_unique<MockExecutor>(),
                                   .musicRoot = tempDir.path(),
                                   .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
                                   .workspaceConfigStore = workspaceConfigStore}};

      session2.workspace().restoreSession(session2.configStore());

      auto const layout = session2.workspace().layoutState();
      CHECK(layout.openViews.size() == 2);
      CHECK(layout.activeViewId != kInvalidViewId);
    }

    SECTION("Session persistence preserves groupBy across instances")
    {
      // Setup grouped view in first runtime
      runtime.workspace().navigateTo(ListId{10});
      auto const viewId = runtime.workspace().layoutState().activeViewId;
      runtime.views().setGrouping(viewId, TrackGroupKey::Artist);

      auto const savedState = runtime.views().trackListState(viewId);
      CHECK(savedState.groupBy == TrackGroupKey::Artist);
      CHECK_FALSE(savedState.sortBy.empty());

      runtime.workspace().saveSession(runtime.configStore());

      auto loaded = SessionState{};
      workspaceConfigStore->load("workspace", loaded);
      REQUIRE(loaded.openViews.size() == 1);
      CHECK(loaded.openViews[0].groupBy == TrackGroupKey::Artist);

      // Restore in new runtime
      auto session2 = AppRuntime{
        AppRuntimeDependencies{.executor = std::make_unique<MockExecutor>(),
                                   .musicRoot = tempDir.path(),
                                   .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
                                   .workspaceConfigStore = workspaceConfigStore}};

      session2.workspace().restoreSession(session2.configStore());

      auto const layout2 = session2.workspace().layoutState();
      REQUIRE(layout2.openViews.size() == 1);
      auto const restoredState = session2.views().trackListState(layout2.openViews[0]);
      CHECK(restoredState.groupBy == TrackGroupKey::Artist);
      CHECK_FALSE(restoredState.sortBy.empty());
    }

    SECTION("Session persistence preserves groupBy=None")
    {
      // Ungrouped view
      runtime.workspace().navigateTo(ListId{10});
      runtime.workspace().saveSession(runtime.configStore());

      auto session2 = AppRuntime{
        AppRuntimeDependencies{.executor = std::make_unique<MockExecutor>(),
                                   .musicRoot = tempDir.path(),
                                   .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
                                   .workspaceConfigStore = workspaceConfigStore}};

      session2.workspace().restoreSession(session2.configStore());

      auto const layout2 = session2.workspace().layoutState();
      REQUIRE(layout2.openViews.size() == 1);
      auto const restoredState = session2.views().trackListState(layout2.openViews[0]);
      CHECK(restoredState.groupBy == TrackGroupKey::None);
    }
  }
}