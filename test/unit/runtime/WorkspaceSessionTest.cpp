// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include <ao/CoreIds.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <fstream>
#include <memory>

namespace ao::rt::test
{
  using namespace ao::test;

  TEST_CASE("WorkspaceService - session restore recreates the initial navigation point",
            "[runtime][unit][workspace][session]")
  {
    auto tempDir = TempDir{};

    {
      auto runtime = makeRuntime(tempDir);
      runtime.workspace().navigateTo(ListId{10});
      runtime.workspace().saveSession(runtime.configStore());
    }

    {
      auto runtime = makeRuntime(tempDir);
      runtime.workspace().restoreSession(runtime.configStore());

      auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
      CHECK(state.listId == ListId{10});
      CHECK(runtime.workspace().canGoBack() == false);
      CHECK(runtime.workspace().canGoForward() == false);
    }
  }

  TEST_CASE("WorkspaceService - restored sessions can navigate back to restored state",
            "[runtime][unit][workspace][session]")
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

  TEST_CASE("WorkspaceService - saveSession tolerates flush failures", "[runtime][unit][workspace][session]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto badConfigStorePtr = std::make_shared<ConfigStore>(tempDir.path());
    CHECK_NOTHROW(runtime.workspace().saveSession(*badConfigStorePtr));
  }

  TEST_CASE("WorkspaceService - restoreSession tolerates malformed config", "[runtime][unit][workspace][session]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto const configPath = tempDir.path() / "bad.yaml";
    std::ofstream{configPath} << "workspace: \"not a map\"";

    auto badConfigStorePtr = std::make_shared<ConfigStore>(configPath, ConfigStore::OpenMode::ReadOnly);
    CHECK_NOTHROW(runtime.workspace().restoreSession(*badConfigStorePtr));
  }

  TEST_CASE("WorkspaceService - session restore falls back to front view if active is lost",
            "[runtime][unit][workspace][session]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto const listId = runtime.library().writer().createList(LibraryWriter::ListDraft{.name = "A list"});
    auto const configPath = tempDir.path() / "config.yaml";

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
} // namespace ao::rt::test
