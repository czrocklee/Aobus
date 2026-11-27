// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/app/SettingsPersistence.h>

#include "test/TestFixtureSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include <ao/Error.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>
#include <ao/winui/WindowPlacement.h>
#include <ao/winui/layout/ShellState.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <utility>

namespace ao::winui::test
{
  namespace
  {
    DesktopSettings candidateSettings()
    {
      auto settings = DesktopSettings{};
      settings.window = WindowPlacement{.x = 11, .y = 22, .width = 800, .height = 600, .maximized = true};
      settings.shellMode = ShellMode::Classic;
      settings.lastLibraryPath = "C:/selected-library";
      return settings;
    }

    rt::ViewId navigateToAllTracks(rt::AppRuntime& runtime)
    {
      auto const navigateRes = runtime.workspace().navigate({.target = rt::GlobalViewKind::AllTracks});
      INFO(std::string{"navigate error: "} + (navigateRes ? std::string{} : std::string{navigateRes.error().message}));
      REQUIRE(navigateRes);
      return *navigateRes;
    }

    void requirePersisted(rt::WorkspaceService& workspace,
                          rt::ConfigStore& workspaceStore,
                          rt::ConfigStore& settingsStore,
                          DesktopSettings const& settings)
    {
      auto const persistedRes = persistDesktopSettingsCandidate(workspace, workspaceStore, settingsStore, settings);
      INFO(std::string{"persist error: "} + (persistedRes ? std::string{} : std::string{persistedRes.error().message}));
      REQUIRE(persistedRes);
    }
  } // namespace

  TEST_CASE("WorkspaceSortCheckpoint - defers and coalesces sort saves independently of desktop preferences")
  {
    auto tempDir = ao::test::TempDir{};
    auto const workspacePath = tempDir.path() / "workspace.yaml";
    auto const desktopPath = tempDir.path() / "windows-settings.yaml";
    constexpr auto kDesktopDocument = "# User settings\ndesktop:\n  version: 1\n  shellMode: classic\n";
    bool desktopBlocked = false;

    SECTION("existing desktop preferences")
    {
      std::ofstream{desktopPath, std::ios::binary} << kDesktopDocument;
      REQUIRE(ao::test::readFile(desktopPath) == kDesktopDocument);
    }

    SECTION("unwritable desktop settings path")
    {
      REQUIRE(std::filesystem::create_directory(desktopPath));
      desktopBlocked = true;
    }

    auto const firstCheckpointPath = tempDir.path() / "first-checkpoint.yaml";
    auto const firstSort = rt::TrackColumnSort{.column = rt::TrackField::Title, .ascending = false};
    auto const secondSort = rt::TrackColumnSort{.column = rt::TrackField::Title};
    {
      auto executorPtr = std::make_unique<rt::test::ManualExecutor>();
      auto& executor = *executorPtr;
      auto ownerPtr = rt::test::makeRuntime(tempDir, std::move(executorPtr));
      auto& runtime = ownerPtr->runtime();
      auto checkpoint = WorkspaceSortCheckpoint{executor, runtime.workspace(), runtime.workspaceConfigStore()};
      auto const viewId = navigateToAllTracks(runtime);

      REQUIRE(runtime.views().setColumnSort(viewId, secondSort));
      checkpoint.request();
      REQUIRE(runtime.views().setColumnSort(viewId, firstSort));
      auto const queuedBefore = executor.queuedCount();
      checkpoint.request();

      CHECK(executor.queuedCount() == queuedBefore);
      CHECK_FALSE(std::filesystem::exists(workspacePath));
      executor.runUntilIdle();
      REQUIRE(std::filesystem::copy_file(workspacePath, firstCheckpointPath));
      auto const firstSaved = ao::test::readFile(firstCheckpointPath);
      REQUIRE_FALSE(firstSaved.empty());

      REQUIRE(runtime.views().setColumnSort(viewId, secondSort));
      checkpoint.request();
      CHECK(ao::test::readFile(workspacePath) == firstSaved);
      executor.runUntilIdle();
      CHECK(ao::test::readFile(workspacePath) != firstSaved);
    }

    // Restore each snapshot after releasing the writer; only one runtime owns the library at a time.
    auto const checkSavedSort = [&](std::filesystem::path const& path, rt::TrackColumnSort const sort)
    {
      INFO(path.filename().string());
      auto restoredOwnerPtr = rt::test::makeStateOnlyRuntime(tempDir);
      auto& runtime = restoredOwnerPtr->runtime();
      auto store = rt::ConfigStore{path};
      REQUIRE(runtime.workspace().restoreSession(store));
      auto const viewId = runtime.workspace().snapshot().activeViewId;
      REQUIRE(viewId != rt::kInvalidViewId);
      auto const restored = runtime.views().trackListState(viewId);
      auto expected = rt::defaultTrackPresentationSpec();
      expected.optColumnSort = sort;
      CHECK(restored.presentation == expected);
      REQUIRE_FALSE(restored.sortBy.empty());
      CHECK(restored.sortBy.front() ==
            rt::TrackSortTerm{.field = rt::TrackSortField::Title, .ascending = sort.ascending});
    };
    checkSavedSort(firstCheckpointPath, firstSort);
    checkSavedSort(workspacePath, secondSort);

    if (desktopBlocked)
    {
      CHECK(std::filesystem::is_directory(desktopPath));
    }
    else
    {
      CHECK(ao::test::readFile(desktopPath) == kDesktopDocument);
    }
  }

