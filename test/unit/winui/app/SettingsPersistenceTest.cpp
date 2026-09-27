// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/app/SettingsPersistence.h>

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include <ao/Error.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>
#include <ao/winui/layout/ShellState.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

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

  TEST_CASE("SettingsPersistence - persists the workspace checkpoint and the desktop candidate", "[winui][unit][app]")
  {
    auto tempDir = ao::test::TempDir{};
    auto const candidate = candidateSettings();

    // One runtime at a time per library directory; the restore side runs after this scope.
    {
      auto runtimePtr = rt::test::makeStateOnlyRuntime(tempDir);
      REQUIRE(navigateToAllTracks(*runtimePtr) != rt::kInvalidViewId);

      auto workspaceStore = rt::ConfigStore{tempDir.path() / "workspace.yaml"};
      auto settingsStore = rt::ConfigStore{tempDir.path() / "settings.yaml"};
      requirePersisted(runtimePtr->workspace(), workspaceStore, settingsStore, candidate);
    }

    // Fresh stores verify the exact persisted state rather than the return value.
    auto restoredRuntimePtr = rt::test::makeStateOnlyRuntime(tempDir);
    auto reopenWorkspaceStore = rt::ConfigStore{tempDir.path() / "workspace.yaml"};
    REQUIRE(restoredRuntimePtr->workspace().restoreSession(reopenWorkspaceStore));
    auto const restored =
      restoredRuntimePtr->views().trackListState(restoredRuntimePtr->workspace().snapshot().activeViewId);
    CHECK(restored.listId == ListId{rt::kAllTracksListId});

    auto reopenSettingsStore = rt::ConfigStore{tempDir.path() / "settings.yaml"};
    auto reloaded = DesktopSettings{};
    auto const presentRes = reopenSettingsStore.load("desktop", reloaded, DesktopSettingsYamlSchema{});
    REQUIRE(presentRes);
    CHECK(*presentRes);
    CHECK(reloaded == candidate);
  }

  TEST_CASE("SettingsPersistence - keeps the desktop result when the workspace checkpoint store fails",
            "[winui][unit][app]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtimePtr = rt::test::makeStateOnlyRuntime(tempDir);

    // A directory at the store path makes every workspace write fail.
    auto const blockedWorkspacePath = tempDir.path() / "blocked-workspace.yaml";
    REQUIRE(std::filesystem::create_directory(blockedWorkspacePath));
    auto blockedWorkspaceStore = rt::ConfigStore{blockedWorkspacePath};
    auto settingsStore = rt::ConfigStore{tempDir.path() / "settings.yaml"};

    auto const candidate = candidateSettings();
    requirePersisted(runtimePtr->workspace(), blockedWorkspaceStore, settingsStore, candidate);

    auto reopenSettingsStore = rt::ConfigStore{tempDir.path() / "settings.yaml"};
    auto reloaded = DesktopSettings{};
    auto const presentRes = reopenSettingsStore.load("desktop", reloaded, DesktopSettingsYamlSchema{});
    REQUIRE(presentRes);
    CHECK(*presentRes);
    CHECK(reloaded == candidate);
  }

  TEST_CASE("SettingsPersistence - keeps the earlier workspace checkpoint when the desktop save fails",
            "[winui][unit][app]")
  {
    auto tempDir = ao::test::TempDir{};

    // One runtime at a time per library directory; the restore side runs after this scope.
    {
      auto runtimePtr = rt::test::makeStateOnlyRuntime(tempDir);
      REQUIRE(navigateToAllTracks(*runtimePtr) != rt::kInvalidViewId);

      auto workspaceStore = rt::ConfigStore{tempDir.path() / "workspace.yaml"};
      auto const blockedSettingsPath = tempDir.path() / "blocked-settings.yaml";
      REQUIRE(std::filesystem::create_directory(blockedSettingsPath));
      auto blockedSettingsStore = rt::ConfigStore{blockedSettingsPath};

      auto const failedRes = persistDesktopSettingsCandidate(
        runtimePtr->workspace(), workspaceStore, blockedSettingsStore, candidateSettings());

      REQUIRE_FALSE(failedRes);
      CHECK(failedRes.error().code == Error::Code::IoError);
    }

    // The workspace checkpoint that ran before the failed desktop save stays written.
    auto restoredRuntimePtr = rt::test::makeStateOnlyRuntime(tempDir);
    auto reopenWorkspaceStore = rt::ConfigStore{tempDir.path() / "workspace.yaml"};
    REQUIRE(restoredRuntimePtr->workspace().restoreSession(reopenWorkspaceStore));
    auto const restored =
      restoredRuntimePtr->views().trackListState(restoredRuntimePtr->workspace().snapshot().activeViewId);
    CHECK(restored.listId == ListId{rt::kAllTracksListId});
  }

  TEST_CASE("SettingsPersistence - reports the desktop failure when both stores fail", "[winui][unit][app]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtimePtr = rt::test::makeStateOnlyRuntime(tempDir);

    auto const blockedWorkspacePath = tempDir.path() / "blocked-workspace.yaml";
    auto const blockedSettingsPath = tempDir.path() / "blocked-settings.yaml";
    REQUIRE(std::filesystem::create_directory(blockedWorkspacePath));
    REQUIRE(std::filesystem::create_directory(blockedSettingsPath));
    auto blockedWorkspaceStore = rt::ConfigStore{blockedWorkspacePath};
    auto blockedSettingsStore = rt::ConfigStore{blockedSettingsPath};

    auto const failedRes = persistDesktopSettingsCandidate(
      runtimePtr->workspace(), blockedWorkspaceStore, blockedSettingsStore, candidateSettings());

    REQUIRE_FALSE(failedRes);
    CHECK(failedRes.error().code == Error::Code::IoError);
  }
} // namespace ao::winui::test
