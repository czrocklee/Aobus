// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include <ao/CoreIds.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <fstream>
#include <memory>

namespace ao::rt::test
{
  using namespace ao::test;

  TEST_CASE("WorkspaceService - missing workspace config is a successful empty restore",
            "[runtime][unit][workspace][session]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto const result = runtime.workspace().restoreSession(runtime.workspaceConfigStore());

    REQUIRE(result);
    CHECK(runtime.workspace().snapshot().openViews.empty());
    CHECK(runtime.workspace().snapshot().activeViewId == kInvalidViewId);
  }

  TEST_CASE("WorkspaceService - session restore recreates the initial navigation point",
            "[runtime][unit][workspace][session]")
  {
    auto tempDir = TempDir{};
    auto listId = kInvalidListId;

    {
      auto runtime = makeRuntime(tempDir);
      listId =
        ao::test::requireValue(runtime.library().writer().createList(LibraryWriter::ListDraft{.name = "Restored"}));
      REQUIRE(runtime.workspace().navigateTo(listId));
      runtime.workspace().saveSession(runtime.workspaceConfigStore());
    }

    {
      auto runtime = makeRuntime(tempDir);
      REQUIRE(runtime.workspace().restoreSession(runtime.workspaceConfigStore()));

      auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
      CHECK(state.listId == listId);
      CHECK(runtime.workspace().canGoBack() == false);
      CHECK(runtime.workspace().canGoForward() == false);
    }
  }

  TEST_CASE("WorkspaceService - restored sessions can navigate back to restored state",
            "[runtime][unit][workspace][session]")
  {
    auto tempDir = TempDir{};
    auto firstListId = kInvalidListId;
    auto secondListId = kInvalidListId;

    {
      auto runtime = makeRuntime(tempDir);
      firstListId = ao::test::requireValue(
        runtime.library().writer().createList(LibraryWriter::ListDraft{.name = "First restored"}));
      secondListId = ao::test::requireValue(
        runtime.library().writer().createList(LibraryWriter::ListDraft{.name = "After restore"}));
      REQUIRE(runtime.workspace().navigateTo(firstListId));
      runtime.workspace().saveSession(runtime.workspaceConfigStore());
    }

    auto runtime = makeRuntime(tempDir);
    REQUIRE(runtime.workspace().restoreSession(runtime.workspaceConfigStore()));
    REQUIRE(runtime.workspace().navigateTo(secondListId));

    CHECK(runtime.workspace().canGoBack() == true);
    REQUIRE(runtime.workspace().goBack());
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == firstListId);
  }

  TEST_CASE("WorkspaceService - multi-view restore publishes one complete snapshot",
            "[runtime][unit][workspace][session]")
  {
    auto tempDir = TempDir{};

    {
      auto runtime = makeRuntime(tempDir);
      auto const firstListId = ao::test::requireValue(
        runtime.library().writer().createList(LibraryWriter::ListDraft{.name = "First snapshot"}));
      auto const secondListId = ao::test::requireValue(
        runtime.library().writer().createList(LibraryWriter::ListDraft{.name = "Second snapshot"}));
      REQUIRE(runtime.workspace().navigateTo(firstListId));
      REQUIRE(runtime.workspace().navigateTo(secondListId));
      runtime.workspace().saveSession(runtime.workspaceConfigStore());
    }

    auto runtime = makeRuntime(tempDir);
    std::int32_t changeCount = 0;
    auto changed = WorkspaceChanged{};
    auto const sub = runtime.workspace().onChanged(
      [&](WorkspaceChanged const& value)
      {
        ++changeCount;
        changed = value;
      });

    auto const receipt = ao::test::requireValue(runtime.workspace().restoreSession(runtime.workspaceConfigStore()));

    CHECK(changeCount == 1);
    CHECK(changed.cause == WorkspaceChangeCause::Restore);
    CHECK(changed.snapshot.openViews.size() == 2);
    CHECK(changed.snapshot == runtime.workspace().snapshot());
    CHECK(receipt.beforeRevision == 0);
    CHECK(receipt.afterRevision == 1);
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
    auto const result = runtime.workspace().restoreSession(*badConfigStorePtr);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::FormatRejected);
  }

  TEST_CASE("WorkspaceService - session restore falls back to front view if active is lost",
            "[runtime][unit][workspace][session]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto const listId =
      ao::test::requireValue(runtime.library().writer().createList(LibraryWriter::ListDraft{.name = "A list"}));
    auto const configPath = tempDir.path() / "config.yaml";

    {
      auto file = std::ofstream{configPath};
      file << "workspace:\n";
      file << "  activeListId: 9999\n";
      file << "  openViews:\n";
      file << "    - listId: " << static_cast<std::uint32_t>(listId) << "\n";
    }

    auto storePtr = std::make_shared<ConfigStore>(configPath, ConfigStore::OpenMode::ReadOnly);
    REQUIRE(runtime.workspace().restoreSession(*storePtr));

    auto layout = runtime.workspace().snapshot();
    CHECK(layout.openViews.size() == 1);
    CHECK(layout.activeViewId == layout.openViews.front());
  }

  TEST_CASE("WorkspaceService - failed session restore leaves workspace views focus and history unchanged",
            "[runtime][unit][workspace][session]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);
    auto const listId =
      ao::test::requireValue(runtime.library().writer().createList(LibraryWriter::ListDraft{.name = "Valid"}));
    auto const configPath = tempDir.path() / "partial.yaml";

    {
      auto file = std::ofstream{configPath};
      file << "workspace:\n";
      file << "  activeListId: " << static_cast<std::uint32_t>(listId) << "\n";
      file << "  openViews:\n";
      file << "    - listId: " << static_cast<std::uint32_t>(listId) << "\n";
      file << "    - listId: 999999\n";
    }

    auto storePtr = std::make_shared<ConfigStore>(configPath, ConfigStore::OpenMode::ReadOnly);
    auto const result = runtime.workspace().restoreSession(*storePtr);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
    auto const layout = runtime.workspace().snapshot();
    CHECK(layout.openViews.empty());
    CHECK(layout.activeViewId == kInvalidViewId);
    CHECK(layout.revision == 0);
    CHECK(runtime.views().listViews().empty());
    CHECK_FALSE(runtime.workspace().canGoBack());
    CHECK_FALSE(runtime.workspace().canGoForward());
  }
} // namespace ao::rt::test
