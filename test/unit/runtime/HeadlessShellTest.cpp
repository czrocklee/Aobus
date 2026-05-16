// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <runtime/AppRuntime.h>
#include <runtime/ConfigStore.h>
#include <runtime/CorePrimitives.h>
#include <runtime/PlaybackService.h>
#include <runtime/SessionPersistenceService.h>
#include <runtime/StateTypes.h>
#include <runtime/ViewService.h>
#include <runtime/WorkspaceService.h>

#include <filesystem>
#include <functional>
#include <memory>

#include <test/unit/lmdb/TestUtils.h>

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
    auto executor = std::make_shared<MockExecutor>();
    auto configStore = std::make_shared<ConfigStore>(std::filesystem::path(tempDir.path()) / "config.yaml");

    auto runtime = AppRuntime{
      AppRuntimeDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore}};

    SECTION("Initial layout is empty")
    {
      auto const layout = runtime.workspace().layoutState();
      CHECK(layout.openViews.empty());
      CHECK(layout.activeViewId == ViewId{});
    }

    SECTION("Navigate to list ID creates a view and marks it active")
    {
      auto const listId = ListId{42};
      runtime.workspace().navigateTo(listId);

      auto const layout = runtime.workspace().layoutState();
      REQUIRE(layout.openViews.size() == 1);
      CHECK(layout.activeViewId == layout.openViews.front());

      auto const viewId = ViewId{layout.activeViewId};
      auto const viewState = runtime.views().trackListState(viewId);
      CHECK(viewState.listId == listId);
    }

    SECTION("Closing a view updates the layout")
    {
      runtime.workspace().navigateTo(ListId{1});
      runtime.workspace().navigateTo(ListId{2});

      auto layout1 = runtime.workspace().layoutState();
      REQUIRE(layout1.openViews.size() == 2);
      auto const viewToClose = ViewId{layout1.openViews.front()};
      auto const remainingView = ViewId{layout1.openViews.back()};

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
      runtime.persistence().save();

      auto loaded = rt::SessionSnapshot{};
      configStore->load("runtime", loaded);
      CHECK(loaded.openViews.size() == 2);

      // Create new runtime with same persistence
      AppRuntime session2(
        AppRuntimeDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore});

      session2.persistence().restore();

      auto const layout = session2.workspace().layoutState();
      CHECK(layout.openViews.size() == 2);
      CHECK(layout.activeViewId != ViewId{});
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

      runtime.persistence().save();

      auto loaded = rt::SessionSnapshot{};
      configStore->load("runtime", loaded);
      REQUIRE(loaded.openViews.size() == 1);
      CHECK(loaded.openViews[0].groupBy == TrackGroupKey::Artist);

      // Restore in new runtime
      AppRuntime session2(
        AppRuntimeDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore});

      session2.persistence().restore();

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
      runtime.persistence().save();

      AppRuntime session2(
        AppRuntimeDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore});

      session2.persistence().restore();

      auto const layout2 = session2.workspace().layoutState();
      REQUIRE(layout2.openViews.size() == 1);
      auto const restoredState = session2.views().trackListState(layout2.openViews[0]);
      CHECK(restoredState.groupBy == TrackGroupKey::None);
    }
  }
}