  TEST_CASE("WorkspaceSortCheckpoint - retired work does not replace the final workspace checkpoint")
  {
    auto tempDir = ao::test::TempDir{};
    auto executorPtr = std::make_unique<rt::test::ManualExecutor>();
    auto& executor = *executorPtr;
    auto ownerPtr = rt::test::makeRuntime(tempDir, std::move(executorPtr));
    auto& runtime = ownerPtr->runtime();
    auto& workspace = runtime.workspace();
    auto& store = runtime.workspaceConfigStore();
    auto checkpoint = WorkspaceSortCheckpoint{executor, workspace, store};
    auto const viewId = navigateToAllTracks(runtime);
    REQUIRE(runtime.views().setColumnSort(viewId, {.column = rt::TrackField::Title}));
    checkpoint.request();

    // Mirror session retirement: disarm the queued task, then save while the old workspace is live.
    checkpoint.retire();
    workspace.saveSession(store);
    auto const workspacePath = tempDir.path() / "workspace.yaml";
    auto const saved = ao::test::readFile(workspacePath);
    REQUIRE_FALSE(saved.empty());

    REQUIRE(runtime.views().setColumnSort(viewId, {.column = rt::TrackField::Title, .ascending = false}));
    executor.runUntilIdle();
    CHECK(ao::test::readFile(workspacePath) == saved);
  }

  TEST_CASE("SettingsPersistence - persists the workspace checkpoint and the desktop candidate")
  {
    auto tempDir = ao::test::TempDir{};
    auto const candidate = candidateSettings();

    // One runtime at a time per library directory; the restore side runs after this scope.
    {
      auto ownerPtr = rt::test::makeStateOnlyRuntime(tempDir);
      auto& runtime = ownerPtr->runtime();
      REQUIRE(navigateToAllTracks(runtime) != rt::kInvalidViewId);

      auto workspaceStore = rt::ConfigStore{tempDir.path() / "workspace.yaml"};
      auto settingsStore = rt::ConfigStore{tempDir.path() / "settings.yaml"};
      requirePersisted(runtime.workspace(), workspaceStore, settingsStore, candidate);
    }

    // Fresh stores verify the exact persisted state rather than the return value.
    auto restoredOwnerPtr = rt::test::makeStateOnlyRuntime(tempDir);
    auto& runtime = restoredOwnerPtr->runtime();
    auto reopenWorkspaceStore = rt::ConfigStore{tempDir.path() / "workspace.yaml"};
    REQUIRE(runtime.workspace().restoreSession(reopenWorkspaceStore));
    auto const restored = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(restored.listId == ListId{rt::kAllTracksListId});

    auto reopenSettingsStore = rt::ConfigStore{tempDir.path() / "settings.yaml"};
    auto reloaded = DesktopSettings{};
    auto const presentRes = reopenSettingsStore.load("desktop", reloaded, DesktopSettingsYamlSchema{});
    REQUIRE(presentRes);
    CHECK(*presentRes);
    CHECK(reloaded == candidate);
  }

