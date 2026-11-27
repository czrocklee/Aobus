// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/PlaybackSessionState.h"
#include "runtime/PlaybackSessionYamlSchema.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibraryImportPlan.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/library/LibraryTransfer.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutYamlSchema.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace ao::rt::test
{
  namespace
  {
    TrackId addFileTrack(AppRuntimeOwner& owner, std::string const& title, std::string_view const fileName)
    {
      auto const uri = audio::test::installAudioFixture(owner.runtime().musicRoot(), "basic_metadata.flac", fileName);
      return addRuntimeTrack(owner, library::test::TrackSpec{.title = title, .uri = uri});
    }

    ListId createList(AppRuntimeOwner& owner, std::string name)
    {
      return ao::test::requireValue(
        runRuntimeTask(owner.runtime(), owner.libraryCommands().createListAsync(ListDraft{.name = std::move(name)})));
    }

    void restoreLibrary(AppRuntimeOwner& owner, std::filesystem::path const& yamlPath)
    {
      auto& jobs = owner.libraryJobs();
      auto planRes = runRuntimeTask(owner.runtime(), jobs.prepareLibraryImportAsync(yamlPath, ImportMode::Restore));
      REQUIRE(planRes);
      REQUIRE(runRuntimeTask(owner.runtime(), jobs.applyLibraryImportPlanAsync(std::move(*planRes))));
    }
  } // namespace

  // Application state outlives a library restore: each frontend keeps its files
  // under the music root, and GTK keeps the playback session in global config.
  // Every object this state names is in the backup, so the restore recreates it,
  // under a new id in the same database or under numbers a new database issues
  // afresh, and no saved reference may bind to it. A saved Track that a backup
  // lacks is an ordinary deletion instead and may select a replacement by
  // anchor; the playback restore-matrix tests own that case.
  TEST_CASE("AppRuntime - state saved before a library restore binds to no object the restore recreated")
  {
    auto tempDir = ao::test::TempDir{};
    auto backupDir = ao::test::TempDir{};
    auto const yamlPath = backupDir.path() / "backup.yaml";
    auto const layoutPath = tempDir.path() / "layout.yaml";
    auto savedListId = kInvalidListId;
    auto savedTrackId = kInvalidTrackId;
    auto savedTrackUri = std::string{};

    {
      auto ownerPtr = makeStateOnlyRuntime(tempDir);
      auto& runtime = ownerPtr->runtime();
      addReadyAudioProvider(runtime);
      std::ignore = addFileTrack(*ownerPtr, "Alpha", "alpha.flac");
      savedTrackId = addFileTrack(*ownerPtr, "Beta", "beta.flac");
      savedListId = createList(*ownerPtr, "Saved list");
      runtime.sources().reloadAllTracks();
      auto const reader = runtime.library().snapshot();
      auto const libraryId = reader.libraryId();
      savedTrackUri = reader.trackUri(savedTrackId).value();

      // A restored session that is saved again goes through the production save path.
      REQUIRE(runtime.workspaceConfigStore().save(kPlaybackSessionConfigGroup,
                                                  PlaybackSessionState{
                                                    .libraryId = libraryId,
                                                    .sourceListId = kAllTracksListId,
                                                    .currentTrackId = savedTrackId,
                                                    .currentTrackUri = savedTrackUri,
                                                    .anchorIndex = 1,
                                                    .positionMs = 400,
                                                  },
                                                  PlaybackSessionYamlSchema{}));
      auto const restoredRes = runtime.restorePlaybackSession();
      REQUIRE(restoredRes);
      REQUIRE(restoredRes->restored);
      REQUIRE(runtime.savePlaybackSession());

      REQUIRE(runtime.workspace().navigate({.target = savedListId}));
      auto customSpec = builtinTrackPresentationPreset(BuiltinPreset::Songs).spec;
      customSpec.presetId = CustomPresetId{42};
      REQUIRE(runtime.workspace().addCustomPreset(
        CustomTrackPresentationPreset{.label = "Kept", .optBasePresetId = BuiltinPreset::Songs, .spec = customSpec}));
      runtime.workspace().saveSession(runtime.workspaceConfigStore());

      auto layoutStore = ConfigStore{layoutPath};
      REQUIRE(layoutStore.saveTogether(
        configWrite(uimodel::kTrackColumnLayoutsConfigGroup,
                    uimodel::TrackColumnLayouts::Snapshot{
                      {savedListId, {uimodel::TrackColumnState{.field = TrackField::Duration, .width = 90}}},
                      {kAllTracksListId, {uimodel::TrackColumnState{.field = TrackField::Duration, .width = 80}}},
                    },
                    uimodel::TrackColumnLayoutYamlSchema{libraryId}),
        configWrite(uimodel::kListPresentationsConfigGroup,
                    uimodel::ListPresentations::Snapshot{
                      {savedListId, BuiltinPreset::Albums}, {kAllTracksListId, BuiltinPreset::Songs}},
                    uimodel::ListPresentationPreferenceYamlSchema{libraryId})));

      REQUIRE(runRuntimeTask(runtime, ownerPtr->libraryJobs().exportLibraryAsync(yamlPath, ExportMode::Full)));
    }

    bool newDatabase = false;

    SECTION("restore into the same database")
    {
      newDatabase = false;
    }

    SECTION("restore into a new database under the same music root")
    {
      newDatabase = true;
      std::filesystem::remove_all(LibraryPaths{tempDir.path()}.databasePath());
    }

    {
      auto ownerPtr = makeStateOnlyRuntime(tempDir);
      restoreLibrary(*ownerPtr, yamlPath);
    }

    auto ownerPtr = makeStateOnlyRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
    addReadyAudioProvider(runtime);
    runtime.sources().reloadAllTracks();
    auto const reader = runtime.library().snapshot();
    // The restore recreated both tracks and the list from the backup. A new
    // database numbers them from 1 again, so the saved ids name live objects.
    REQUIRE(reader.lists().size() == 1);
    REQUIRE(reader.containsTrackUri(savedTrackUri));
    CHECK(reader.trackUri(savedTrackId).has_value() == newDatabase);
    CHECK(reader.listNode(savedListId).has_value() == newDatabase);

    auto const playbackRes = runtime.restorePlaybackSession();
    REQUIRE(playbackRes);
    CHECK_FALSE(playbackRes->restored);

    auto const workspaceRes = runtime.workspace().restoreSession(runtime.workspaceConfigStore());

    if (newDatabase)
    {
      // Another library's list ids are dropped while its custom presets remain.
      REQUIRE(workspaceRes);
      CHECK(runtime.workspace().customPresets().size() == 1);
    }
    else
    {
      // The same library's ids are never reused, so the saved view finds no list.
      REQUIRE_FALSE(workspaceRes);
      CHECK(workspaceRes.error().code == Error::Code::NotFound);
    }

    CHECK(runtime.workspace().snapshot().openViews.empty());

    auto layoutStore = ConfigStore{layoutPath, ConfigStore::OpenMode::ReadOnly};
    auto layouts = uimodel::TrackColumnLayouts::Snapshot{};
    auto presentations = uimodel::ListPresentations::Snapshot{};
    REQUIRE(layoutStore.load(
      uimodel::kTrackColumnLayoutsConfigGroup, layouts, uimodel::TrackColumnLayoutYamlSchema{reader.libraryId()}));
    REQUIRE(layoutStore.load(uimodel::kListPresentationsConfigGroup,
                             presentations,
                             uimodel::ListPresentationPreferenceYamlSchema{reader.libraryId()}));
    CHECK(layouts.contains(kAllTracksListId));
    CHECK(presentations.contains(kAllTracksListId));

    // Every surviving real-list entry names a list the library does not have,
    // which the presentation stores retire on restore.
    for (auto const& [listId, columns] : layouts)
    {
      CHECK((isVirtualListId(listId) || !reader.listNode(listId)));
    }

    for (auto const& [listId, presentationId] : presentations)
    {
      CHECK((isVirtualListId(listId) || !reader.listNode(listId)));
    }

    CHECK((newDatabase ? layouts.size() == 1 : layouts.contains(savedListId)));
  }
} // namespace ao::rt::test
