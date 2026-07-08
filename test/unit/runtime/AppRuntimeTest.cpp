// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include <ao/CoreIds.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <utility>

namespace ao::rt::test
{
  using namespace ao::test;

  TEST_CASE("AppRuntime - dependencies expose services and empty selection is safe", "[runtime][unit][app-runtime]")
  {
    auto tempDir = ao::test::TempDir{};

    auto appPtr = std::make_unique<AppRuntime>(AppRuntimeDependencies{
      .executorPtr = std::make_unique<MockExecutor>(),
      .musicRoot = tempDir.path(),
      .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
      .workspaceConfigStorePtr =
        std::make_unique<ConfigStore>(std::filesystem::path{tempDir.path()} / "workspace.yaml"),
    });

    CHECK(appPtr->musicRoot() == std::filesystem::path{tempDir.path()});
    CHECK(appPtr->databasePath() == std::filesystem::path{tempDir.path()} / ".aobus" / "library");

    // Verify accessors
    [[maybe_unused]] auto& commands = appPtr->library().writer();
    [[maybe_unused]] auto& notifications = appPtr->notifications();

    appPtr->addAudioProvider(
      makeReadyAudioProvider(audio::BackendProvider::Status{.metadata = {.id = audio::BackendId{"dummy"}}}));

    // reloadAllTracks
    CHECK_NOTHROW(appPtr->reloadAllTracks());

    // playSelectionInFocusedView (with no focused view)
    CHECK(appPtr->playSelectionInFocusedView() == kInvalidTrackId);

    // Add a view and focus it
    appPtr->workspace().navigateTo(GlobalViewKind::AllTracks);

    // playSelectionInFocusedView (with focused view but no selection)
    CHECK(appPtr->playSelectionInFocusedView() == kInvalidTrackId);

    // Cover polymorphic destruction of CoreRuntime
    auto const corePtr = std::unique_ptr<CoreRuntime>{std::move(appPtr)};
  }
} // namespace ao::rt::test
