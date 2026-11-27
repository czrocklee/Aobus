// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/TestFixtureSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/utility/Uuid.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
  void writeWorkspaceConfig(std::filesystem::path const& path,
                            ao::utility::UuidBytes const& libraryId,
                            std::initializer_list<std::uint32_t> listIds,
                            std::uint32_t activeViewIndex,
                            std::string_view group = "none",
                            std::uint32_t presentationVersion = 3)
  {
    auto file = std::ofstream{path};
    file << "workspace:\n";
    file << "  presentationVersion: " << presentationVersion << "\n";
    file << "  libraryId: " << ao::utility::formatUuid(libraryId) << "\n";
    file << "  openViews:";

    if (std::empty(listIds))
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
      file << "        kind: builtin\n";
      file << "        presetId: library\n";
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

  namespace
  {
    utility::UuidBytes libraryIdOf(AppRuntime& runtime)
    {
      return runtime.library().snapshot().libraryId();
    }

    ListId createList(AppRuntimeOwner& owner, std::string name)
    {
      return ao::test::requireValue(
        runRuntimeTask(owner.runtime(), owner.libraryCommands().createListAsync(ListDraft{.name = std::move(name)})));
    }
  } // namespace

  TEST_CASE("WorkspaceService - missing workspace config is a successful empty restore")
  {
    auto tempDir = TempDir{};
    auto ownerPtr = makeStateOnlyRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();

    auto const res = runtime.workspace().restoreSession(runtime.workspaceConfigStore());

    REQUIRE(res);
    CHECK(runtime.workspace().snapshot().openViews.empty());
    CHECK(runtime.workspace().snapshot().activeViewId == kInvalidViewId);
  }

  TEST_CASE("WorkspaceService - missing workspace group preserves an existing workspace")
  {
    auto tempDir = TempDir{};
    auto ownerPtr = makeStateOnlyRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
    auto const listId = createList(*ownerPtr, "Existing");
    REQUIRE(runtime.workspace().navigate({.target = listId}));
    auto const before = runtime.workspace().snapshot();
    auto const configPath = tempDir.path() / "other-group.yaml";
    std::ofstream{configPath} << "other:\n"
                                 "  value: 1\n";
    auto store = ConfigStore{configPath, ConfigStore::OpenMode::ReadOnly};

    REQUIRE(runtime.workspace().restoreSession(store));
    CHECK(runtime.workspace().snapshot() == before);
    CHECK(runtime.workspace().snapshot().openViews.size() == 1);
  }

  TEST_CASE("WorkspaceService - session restore recreates the initial navigation point")
  {
    auto tempDir = TempDir{};
    auto listId = kInvalidListId;

    {
      auto ownerPtr = makeStateOnlyRuntime(tempDir);
      auto& runtime = ownerPtr->runtime();
      listId = createList(*ownerPtr, "Restored");
      REQUIRE(runtime.workspace().navigate({.target = listId}));
      runtime.workspace().saveSession(runtime.workspaceConfigStore());
    }

    {
      auto ownerPtr = makeStateOnlyRuntime(tempDir);
      auto& runtime = ownerPtr->runtime();
      REQUIRE(runtime.workspace().restoreSession(runtime.workspaceConfigStore()));

      auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
      CHECK(state.listId == listId);
    }
  }

  TEST_CASE("WorkspaceService - restored sessions can navigate back to restored state")
  {
    auto tempDir = TempDir{};
    auto firstListId = kInvalidListId;
    auto secondListId = kInvalidListId;

    {
      auto ownerPtr = makeStateOnlyRuntime(tempDir);
      auto& runtime = ownerPtr->runtime();
      firstListId = createList(*ownerPtr, "First restored");
      secondListId = createList(*ownerPtr, "After restore");
      REQUIRE(runtime.workspace().navigate({.target = firstListId}));
      runtime.workspace().saveSession(runtime.workspaceConfigStore());
    }

    auto ownerPtr = makeStateOnlyRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
    REQUIRE(runtime.workspace().restoreSession(runtime.workspaceConfigStore()));
    REQUIRE(runtime.workspace().navigate({.target = secondListId}));

    REQUIRE(runtime.workspace().goBack());
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == firstListId);
  }

  TEST_CASE("WorkspaceService - session round-trip restores the exact active view over one list")
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
      auto ownerPtr = makeStateOnlyRuntime(tempDir);
      auto& runtime = ownerPtr->runtime();
      listId = createList(*ownerPtr, "Shared list");
      nextListId = createList(*ownerPtr, "After restore");
      firstPresentation = normalizeTrackPresentationSpec(builtinTrackPresentationPreset(BuiltinPreset::Songs).spec);
      secondPresentation = normalizeTrackPresentationSpec(builtinTrackPresentationPreset(BuiltinPreset::Albums).spec);
      // The open view outlives its custom catalog entry; its stored fields and base order are authoritative.
      secondPresentation.presetId = CustomPresetId{42};
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

      auto const columnSort = TrackColumnSort{.column = TrackField::DisplayTrackNumber, .ascending = false};
      REQUIRE(runtime.views().setColumnSort(secondViewId, columnSort));
      secondPresentation.optColumnSort = columnSort;

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

    auto ownerPtr = makeStateOnlyRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
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
    CHECK(secondState.sortBy == secondPresentation.effectiveSortBy());
    CHECK(runtime.workspace().customPresets().empty());

    REQUIRE(runtime.workspace().navigate({.target = nextListId}));
    REQUIRE(runtime.workspace().goBack());
    CHECK(runtime.workspace().snapshot().activeViewId == restored.openViews[expectedActiveIndex]);
    auto const replayed = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(replayed.filterExpression == (expectedActiveIndex == 0 ? "$title ~ \"First\"" : "$title ~ \"Second\""));
    CHECK(replayed.presentation == (expectedActiveIndex == 0 ? firstPresentation : secondPresentation));
  }

  TEST_CASE("WorkspaceService - a session saved for another library restores only its custom presets")
  {
    auto savedDir = TempDir{};
    auto restoringDir = TempDir{};
    auto const configPath = restoringDir.path() / "other-library.yaml";
    auto savedListId = kInvalidListId;
    auto customPreset = CustomTrackPresentationPreset{};

    {
      auto ownerPtr = makeStateOnlyRuntime(savedDir);
      auto& runtime = ownerPtr->runtime();
      savedListId = createList(*ownerPtr, "Saved");
      REQUIRE(runtime.workspace().navigate({.target = savedListId}));
      auto customSpec = builtinTrackPresentationPreset(BuiltinPreset::Songs).spec;
      customSpec.presetId = CustomPresetId{42};
      customPreset =
        CustomTrackPresentationPreset{.label = "Portable", .optBasePresetId = BuiltinPreset::Songs, .spec = customSpec};
      REQUIRE(runtime.workspace().addCustomPreset(customPreset));
      auto store = ConfigStore{configPath};
      runtime.workspace().saveSession(store);
    }

    auto ownerPtr = makeStateOnlyRuntime(restoringDir);
    auto& runtime = ownerPtr->runtime();
    // A fresh library numbers its lists from 1 as well, so the saved id names an unrelated list here.
    REQUIRE(createList(*ownerPtr, "Unrelated") == savedListId);
    auto store = ConfigStore{configPath, ConfigStore::OpenMode::ReadOnly};

    REQUIRE(runtime.workspace().restoreSession(store));
    settleRuntimeCallbacks(runtime);
    auto const restored = runtime.workspace().snapshot();
    CHECK(restored.openViews.empty());
    CHECK(restored.activeViewId == kInvalidViewId);
    REQUIRE(runtime.workspace().customPresets().size() == 1);
    CHECK(runtime.workspace().customPresets().front() == customPreset);
  }

  TEST_CASE("WorkspaceService - multi-view restore publishes one complete snapshot")
  {
    auto tempDir = TempDir{};
    auto customSpec = builtinTrackPresentationPreset(BuiltinPreset::Songs).spec;
    customSpec.presetId = CustomPresetId{7};
    customSpec.visibleFields = {TrackField::Title, TrackField::Artist, TrackField::Duration};
    customSpec.redundantFields = {TrackField::Album};
    auto const expectedPreset = CustomTrackPresentationPreset{
      .label = "Session songs",
      .optBasePresetId = BuiltinPreset::Songs,
      .spec = normalizeTrackPresentationSpec(customSpec),
    };

    {
      auto ownerPtr = makeStateOnlyRuntime(tempDir);
      auto& runtime = ownerPtr->runtime();
      auto const firstListId = createList(*ownerPtr, "First snapshot");
      auto const secondListId = createList(*ownerPtr, "Second snapshot");
      REQUIRE(runtime.workspace().navigate({.target = firstListId}));
      REQUIRE(runtime.workspace().navigate({.target = secondListId}));
      REQUIRE(runtime.workspace().addCustomPreset(expectedPreset));
      runtime.workspace().saveSession(runtime.workspaceConfigStore());
    }

    auto ownerPtr = makeStateOnlyRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
    std::int32_t changeCount = 0;
    auto changed = WorkspaceChanged{};
    auto const sub = runtime.workspace().onChanged(
      [&](WorkspaceChanged const& value) noexcept
      {
        ++changeCount;
        changed = value;
      });

    REQUIRE(runtime.workspace().restoreSession(runtime.workspaceConfigStore()));
    runRuntimeTask(runtime, runtime.async().resumeOnCallbackExecutorAsync());

    CHECK(changeCount == 1);
    CHECK(changed.cause == WorkspaceChangeCause::Restore);
    CHECK(changed.snapshot.openViews.size() == 2);
    CHECK(changed.snapshot == runtime.workspace().snapshot());
    CHECK(changed.snapshot.revision == 1);
    REQUIRE(changed.snapshot.customPresets.size() == 1);
    CHECK(changed.snapshot.customPresets[0] == expectedPreset);
    auto const restoredPresets = runtime.workspace().customPresets();
    REQUIRE(restoredPresets.size() == 1);
    CHECK(restoredPresets[0] == expectedPreset);
  }

  TEST_CASE("WorkspaceService - saveSession tolerates flush failures")
  {
    auto tempDir = TempDir{};
    auto ownerPtr = makeStateOnlyRuntime(tempDir);

    auto badConfigStorePtr = std::make_shared<ConfigStore>(tempDir.path());
    CHECK_NOTHROW(ownerPtr->runtime().workspace().saveSession(*badConfigStorePtr));
  }

  TEST_CASE("WorkspaceService - restoreSession tolerates malformed config")
  {
    auto tempDir = TempDir{};
    auto ownerPtr = makeStateOnlyRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();

    auto const configPath = tempDir.path() / "bad.yaml";
    std::ofstream{configPath} << "workspace: \"not a map\"";

    auto badConfigStorePtr = std::make_shared<ConfigStore>(configPath, ConfigStore::OpenMode::ReadOnly);
    auto const res = runtime.workspace().restoreSession(*badConfigStorePtr);
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::FormatRejected);
  }

  TEST_CASE("WorkspaceService - out-of-bounds active index rejects before mutation")
  {
    auto tempDir = TempDir{};
    auto ownerPtr = makeStateOnlyRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();

    auto const existingListId = createList(*ownerPtr, "Existing");
    auto const storedListId = createList(*ownerPtr, "Stored");
    auto const nextListId = createList(*ownerPtr, "Next");
    auto const existingViewId = ao::test::requireValue(runtime.workspace().navigate({.target = existingListId}));
    auto customSpec = builtinTrackPresentationPreset(BuiltinPreset::Songs).spec;
    customSpec.presetId = CustomPresetId{1};
    REQUIRE(runtime.workspace().addCustomPreset(
      CustomTrackPresentationPreset{.label = "Keep", .optBasePresetId = BuiltinPreset::Songs, .spec = customSpec}));
    settleRuntimeCallbacks(runtime);
    auto const beforeSnapshot = runtime.workspace().snapshot();
    auto const configPath = tempDir.path() / "config.yaml";
    writeWorkspaceConfig(configPath, libraryIdOf(runtime), {storedListId.raw()}, 1);
    std::int32_t changeCount = 0;
    auto const sub = runtime.workspace().onChanged([&](WorkspaceChanged const&) noexcept { ++changeCount; });
    auto destroyed = std::vector<ViewService::ViewDestroyed>{};
    auto const destroyedSub = runtime.views().onViewDestroyed([&](ViewService::ViewDestroyed const& event) noexcept
                                                              { destroyed.push_back(event); });

    auto storePtr = std::make_shared<ConfigStore>(configPath, ConfigStore::OpenMode::ReadOnly);
    auto const res = runtime.workspace().restoreSession(*storePtr);

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::FormatRejected);
    settleRuntimeCallbacks(runtime);
    CHECK(runtime.workspace().snapshot() == beforeSnapshot);
    CHECK(runtime.workspace().customPresets().size() == 1);
    CHECK(changeCount == 0);
    CHECK(destroyed.empty());

    auto const nextViewId = ao::test::requireValue(runtime.workspace().navigate({.target = nextListId}));
    CHECK(existingViewId == ViewId{1});
    CHECK(nextViewId == ViewId{2});
    REQUIRE(runtime.workspace().goBack());
    CHECK(runtime.workspace().snapshot().activeViewId == existingViewId);
  }

  TEST_CASE("WorkspaceService - empty session preserves empty focus and selects an existing front view")
  {
    auto tempDir = TempDir{};
    auto ownerPtr = makeStateOnlyRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
    auto const configPath = tempDir.path() / "empty.yaml";
    writeWorkspaceConfig(configPath, libraryIdOf(runtime), {}, 0);
    auto store = ConfigStore{configPath, ConfigStore::OpenMode::ReadOnly};

    SECTION("Empty workspace remains unfocused")
    {
      std::int32_t changeCount = 0;
      auto const sub = runtime.workspace().onChanged([&](WorkspaceChanged const&) noexcept { ++changeCount; });

      REQUIRE(runtime.workspace().restoreSession(store));
      settleRuntimeCallbacks(runtime);
      CHECK(runtime.workspace().snapshot().openViews.empty());
      CHECK(runtime.workspace().snapshot().activeViewId == kInvalidViewId);
      CHECK(runtime.workspace().snapshot().revision == 0);
      CHECK(changeCount == 0);
    }

    SECTION("Existing workspace selects its first view")
    {
      auto const firstListId = createList(*ownerPtr, "First existing");
      auto const secondListId = createList(*ownerPtr, "Second existing");
      auto const firstViewId = ao::test::requireValue(runtime.workspace().navigate({.target = firstListId}));
      REQUIRE(runtime.workspace().navigate({.target = secondListId}));
      settleRuntimeCallbacks(runtime);
      auto const before = runtime.workspace().snapshot();
      auto changes = std::vector<WorkspaceChanged>{};
      auto const sub =
        runtime.workspace().onChanged([&](WorkspaceChanged const& changed) noexcept { changes.push_back(changed); });

      REQUIRE(runtime.workspace().restoreSession(store));
      settleRuntimeCallbacks(runtime);
      auto const restored = runtime.workspace().snapshot();
      CHECK(restored.activeViewId == firstViewId);
      CHECK(restored.openViews == before.openViews);
      CHECK(restored.revision == before.revision + 1);
      REQUIRE(changes.size() == 1);
      CHECK(changes[0].cause == WorkspaceChangeCause::Restore);
      CHECK(changes[0].snapshot == restored);
    }
  }

  TEST_CASE("WorkspaceService - failed session restore leaves workspace views focus and history unchanged")
  {
    auto tempDir = TempDir{};
    auto ownerPtr = makeStateOnlyRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
    auto const listId = createList(*ownerPtr, "Valid");
    auto const configPath = tempDir.path() / "partial.yaml";

    writeWorkspaceConfig(configPath, libraryIdOf(runtime), {listId.raw(), 999999}, 0);

    auto destroyed = std::vector<ViewService::ViewDestroyed>{};
    auto const destroyedSub = runtime.views().onViewDestroyed([&](ViewService::ViewDestroyed const& event) noexcept
                                                              { destroyed.push_back(event); });
    std::int32_t changeCount = 0;
    auto const workspaceSub = runtime.workspace().onChanged([&](WorkspaceChanged const&) noexcept { ++changeCount; });
    auto storePtr = std::make_shared<ConfigStore>(configPath, ConfigStore::OpenMode::ReadOnly);
    auto const res = runtime.workspace().restoreSession(*storePtr);

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotFound);
    settleRuntimeCallbacks(runtime);
    auto const layout = runtime.workspace().snapshot();
    CHECK(layout.openViews.empty());
    CHECK(layout.activeViewId == kInvalidViewId);
    CHECK(layout.revision == 0);
    CHECK(changeCount == 0);
    REQUIRE(destroyed.size() == 1);
    CHECK(destroyed[0].viewId == ViewId{1});
    auto const candidateStateRes = runtime.views().findTrackListState(ViewId{1});
    auto const candidateProjectionRes = runtime.views().findTrackListProjection(ViewId{1});
    REQUIRE_FALSE(candidateStateRes);
    CHECK(candidateStateRes.error().code == Error::Code::NotFound);
    REQUIRE_FALSE(candidateProjectionRes);
    CHECK(candidateProjectionRes.error().code == Error::Code::NotFound);

    auto const nextViewId = ao::test::requireValue(runtime.workspace().navigate({.target = listId}));
    CHECK(nextViewId == ViewId{2});
    auto const nextStateRes = runtime.views().findTrackListState(nextViewId);
    REQUIRE(nextStateRes);
    CHECK(nextStateRes->listId == listId);
    auto const backRes = runtime.workspace().goBack();
    REQUIRE_FALSE(backRes);
    CHECK(backRes.error().code == Error::Code::NotFound);
  }

  TEST_CASE("WorkspaceService - reentrant navigation during failed restore does not adopt rollback views")
  {
    auto tempDir = TempDir{};
    auto ownerPtr = makeStateOnlyRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
    auto& workspace = runtime.workspace();
    auto& views = runtime.views();
    auto const targetListId = createList(*ownerPtr, "Reentrant target");
    auto const departureListId = createList(*ownerPtr, "Already open");
    auto const oldTargetViewId = requireValue(workspace.navigate({.target = targetListId}));
    auto const departureViewId = requireValue(workspace.navigate({.target = departureListId}));
    REQUIRE(workspace.closeView(oldTargetViewId));
    REQUIRE(workspace.canGoBack());
    REQUIRE_FALSE(workspace.canGoForward());
    auto const before = workspace.snapshot();
    auto const configPath = tempDir.path() / "partial.yaml";
    // Whichever candidate is destroyed first, the other still matches the navigation target.
    writeWorkspaceConfig(configPath, libraryIdOf(runtime), {targetListId.raw(), targetListId.raw(), 999999}, 0);
    auto store = ConfigStore{configPath, ConfigStore::OpenMode::ReadOnly};
    auto const replayHistory = GENERATE(false, true);
    CAPTURE(replayHistory);
    auto destroyed = std::vector<ViewId>{};
    auto duringRollback = WorkspaceSnapshot{};
    bool navigated = false;
    auto const sub = views.onViewDestroyed(
      [&](ViewService::ViewDestroyed const& event)
      {
        destroyed.push_back(event.viewId);

        if (destroyed.size() == 1)
        {
          duringRollback = workspace.snapshot();
          navigated =
            replayHistory ? workspace.goBack().has_value() : workspace.navigate({.target = targetListId}).has_value();
        }
      });

    auto const restoreRes = workspace.restoreSession(store);

    REQUIRE_FALSE(restoreRes);
    CHECK(restoreRes.error().code == Error::Code::NotFound);
    REQUIRE(destroyed.size() == 2);
    CHECK(duringRollback == before);
    REQUIRE(navigated);
    auto const after = workspace.snapshot();
    CHECK_FALSE(std::ranges::contains(destroyed, after.activeViewId));
    CHECK(after.openViews == std::vector{departureViewId, after.activeViewId});
    CHECK(after.revision == before.revision + 1);
    auto const stateRes = views.findTrackListState(after.activeViewId);
    REQUIRE(stateRes);
    CHECK(stateRes->listId == targetListId);

    if (replayHistory)
    {
      CHECK_FALSE(workspace.canGoBack());
      REQUIRE(workspace.goForward());
    }
    else
    {
      CHECK_FALSE(workspace.canGoForward());
      REQUIRE(workspace.goBack());
    }

    CHECK(workspace.snapshot().activeViewId == departureViewId);
  }

  TEST_CASE("WorkspaceService - restore rejects unsupported or unknown presentation vocabulary")
  {
    auto tempDir = TempDir{};
    auto ownerPtr = makeStateOnlyRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
    auto const listId = createList(*ownerPtr, "Valid");
    auto const configPath = tempDir.path() / "versioned.yaml";
    auto expectedCode = Error::Code::FormatRejected;

    SECTION("Unsupported presentation version")
    {
      writeWorkspaceConfig(configPath, libraryIdOf(runtime), {listId.raw()}, 0, "none", 2);
      expectedCode = Error::Code::NotSupported;
    }

    SECTION("Unknown group id")
    {
      writeWorkspaceConfig(configPath, libraryIdOf(runtime), {listId.raw()}, 0, "future-group");
    }

    SECTION("Unknown root field")
    {
      writeWorkspaceConfig(configPath, libraryIdOf(runtime), {listId.raw()}, 0);
      std::ofstream{configPath, std::ios::app} << "  unexpected: true\n";
    }

    auto const before = runtime.workspace().snapshot();
    std::int32_t changeCount = 0;
    auto const sub = runtime.workspace().onChanged([&](WorkspaceChanged const&) noexcept { ++changeCount; });
    auto storePtr = std::make_shared<ConfigStore>(configPath, ConfigStore::OpenMode::ReadOnly);
    auto const res = runtime.workspace().restoreSession(*storePtr);

    REQUIRE_FALSE(res);
    CHECK(res.error().code == expectedCode);
    settleRuntimeCallbacks(runtime);
    CHECK(runtime.workspace().snapshot() == before);
    CHECK(changeCount == 0);
  }

  TEST_CASE("WorkspaceService - checkpointing a fallback view preserves a rejected session")
  {
    auto tempDir = TempDir{};
    auto ownerPtr = makeStateOnlyRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
    auto const path = tempDir.path() / "workspace.yaml";
    auto const version = GENERATE(1U, 3U);
    writeWorkspaceConfig(path, libraryIdOf(runtime), {kAllTracksListId.raw()}, 0, "unknown-group", version);
    std::ofstream{path, std::ios::app} << "sibling: retained\n";
    auto const original = readFile(path);
    auto store = ConfigStore{path};
    auto& workspace = runtime.workspace();

    auto const restoredRes = workspace.restoreSession(store);
    REQUIRE_FALSE(restoredRes);
    REQUIRE(workspace.navigate({.target = GlobalViewKind::AllTracks}));
    workspace.saveSession(store);
    CHECK(readFile(path) == original);

    // A write to a different group still commits and leaves the rejected payload intact.
    REQUIRE(store.removeGroup("sibling"));
    auto afterSiblingWrite = ConfigStore{path};
    CHECK_FALSE(requireValue(afterSiblingWrite.contains("sibling")));
    auto const stillRejectedRes = workspace.restoreSession(afterSiblingWrite);
    REQUIRE_FALSE(stillRejectedRes);
    CHECK(stillRejectedRes.error().code == restoredRes.error().code);

    // Explicitly discarding just this group permits a fresh checkpoint on the same store.
    REQUIRE(store.removeGroup("workspace"));
    workspace.saveSession(store);
    auto replacement = ConfigStore{path};
    REQUIRE(requireValue(replacement.contains("workspace")));
    REQUIRE(workspace.restoreSession(replacement));
  }

  TEST_CASE("WorkspaceService - restore rejects the unversioned numeric presentation format")
  {
    auto tempDir = TempDir{};
    auto ownerPtr = makeStateOnlyRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
    auto const configPath = tempDir.path() / "unversioned.yaml";
    std::ofstream{configPath} << "workspace:\n"
                                 "  openViews: []\n"
                                 "  activeViewIndex: 0\n"
                                 "  customPresets: []\n";

    auto storePtr = std::make_shared<ConfigStore>(configPath, ConfigStore::OpenMode::ReadOnly);
    auto const res = runtime.workspace().restoreSession(*storePtr);

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::FormatRejected);
    CHECK(runtime.workspace().snapshot().revision == 0);
  }
} // namespace ao::rt::test