  TEST_CASE("SettingsPersistence - keeps the desktop result when the workspace checkpoint store fails")
  {
    auto tempDir = ao::test::TempDir{};
    auto ownerPtr = rt::test::makeStateOnlyRuntime(tempDir);

    // A directory at the store path makes every workspace write fail.
    auto const blockedWorkspacePath = tempDir.path() / "blocked-workspace.yaml";
    REQUIRE(std::filesystem::create_directory(blockedWorkspacePath));
    auto blockedWorkspaceStore = rt::ConfigStore{blockedWorkspacePath};
    auto settingsStore = rt::ConfigStore{tempDir.path() / "settings.yaml"};

    auto const candidate = candidateSettings();
    requirePersisted(ownerPtr->runtime().workspace(), blockedWorkspaceStore, settingsStore, candidate);

    auto reopenSettingsStore = rt::ConfigStore{tempDir.path() / "settings.yaml"};
    auto reloaded = DesktopSettings{};
    auto const presentRes = reopenSettingsStore.load("desktop", reloaded, DesktopSettingsYamlSchema{});
    REQUIRE(presentRes);
    CHECK(*presentRes);
    CHECK(reloaded == candidate);
  }

  TEST_CASE("SettingsPersistence - keeps the earlier workspace checkpoint when the desktop save fails")
  {
    auto tempDir = ao::test::TempDir{};

    // One runtime at a time per library directory; the restore side runs after this scope.
    {
      auto ownerPtr = rt::test::makeStateOnlyRuntime(tempDir);
      auto& runtime = ownerPtr->runtime();
      REQUIRE(navigateToAllTracks(runtime) != rt::kInvalidViewId);

      auto workspaceStore = rt::ConfigStore{tempDir.path() / "workspace.yaml"};
      auto const blockedSettingsPath = tempDir.path() / "blocked-settings.yaml";
      REQUIRE(std::filesystem::create_directory(blockedSettingsPath));
      auto blockedSettingsStore = rt::ConfigStore{blockedSettingsPath};

      auto const failedRes =
        persistDesktopSettingsCandidate(runtime.workspace(), workspaceStore, blockedSettingsStore, candidateSettings());

      REQUIRE_FALSE(failedRes);
      CHECK(failedRes.error().code == Error::Code::IoError);
    }

    // The workspace checkpoint that ran before the failed desktop save stays written.
    auto restoredOwnerPtr = rt::test::makeStateOnlyRuntime(tempDir);
    auto& runtime = restoredOwnerPtr->runtime();
    auto reopenWorkspaceStore = rt::ConfigStore{tempDir.path() / "workspace.yaml"};
    REQUIRE(runtime.workspace().restoreSession(reopenWorkspaceStore));
    auto const restored = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(restored.listId == ListId{rt::kAllTracksListId});
  }

  TEST_CASE("SettingsPersistence - reports the desktop failure when both stores fail")
  {
    auto tempDir = ao::test::TempDir{};
    auto ownerPtr = rt::test::makeStateOnlyRuntime(tempDir);

    auto const blockedWorkspacePath = tempDir.path() / "blocked-workspace.yaml";
    auto const blockedSettingsPath = tempDir.path() / "blocked-settings.yaml";
    REQUIRE(std::filesystem::create_directory(blockedWorkspacePath));
    REQUIRE(std::filesystem::create_directory(blockedSettingsPath));
    auto blockedWorkspaceStore = rt::ConfigStore{blockedWorkspacePath};
    auto blockedSettingsStore = rt::ConfigStore{blockedSettingsPath};

    auto const failedRes = persistDesktopSettingsCandidate(
      ownerPtr->runtime().workspace(), blockedWorkspaceStore, blockedSettingsStore, candidateSettings());

    REQUIRE_FALSE(failedRes);
    CHECK(failedRes.error().code == Error::Code::IoError);
  }
} // namespace ao::winui::test
