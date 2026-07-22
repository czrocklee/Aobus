// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include <ao/CoreIds.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <memory>
#include <string>
#include <string_view>

namespace
{
  void writeWorkspaceConfig(std::filesystem::path const& path,
                            std::initializer_list<std::uint32_t> listIds,
                            std::uint32_t activeViewIndex,
                            std::string_view group = "none",
                            std::uint32_t presentationVersion = 1)
  {
    auto file = std::ofstream{path};
    file << "workspace:\n";
    file << "  presentationVersion: " << presentationVersion << "\n";
    file << "  openViews:";

    if (listIds.size() == 0)
    {
      file << " []\n";
    }
    else
    {
      file << "\n";
    }

    for (auto const listId : listIds)
    {
      file << "    - listId: " << listId << "\n";
      file << "      filterExpression: \"\"\n";
      file << "      presentation:\n";
      file << "        id: library\n";
      file << "        group: " << group << "\n";
      file << "        sort: []\n";
      file << "        visibleFields:\n";
      file << "          - title\n";
      file << "        redundantFields: []\n";
    }

    file << "  activeViewIndex: " << activeViewIndex << "\n";
    file << "  customPresets: []\n";
  }
} // namespace

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

  TEST_CASE("WorkspaceService - missing workspace group preserves an existing workspace",
            "[runtime][unit][workspace][session]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);
    auto const listId = ao::test::requireValue(runtime.library().writer().createList(
      LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "Existing"}));
    REQUIRE(runtime.workspace().navigate({.target = listId}));
    auto const before = runtime.workspace().snapshot();
    auto const configPath = tempDir.path() / "other-group.yaml";
    std::ofstream{configPath} << "other:\n"
                                 "  value: 1\n";
    auto store = ConfigStore{configPath, ConfigStore::OpenMode::ReadOnly};

    REQUIRE(runtime.workspace().restoreSession(store));
    CHECK(runtime.workspace().snapshot() == before);
    CHECK(runtime.views().listViews().size() == 1);
  }

  TEST_CASE("WorkspaceService - session restore recreates the initial navigation point",
            "[runtime][unit][workspace][session]")
  {
    auto tempDir = TempDir{};
    auto listId = kInvalidListId;

    {
      auto runtime = makeRuntime(tempDir);
      listId = ao::test::requireValue(runtime.library().writer().createList(
        LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "Restored"}));
      REQUIRE(runtime.workspace().navigate({.target = listId}));
      runtime.workspace().saveSession(runtime.workspaceConfigStore());
    }

    {
      auto runtime = makeRuntime(tempDir);
      REQUIRE(runtime.workspace().restoreSession(runtime.workspaceConfigStore()));

      auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
      CHECK(state.listId == listId);
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
      firstListId = ao::test::requireValue(runtime.library().writer().createList(
        LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "First restored"}));
      secondListId = ao::test::requireValue(runtime.library().writer().createList(
        LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "After restore"}));
      REQUIRE(runtime.workspace().navigate({.target = firstListId}));
      runtime.workspace().saveSession(runtime.workspaceConfigStore());
    }

    auto runtime = makeRuntime(tempDir);
    REQUIRE(runtime.workspace().restoreSession(runtime.workspaceConfigStore()));
    REQUIRE(runtime.workspace().navigate({.target = secondListId}));

    REQUIRE(runtime.workspace().goBack());
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == firstListId);
  }

  TEST_CASE("WorkspaceService - session round-trip restores the exact active view over one list",
            "[runtime][unit][workspace][session]")
  {
    std::size_t expectedActiveIndex = 0;

    SECTION("First ordered view is active")
    {
      expectedActiveIndex = 0;
    }

    SECTION("Second ordered view is active")
    {
      expectedActiveIndex = 1;
    }

    auto tempDir = TempDir{};
    auto listId = kInvalidListId;
    auto nextListId = kInvalidListId;
    auto firstPresentation = TrackPresentationSpec{};
    auto secondPresentation = TrackPresentationSpec{};

    {
      auto runtime = makeRuntime(tempDir);
      listId = ao::test::requireValue(runtime.library().writer().createList(
        LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "Shared list"}));
      nextListId = ao::test::requireValue(runtime.library().writer().createList(
        LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "After restore"}));
      auto const* songsPreset = builtinTrackPresentationPreset("songs");
      auto const* albumsPreset = builtinTrackPresentationPreset("albums");
      REQUIRE(songsPreset != nullptr);
      REQUIRE(albumsPreset != nullptr);
      firstPresentation = normalizeTrackPresentationSpec(songsPreset->spec);
      secondPresentation = normalizeTrackPresentationSpec(albumsPreset->spec);
      auto const firstViewId = ao::test::requireValue(runtime.workspace().navigate({
        .target = FilteredListTarget{.listId = listId, .filterExpression = "$title ~ \"First\""},
        .optPresentation =
          NavigationPresentation{.mode = NavigationPresentationMode::Override, .spec = firstPresentation},
      }));
      auto const secondViewId = ao::test::requireValue(runtime.workspace().navigate({
        .target = FilteredListTarget{.listId = listId, .filterExpression = "$title ~ \"Second\""},
        .optPresentation =
          NavigationPresentation{.mode = NavigationPresentationMode::Override, .spec = secondPresentation},
      }));

      if (expectedActiveIndex == 0)
      {
        REQUIRE(runtime.workspace().focusView(firstViewId));
      }
      else
      {
        CHECK(runtime.workspace().snapshot().activeViewId == secondViewId);
      }

      runtime.workspace().saveSession(runtime.workspaceConfigStore());
    }

    auto runtime = makeRuntime(tempDir);
    REQUIRE(runtime.workspace().restoreSession(runtime.workspaceConfigStore()));
    auto const restored = runtime.workspace().snapshot();
    REQUIRE(restored.openViews.size() == 2);
    CHECK(restored.activeViewId == restored.openViews[expectedActiveIndex]);
    auto const firstState = runtime.views().trackListState(restored.openViews[0]);
    auto const secondState = runtime.views().trackListState(restored.openViews[1]);
    CHECK(firstState.listId == listId);
    CHECK(firstState.filterExpression == "$title ~ \"First\"");
    CHECK(firstState.presentation == firstPresentation);
    CHECK(secondState.listId == listId);
    CHECK(secondState.filterExpression == "$title ~ \"Second\"");
    CHECK(secondState.presentation == secondPresentation);

    REQUIRE(runtime.workspace().navigate({.target = nextListId}));
    REQUIRE(runtime.workspace().goBack());
    CHECK(runtime.workspace().snapshot().activeViewId == restored.openViews[expectedActiveIndex]);
    auto const replayed = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(replayed.filterExpression == (expectedActiveIndex == 0 ? "$title ~ \"First\"" : "$title ~ \"Second\""));
    CHECK(replayed.presentation == (expectedActiveIndex == 0 ? firstPresentation : secondPresentation));
  }

  TEST_CASE("WorkspaceService - multi-view restore publishes one complete snapshot",
            "[runtime][unit][workspace][session]")
  {
    auto tempDir = TempDir{};

    {
      auto runtime = makeRuntime(tempDir);
      auto const firstListId = ao::test::requireValue(runtime.library().writer().createList(
        LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "First snapshot"}));
      auto const secondListId = ao::test::requireValue(runtime.library().writer().createList(
        LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "Second snapshot"}));
      REQUIRE(runtime.workspace().navigate({.target = firstListId}));
      REQUIRE(runtime.workspace().navigate({.target = secondListId}));
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

    REQUIRE(runtime.workspace().restoreSession(runtime.workspaceConfigStore()));

    CHECK(changeCount == 1);
    CHECK(changed.cause == WorkspaceChangeCause::Restore);
    CHECK(changed.snapshot.openViews.size() == 2);
    CHECK(changed.snapshot == runtime.workspace().snapshot());
    CHECK(changed.snapshot.revision == 1);
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

  TEST_CASE("WorkspaceService - out-of-bounds active index rejects before mutation",
            "[runtime][unit][workspace][session]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto const existingListId = ao::test::requireValue(runtime.library().writer().createList(
      LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "Existing"}));
    auto const storedListId = ao::test::requireValue(runtime.library().writer().createList(
      LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "Stored"}));
    auto const nextListId = ao::test::requireValue(runtime.library().writer().createList(
      LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "Next"}));
    auto const existingViewId = ao::test::requireValue(runtime.workspace().navigate({.target = existingListId}));
    auto const* songsPreset = builtinTrackPresentationPreset("songs");
    REQUIRE(songsPreset != nullptr);
    auto customSpec = songsPreset->spec;
    customSpec.id = "custom.keep";
    REQUIRE(runtime.workspace().addCustomPreset(
      CustomTrackPresentationPreset{.label = "Keep", .basePresetId = "songs", .spec = customSpec}));
    auto const beforeSnapshot = runtime.workspace().snapshot();
    auto const beforeViews = runtime.views().listViews();
    auto const configPath = tempDir.path() / "config.yaml";
    writeWorkspaceConfig(configPath, {storedListId.raw()}, 1);
    std::int32_t changeCount = 0;
    auto const sub = runtime.workspace().onChanged([&](WorkspaceChanged const&) { ++changeCount; });

    auto storePtr = std::make_shared<ConfigStore>(configPath, ConfigStore::OpenMode::ReadOnly);
    auto const result = runtime.workspace().restoreSession(*storePtr);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::FormatRejected);
    CHECK(runtime.workspace().snapshot() == beforeSnapshot);
    CHECK(runtime.views().listViews() == beforeViews);
    CHECK(runtime.workspace().customPresets().size() == 1);
    CHECK(changeCount == 0);

    REQUIRE(runtime.workspace().navigate({.target = nextListId}));
    REQUIRE(runtime.workspace().goBack());
    CHECK(runtime.workspace().snapshot().activeViewId == existingViewId);
  }

  TEST_CASE("WorkspaceService - empty session preserves empty focus and selects an existing front view",
            "[runtime][unit][workspace][session]")
  {
    auto tempDir = TempDir{};
    auto const configPath = tempDir.path() / "empty.yaml";
    writeWorkspaceConfig(configPath, {}, 0);
    auto store = ConfigStore{configPath, ConfigStore::OpenMode::ReadOnly};

    SECTION("Empty workspace remains unfocused")
    {
      auto runtime = makeRuntime(tempDir);
      REQUIRE(runtime.workspace().restoreSession(store));
      CHECK(runtime.workspace().snapshot().openViews.empty());
      CHECK(runtime.workspace().snapshot().activeViewId == kInvalidViewId);
      CHECK(runtime.workspace().snapshot().revision == 0);
    }

    SECTION("Existing workspace selects its first view")
    {
      auto runtime = makeRuntime(tempDir);
      auto const firstListId = ao::test::requireValue(runtime.library().writer().createList(
        LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "First existing"}));
      auto const secondListId = ao::test::requireValue(runtime.library().writer().createList(
        LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "Second existing"}));
      auto const firstViewId = ao::test::requireValue(runtime.workspace().navigate({.target = firstListId}));
      REQUIRE(runtime.workspace().navigate({.target = secondListId}));
      auto const beforeViews = runtime.views().listViews();

      REQUIRE(runtime.workspace().restoreSession(store));
      CHECK(runtime.workspace().snapshot().activeViewId == firstViewId);
      CHECK(runtime.views().listViews() == beforeViews);
    }
  }

  TEST_CASE("WorkspaceService - failed session restore leaves workspace views focus and history unchanged",
            "[runtime][unit][workspace][session]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);
    auto const listId = ao::test::requireValue(runtime.library().writer().createList(
      LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "Valid"}));
    auto const configPath = tempDir.path() / "partial.yaml";

    writeWorkspaceConfig(configPath, {listId.raw(), 999999}, 0);

    auto storePtr = std::make_shared<ConfigStore>(configPath, ConfigStore::OpenMode::ReadOnly);
    auto const result = runtime.workspace().restoreSession(*storePtr);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
    auto const layout = runtime.workspace().snapshot();
    CHECK(layout.openViews.empty());
    CHECK(layout.activeViewId == kInvalidViewId);
    CHECK(layout.revision == 0);
    CHECK(runtime.views().listViews().empty());
  }

  TEST_CASE("WorkspaceService - restore rejects unsupported or unknown presentation vocabulary",
            "[runtime][unit][workspace][session]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);
    auto const listId = ao::test::requireValue(runtime.library().writer().createList(
      LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "Valid"}));
    auto const configPath = tempDir.path() / "versioned.yaml";
    auto expectedCode = Error::Code::FormatRejected;

    SECTION("Unsupported presentation version")
    {
      writeWorkspaceConfig(configPath, {listId.raw()}, 0, "none", 2);
      expectedCode = Error::Code::NotSupported;
    }

    SECTION("Unknown group id")
    {
      writeWorkspaceConfig(configPath, {listId.raw()}, 0, "future-group");
    }

    SECTION("Unknown root field")
    {
      writeWorkspaceConfig(configPath, {listId.raw()}, 0);
      std::ofstream{configPath, std::ios::app} << "  unexpected: true\n";
    }

    auto storePtr = std::make_shared<ConfigStore>(configPath, ConfigStore::OpenMode::ReadOnly);
    auto const result = runtime.workspace().restoreSession(*storePtr);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == expectedCode);
    CHECK(runtime.workspace().snapshot().openViews.empty());
    CHECK(runtime.views().listViews().empty());
  }

  TEST_CASE("WorkspaceService - restore rejects the unversioned numeric presentation format",
            "[runtime][unit][workspace][session]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);
    auto const configPath = tempDir.path() / "unversioned.yaml";
    std::ofstream{configPath} << "workspace:\n"
                                 "  openViews: []\n"
                                 "  activeViewIndex: 0\n"
                                 "  customPresets: []\n";

    auto storePtr = std::make_shared<ConfigStore>(configPath, ConfigStore::OpenMode::ReadOnly);
    auto const result = runtime.workspace().restoreSession(*storePtr);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::FormatRejected);
    CHECK(runtime.workspace().snapshot().revision == 0);
  }
} // namespace ao::rt::test
