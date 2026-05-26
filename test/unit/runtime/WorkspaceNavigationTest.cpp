// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/Type.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>

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

  namespace
  {
    auto makeRuntime(TempDir const& tempDir, std::shared_ptr<ConfigStore> workspaceConfigStore = nullptr)
    {
      if (!workspaceConfigStore)
      {
        workspaceConfigStore = std::make_shared<ConfigStore>(std::filesystem::path{tempDir.path()} / "workspace.yaml");
      }

      return AppRuntime{AppRuntimeDependencies{
        .executor = std::make_unique<MockExecutor>(),
        .musicRoot = tempDir.path(),
        .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
        .workspaceConfigStore = std::move(workspaceConfigStore),
      }};
    }
  }

  TEST_CASE("NavigationWorkspace - first navigateTo commits", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});

    CHECK(runtime.workspace().canGoBack() == false);
    CHECK(runtime.workspace().canGoForward() == false);
    auto const layout = runtime.workspace().layoutState();
    CHECK(layout.activeViewId != kInvalidViewId);
    auto const state = runtime.views().trackListState(layout.activeViewId);
    CHECK(state.listId == ListId{10});
  }

  TEST_CASE("NavigationWorkspace - second navigateTo makes canGoBack true", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});

    CHECK(runtime.workspace().canGoBack() == true);
    CHECK(runtime.workspace().canGoForward() == false);
  }

  TEST_CASE("NavigationWorkspace - navigateTo same list dedups", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});
    runtime.workspace().navigateTo(ListId{20}); // same as current

    CHECK(runtime.workspace().canGoBack() == true);
    // The duplicate should not grow history; back still goes to 10.
    runtime.workspace().goBack();
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == ListId{10});
  }

  TEST_CASE("NavigationWorkspace - navigateTo AllTracks", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(GlobalViewKind::AllTracks);

    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == kAllTracksListId);
    CHECK(runtime.workspace().canGoBack() == true);
  }

  TEST_CASE("NavigationWorkspace - navigateTo with recordHistory false", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20}, {.recordHistory = false});

    // Active view changed to B, but history should NOT have grown.
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == ListId{20});
    CHECK(runtime.workspace().canGoBack() == false);
  }

  TEST_CASE("NavigationWorkspace - navigateTo query commits filter", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo("genre == \"Rock\"");

    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.filterExpression == "genre == \"Rock\"");
    CHECK(runtime.workspace().canGoBack() == true);

    // Back should restore unfiltered view.
    runtime.workspace().goBack();
    auto const backState = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(backState.listId == ListId{10});
    CHECK(backState.filterExpression.empty());
  }

  TEST_CASE("NavigationWorkspace - goBack restores list", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});
    runtime.workspace().navigateTo(ListId{30});

    CHECK(runtime.workspace().goBack());
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == ListId{20});
    CHECK(runtime.workspace().canGoBack() == true);
    CHECK(runtime.workspace().canGoForward() == true);
  }

  TEST_CASE("NavigationWorkspace - goBack twice restores first", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});
    runtime.workspace().navigateTo(ListId{30});

    runtime.workspace().goBack();
    runtime.workspace().goBack();

    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == ListId{10});
    CHECK(runtime.workspace().canGoBack() == false);
    CHECK(runtime.workspace().canGoForward() == true);
  }

  TEST_CASE("NavigationWorkspace - goForward after back", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});
    runtime.workspace().navigateTo(ListId{30});
    runtime.workspace().goBack();

    CHECK(runtime.workspace().goForward());
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == ListId{30});
    CHECK(runtime.workspace().canGoForward() == false);
  }

  TEST_CASE("NavigationWorkspace - goBack at boundary returns false", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    CHECK_FALSE(runtime.workspace().goBack());
  }

  TEST_CASE("NavigationWorkspace - goForward at boundary returns false", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    CHECK_FALSE(runtime.workspace().goForward());
  }

  TEST_CASE("NavigationWorkspace - new navigation after back truncates future", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});
    runtime.workspace().navigateTo(ListId{30});
    runtime.workspace().goBack(); // back to 20
    runtime.workspace().navigateTo(ListId{40});

    CHECK(runtime.workspace().canGoForward() == false);
    // First back goes to 20 (index 1), second back goes to 10 (index 0).
    runtime.workspace().goBack();
    auto const midState = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(midState.listId == ListId{20});
    runtime.workspace().goBack();
    auto const firstState = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(firstState.listId == ListId{10});
  }

  TEST_CASE("NavigationWorkspace - back restores presentation", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);
    runtime.workspace().setActivePresentation(albumsPreset->spec);

    runtime.workspace().goBack();
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.presentation.id == "songs");
  }

  TEST_CASE("NavigationWorkspace - close active view then back", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});
    auto const viewB = runtime.workspace().layoutState().activeViewId;
    runtime.workspace().closeView(viewB);

    // Closing the active view falls back to the remaining view (A=10).
    // back should still work — it restores the history entry for A.
    CHECK(runtime.workspace().goBack());
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == ListId{10});
  }

  TEST_CASE("NavigationWorkspace - onNavigationHistoryChanged emits on navigate", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto received = WorkspaceService::NavigationHistoryChanged{};
    auto const sub = runtime.workspace().onNavigationHistoryChanged([&](auto const& ev) { received = ev; });

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});

    CHECK(received.canGoBack == true);
    CHECK(received.canGoForward == false);
  }

  TEST_CASE("NavigationWorkspace - onNavigationHistoryChanged emits on back", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});

    auto received = WorkspaceService::NavigationHistoryChanged{};
    auto const sub = runtime.workspace().onNavigationHistoryChanged([&](auto const& ev) { received = ev; });

    runtime.workspace().goBack();

    CHECK(received.canGoBack == false);
    CHECK(received.canGoForward == true);
  }

  TEST_CASE("NavigationWorkspace - signal not emitted on dedup", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});

    std::int32_t callCount = 0;
    auto const sub = runtime.workspace().onNavigationHistoryChanged([&](auto const&) { ++callCount; });

    runtime.workspace().navigateTo(ListId{10}); // same list = dedup
    CHECK(callCount == 0);
  }

  TEST_CASE("NavigationWorkspace - session restore commits initial point", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto configStore = std::make_shared<ConfigStore>(std::filesystem::path{tempDir.path()} / "workspace.yaml");

    // Save session with one view.
    {
      auto runtime = makeRuntime(tempDir, configStore);
      runtime.workspace().navigateTo(ListId{10});
      runtime.workspace().saveSession(runtime.configStore());
    }

    // Restore in new runtime.
    {
      auto runtime = makeRuntime(tempDir, configStore);
      runtime.workspace().restoreSession(runtime.configStore());

      auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
      CHECK(state.listId == ListId{10});
      CHECK(runtime.workspace().canGoBack() == false);
      CHECK(runtime.workspace().canGoForward() == false);
    }
  }

  TEST_CASE("NavigationWorkspace - restore then navigate and back", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto configStore = std::make_shared<ConfigStore>(std::filesystem::path{tempDir.path()} / "workspace.yaml");

    {
      auto runtime = makeRuntime(tempDir, configStore);
      runtime.workspace().navigateTo(ListId{10});
      runtime.workspace().saveSession(runtime.configStore());
    }

    auto runtime = makeRuntime(tempDir, configStore);
    runtime.workspace().restoreSession(runtime.configStore());
    runtime.workspace().navigateTo(ListId{20});

    CHECK(runtime.workspace().canGoBack() == true);
    runtime.workspace().goBack();
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == ListId{10});
  }

  TEST_CASE("NavigationWorkspace - back does not grow history", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});

    // goBack + goForward should not add history entries.
    runtime.workspace().goBack();
    runtime.workspace().goForward();
    runtime.workspace().goBack();

    // History should still just be [10, 20].
    runtime.workspace().goForward();
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == ListId{20});
  }

  TEST_CASE("NavigationWorkspace - setActivePresentation commits", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});

    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);
    runtime.workspace().setActivePresentation(albumsPreset->spec);

    CHECK(runtime.workspace().canGoBack() == true);
    runtime.workspace().goBack();
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.presentation.id == "songs");
  }

  TEST_CASE("NavigationWorkspace - setActivePresentation with no active view is safe", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);
    REQUIRE_NOTHROW(runtime.workspace().setActivePresentation(albumsPreset->spec));
    CHECK(runtime.workspace().canGoBack() == false);
  }

  TEST_CASE("NavigationWorkspace - setActivePresentation dedups same spec", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);
    runtime.workspace().navigateTo(ListId{10});

    runtime.workspace().setActivePresentation(albumsPreset->spec);
    // Setting same presentation again should dedup.
    runtime.workspace().setActivePresentation(albumsPreset->spec);

    // Only one entry added (the albums switch). Back once goes to songs.
    runtime.workspace().goBack();
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.presentation.id == "songs");
    CHECK(runtime.workspace().canGoBack() == false);
  }

  TEST_CASE("NavigationWorkspace - setActivePresentation with recordHistory false", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});

    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);
    runtime.workspace().setActivePresentation(albumsPreset->spec, {.recordHistory = false});

    // Presentation changed but history unchanged.
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.presentation.id == "albums");
    CHECK(runtime.workspace().canGoBack() == false);
  }

  TEST_CASE("NavigationWorkspace - setActivePresentation by string id", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    auto const spec = runtime.workspace().setActivePresentation("albums");

    CHECK(spec.id == "albums");
    CHECK(spec.groupBy == TrackGroupKey::Album);
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.presentation.id == "albums");
  }

  TEST_CASE("NavigationWorkspace - setActivePresentation unknown id returns empty", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    auto const spec = runtime.workspace().setActivePresentation("nonexistent");

    CHECK(spec.id.empty());
    CHECK(runtime.workspace().canGoBack() == false);
  }

  TEST_CASE("NavigationWorkspace - jumpToAlbum invalid track no-ops", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().jumpToAlbum(kInvalidTrackId);

    // Invalid track → no-op, history unchanged, still at list 10.
    CHECK(runtime.workspace().canGoBack() == false);
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == ListId{10});
  }

  TEST_CASE("NavigationWorkspace - back after navigate compound returns to source", "[navigation][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});
    runtime.workspace().navigateTo(ListId{30});
    runtime.workspace().goBack();
    runtime.workspace().goBack();

    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == ListId{10});
    CHECK(runtime.workspace().canGoBack() == false);
    CHECK(runtime.workspace().canGoForward() == true);
  }
}
