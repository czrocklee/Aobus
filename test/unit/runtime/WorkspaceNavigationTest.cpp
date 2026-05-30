// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestUtils.h"
#include <ao/Type.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ListSourceStore.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

namespace ao::rt::test
{
  using namespace ao::lmdb::test;

  namespace
  {
  }

  TEST_CASE("NavigationWorkspace - first navigateTo commits", "[navigation][unit][workspace]")
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

  TEST_CASE("NavigationWorkspace - second navigateTo makes canGoBack true", "[navigation][unit][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});

    CHECK(runtime.workspace().canGoBack() == true);
    CHECK(runtime.workspace().canGoForward() == false);
  }

  TEST_CASE("NavigationWorkspace - navigateTo same list dedups", "[navigation][unit][workspace]")
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

  TEST_CASE("NavigationWorkspace - navigateTo AllTracks", "[navigation][unit][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(GlobalViewKind::AllTracks);

    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == kAllTracksListId);
    CHECK(runtime.workspace().canGoBack() == true);
  }

  TEST_CASE("NavigationWorkspace - navigateTo with recordHistory false", "[navigation][unit][workspace]")
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

  TEST_CASE("NavigationWorkspace - navigateTo query commits filter", "[navigation][unit][workspace]")
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

  TEST_CASE("NavigationWorkspace - goBack restores list", "[navigation][unit][workspace]")
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

  TEST_CASE("NavigationWorkspace - goBack twice restores first", "[navigation][unit][workspace]")
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

  TEST_CASE("NavigationWorkspace - goForward after back", "[navigation][unit][workspace]")
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

  TEST_CASE("NavigationWorkspace - goBack at boundary returns false", "[navigation][unit][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    CHECK_FALSE(runtime.workspace().goBack());
  }

  TEST_CASE("NavigationWorkspace - goForward at boundary returns false", "[navigation][unit][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    CHECK_FALSE(runtime.workspace().goForward());
  }

  TEST_CASE("NavigationWorkspace - new navigation after back truncates future", "[navigation][unit][workspace]")
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

  TEST_CASE("NavigationWorkspace - back restores presentation", "[navigation][unit][workspace]")
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

  TEST_CASE("NavigationWorkspace - close active view then back", "[navigation][unit][workspace]")
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

  TEST_CASE("NavigationWorkspace - onNavigationHistoryChanged emits on navigate", "[navigation][unit][workspace]")
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

  TEST_CASE("NavigationWorkspace - onNavigationHistoryChanged emits on back", "[navigation][unit][workspace]")
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

  TEST_CASE("NavigationWorkspace - signal not emitted on dedup", "[navigation][unit][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});

    std::int32_t callCount = 0;
    auto const sub = runtime.workspace().onNavigationHistoryChanged([&](auto const&) { ++callCount; });

    runtime.workspace().navigateTo(ListId{10}); // same list = dedup
    CHECK(callCount == 0);
  }

  TEST_CASE("NavigationWorkspace - session restore commits initial point", "[navigation][unit][workspace]")
  {
    auto tempDir = TempDir{};

    // Save session with one view.
    {
      auto runtime = makeRuntime(tempDir);
      runtime.workspace().navigateTo(ListId{10});
      runtime.workspace().saveSession(runtime.configStore());
    }

    // Restore in new runtime.
    {
      auto runtime = makeRuntime(tempDir);
      runtime.workspace().restoreSession(runtime.configStore());

      auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
      CHECK(state.listId == ListId{10});
      CHECK(runtime.workspace().canGoBack() == false);
      CHECK(runtime.workspace().canGoForward() == false);
    }
  }

  TEST_CASE("NavigationWorkspace - restore then navigate and back", "[navigation][unit][workspace]")
  {
    auto tempDir = TempDir{};

    {
      auto runtime = makeRuntime(tempDir);
      runtime.workspace().navigateTo(ListId{10});
      runtime.workspace().saveSession(runtime.configStore());
    }

    auto runtime = makeRuntime(tempDir);
    runtime.workspace().restoreSession(runtime.configStore());
    runtime.workspace().navigateTo(ListId{20});

    CHECK(runtime.workspace().canGoBack() == true);
    runtime.workspace().goBack();
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == ListId{10});
  }

  TEST_CASE("NavigationWorkspace - back does not grow history", "[navigation][unit][workspace]")
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

  TEST_CASE("NavigationWorkspace - setActivePresentation commits", "[navigation][unit][workspace]")
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

  TEST_CASE("NavigationWorkspace - setActivePresentation with no active view is safe", "[navigation][unit][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);
    REQUIRE_NOTHROW(runtime.workspace().setActivePresentation(albumsPreset->spec));
    CHECK(runtime.workspace().canGoBack() == false);
  }

  TEST_CASE("NavigationWorkspace - setActivePresentation dedups same spec", "[navigation][unit][workspace]")
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

  TEST_CASE("NavigationWorkspace - setActivePresentation with recordHistory false", "[navigation][unit][workspace]")
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

  TEST_CASE("NavigationWorkspace - setActivePresentation by string id", "[navigation][unit][workspace]")
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

  TEST_CASE("NavigationWorkspace - setActivePresentation unknown id returns empty", "[navigation][unit][workspace]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    auto const spec = runtime.workspace().setActivePresentation("nonexistent");

    CHECK(spec.id.empty());
    CHECK(runtime.workspace().canGoBack() == false);
  }

  TEST_CASE("NavigationWorkspace - jumpToAlbum invalid track no-ops", "[navigation][unit][workspace]")
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

  TEST_CASE("NavigationWorkspace - back after navigate compound returns to source", "[navigation][unit][workspace]")
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

  TEST_CASE("WorkspaceService - onFocusedViewChanged emits on focus changes", "[workspace][unit]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto focusedViewId = kInvalidViewId;
    auto const sub = runtime.workspace().onFocusedViewChanged([&](ViewId id) { focusedViewId = id; });

    runtime.workspace().navigateTo(ListId{10});
    auto activeViewId = runtime.workspace().layoutState().activeViewId;
    CHECK(focusedViewId == activeViewId);
  }

  TEST_CASE("WorkspaceService - custom presets management", "[workspace][unit]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    std::int32_t emitCount = 0;
    auto const sub = runtime.workspace().onCustomPresetsChanged([&] { emitCount++; });

    auto preset = CustomTrackPresentationPreset{};
    preset.label = "custom1";
    preset.spec.id = "custom1_id";
    preset.spec.groupBy = TrackGroupKey::Composer;

    runtime.workspace().addCustomPreset(preset);
    CHECK(emitCount == 1);

    auto presets = runtime.workspace().customPresets();
    REQUIRE(presets.size() == 1);
    CHECK(presets[0].label == "custom1");

    // Add another preset to see if it updates
    preset.spec.groupBy = TrackGroupKey::Work;
    runtime.workspace().addCustomPreset(preset);
    CHECK(emitCount == 2);
    presets = runtime.workspace().customPresets();
    REQUIRE(presets.size() == 1);
    CHECK(presets[0].spec.groupBy == TrackGroupKey::Work);

    // Retrieve preset by id
    runtime.workspace().navigateTo(ListId{10});
    auto const spec = runtime.workspace().setActivePresentation("custom1_id");
    CHECK(spec.groupBy == TrackGroupKey::Work);

    // Remove preset
    runtime.workspace().removeCustomPreset("custom1");
    CHECK(emitCount == 3);
    CHECK(runtime.workspace().customPresets().empty());
  }

  TEST_CASE("WorkspaceService - lists deleted causes views to close", "[workspace][unit]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto listId = runtime.mutation().createList(LibraryMutationService::ListDraft{.name = "Test List"});
    runtime.workspace().navigateTo(listId);

    auto activeViewId = runtime.workspace().layoutState().activeViewId;
    CHECK(activeViewId != kInvalidViewId);

    runtime.mutation().deleteList(listId);

    auto layout = runtime.workspace().layoutState();
    CHECK(!std::ranges::contains(layout.openViews, activeViewId));
  }

  TEST_CASE("WorkspaceService - jumpToAlbum valid track reveals track", "[workspace][unit]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto const trackId =
      TrackId{100}; // jumpToAlbum doesn't validate if track exists in library, it just passes the ID to playback

    bool revealCalled = false;
    auto const sub = runtime.playback().onRevealTrackRequested(
      [&](PlaybackService::RevealTrackRequested const& req)
      {
        if (req.trackId == trackId)
        {
          revealCalled = true;
        }
      });

    runtime.workspace().jumpToAlbum(trackId);
    CHECK(revealCalled == true);

    auto state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == kAllTracksListId);
    CHECK(state.presentation.id == "albums");
  }

  TEST_CASE("WorkspaceService - session flush error path", "[workspace][unit]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    // Create a config store using a directory path, which will fail to open as a file
    auto badConfigStorePtr = std::make_shared<ConfigStore>(tempDir.path());
    // Should not throw, but flush will fail and log an error
    REQUIRE_NOTHROW(runtime.workspace().saveSession(*badConfigStorePtr));
  }

  TEST_CASE("WorkspaceService - session restore error path", "[workspace][unit]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto const configPath = tempDir.path() + "/bad.yaml";
    std::ofstream{configPath} << "workspace: \"not a map\"";

    auto badConfigStorePtr = std::make_shared<ConfigStore>(configPath, ConfigStore::OpenMode::ReadOnly);
    // Should not throw, just return early and log a warning
    REQUIRE_NOTHROW(runtime.workspace().restoreSession(*badConfigStorePtr));
  }

  TEST_CASE("WorkspaceService - invalid navigation targets and presentations are handled gracefully",
            "[workspace][unit]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    // Test invalid target (invalid GlobalViewKind)
    runtime.workspace().navigateTo(static_cast<GlobalViewKind>(999));
    CHECK(runtime.workspace().layoutState().activeViewId == kInvalidViewId);

    // Test invalid presentation ID
    auto spec = runtime.workspace().setActivePresentation("non_existent_preset");
    CHECK(spec.id.empty());
  }

  TEST_CASE("WorkspaceService - goBack recreates view if destroyed", "[workspace][unit]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto const listA = runtime.mutation().createList(LibraryMutationService::ListDraft{.name = "A"});
    auto const listB = runtime.mutation().createList(LibraryMutationService::ListDraft{.name = "B"});

    runtime.workspace().navigateTo(listA, {.recordHistory = true});
    auto const viewA = runtime.workspace().layoutState().activeViewId;

    runtime.workspace().navigateTo(listB, {.recordHistory = true});
    auto const viewB = runtime.workspace().layoutState().activeViewId;

    CHECK(viewA != viewB);

    // Destroy View A
    runtime.workspace().closeView(viewA);

    // Go back to listA, this should recreate the view
    CHECK(runtime.workspace().goBack());

    auto const newViewA = runtime.workspace().layoutState().activeViewId;
    CHECK(newViewA != kInvalidViewId);
    CHECK(newViewA != viewA); // Should be a new ID since it was recreated
  }

  TEST_CASE("WorkspaceService - session restore falls back to front view if active is lost", "[workspace][unit]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto const listId = runtime.mutation().createList(LibraryMutationService::ListDraft{.name = "A list"});
    auto const configPath = tempDir.path() + "/config.yaml";

    {
      auto file = std::ofstream{configPath};
      file << "workspace:\n";
      file << "  activeListId: 9999\n";
      file << "  openViews:\n";
      file << "    - listId: " << static_cast<std::uint32_t>(listId) << "\n";
    }

    auto storePtr = std::make_shared<ConfigStore>(configPath, ConfigStore::OpenMode::ReadOnly);
    runtime.workspace().restoreSession(*storePtr);

    auto layout = runtime.workspace().layoutState();
    CHECK(layout.openViews.size() == 1);
    CHECK(layout.activeViewId == layout.openViews.front());
  }
}
