// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestUtils.h"
#include <ao/Type.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ListSourceStore.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ProjectionTypes.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackDetailProjection.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/async/Runtime.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct Env final
    {
      TestMusicLibrary lib;
      MockExecutor executor;
      async::Runtime runtime;
      LibraryMutationService mutation;
      ListSourceStore sources;
      ViewService views;
      ConfigStore config;
      PlaybackService playback;
      WorkspaceService workspace;

      Env()
        : runtime{executor}
        , mutation{runtime, lib.library()}
        , sources{lib.library(), mutation}
        , views{executor, lib.library(), sources}
        , config{lib.library().rootPath() / "config.json"}
        , playback{executor, views, lib.library()}
        , workspace{views, playback, mutation, lib.library()}
      {
      }
    };
  }

  TEST_CASE("TrackDetailProjection refreshes on TracksMutated", "[projection][unit]")
  {
    auto env = Env{};

    auto const id1 = env.lib.addTrack(TrackSpec{.title = "Before", .artist = "ArtistA", .album = "AlbumX"});

    auto const reply = env.views.createView(TrackListViewConfig{.listId = kAllTracksListId});
    env.views.setSelection(reply.viewId, {id1});

    auto projPtr = env.views.detailProjection(ExplicitViewTarget{reply.viewId}, env.workspace, env.mutation);

    auto snap = projPtr->snapshot();
    REQUIRE(snap.selectionKind == SelectionKind::Single);
    REQUIRE(snap.title.optValue.has_value());
    CHECK(snap.title.optValue.value() == "Before");

    // Mutate the track in the library using the service
    {
      auto const patch = MetadataPatch{.optTitle = "After"};
      auto const targetIds = std::array{id1};
      env.mutation.updateMetadata(targetIds, patch);
    }

    // Mutation service already published the signal

    snap = projPtr->snapshot();
    CHECK(snap.title.optValue.value() == "After");
  }

  TEST_CASE("TrackDetailProjection ignores non-intersecting TracksMutated", "[projection][unit]")
  {
    auto env = Env{};

    auto const id1 = env.lib.addTrack("Selected");
    auto const id2 = env.lib.addTrack("Other");

    auto const reply = env.views.createView(TrackListViewConfig{.listId = kAllTracksListId});
    env.views.setSelection(reply.viewId, {id1});

    auto projPtr = env.views.detailProjection(ExplicitViewTarget{reply.viewId}, env.workspace, env.mutation);

    auto const revBefore = projPtr->snapshot().revision;

    // Mutate a track not in the selection
    auto const otherIds = std::array{id2};
    env.mutation.updateMetadata(otherIds, MetadataPatch{.optTitle = "Something Else"});

    // Revision should NOT change because the mutated track is not selected
    CHECK(projPtr->snapshot().revision == revBefore);
  }

  TEST_CASE("TrackDetailProjection aggregates metadata for multi-select", "[projection][unit]")
  {
    auto env = Env{};

    auto const id1 = env.lib.addTrack(TrackSpec{.title = "Song A", .artist = "Same", .album = "AlbumX"});
    auto const id2 = env.lib.addTrack(TrackSpec{.title = "Song B", .artist = "Same", .album = "AlbumY"});

    auto const reply = env.views.createView(TrackListViewConfig{.listId = kAllTracksListId});
    env.views.setSelection(reply.viewId, {id1, id2});

    auto const projPtr = env.views.detailProjection(ExplicitViewTarget{reply.viewId}, env.workspace, env.mutation);
    auto const snap = projPtr->snapshot();

    REQUIRE(snap.selectionKind == SelectionKind::Multiple);

    // Titles differ
    CHECK(snap.title.mixed);

    // Artists are the same
    CHECK_FALSE(snap.artist.mixed);
    REQUIRE(snap.artist.optValue.has_value());
    CHECK(snap.artist.optValue.value() == "Same");

    // Albums differ
    CHECK(snap.album.mixed);
  }

  TEST_CASE("TrackDetailProjection with ExplicitSelectionTarget", "[projection][unit]")
  {
    auto env = Env{};
    auto const id1 = env.lib.addTrack(TrackSpec{.title = "Song A"});
    auto const projPtr =
      env.views.detailProjection(ExplicitSelectionTarget{std::vector{id1}}, env.workspace, env.mutation);
    auto const snap = projPtr->snapshot();
    REQUIRE(snap.selectionKind == SelectionKind::Single);
    CHECK(snap.title.optValue.value() == "Song A");
  }

  TEST_CASE("TrackDetailProjection with FocusedViewTarget", "[projection][unit]")
  {
    auto env = Env{};
    auto const id1 = env.lib.addTrack(TrackSpec{.title = "Song A"});
    auto const id2 = env.lib.addTrack(TrackSpec{.title = "Song B"});

    auto const projPtr = env.views.detailProjection(FocusedViewTarget{}, env.workspace, env.mutation);

    // Subscribe to verify notifications
    std::int32_t callCount = 0;
    auto sub = projPtr->subscribe([&](TrackDetailSnapshot const&) { callCount++; });
    CHECK(callCount == 1); // Called immediately

    auto const reply1 = env.views.createView(TrackListViewConfig{.listId = kAllTracksListId});
    env.views.setSelection(reply1.viewId, {id1});
    env.workspace.navigateTo(GlobalViewKind::AllTracks); // Should trigger onFocusedViewChanged

    CHECK(callCount >= 2);
    CHECK(projPtr->snapshot().title.optValue.value() == "Song A");

    // Change selection in the focused view
    env.views.setSelection(reply1.viewId, {id2});
    CHECK(projPtr->snapshot().title.optValue.value() == "Song B");

    // Change focus away
    env.workspace.closeView(reply1.viewId);
    // Unsubscribe
    sub = {};

    // Now trigger a selection change in the old view, should NOT update because it's no longer focused
    env.views.setSelection(reply1.viewId, {id1});
    CHECK(projPtr->snapshot().title.optValue.value() == "Song B");
  }

  TEST_CASE("TrackDetailProjection with non-existent track", "[projection][unit]")
  {
    auto env = Env{};
    auto const projPtr =
      env.views.detailProjection(ExplicitSelectionTarget{std::vector{TrackId{9999}}}, env.workspace, env.mutation);
    auto const snap = projPtr->snapshot();
    REQUIRE(snap.selectionKind == SelectionKind::Single);
    CHECK_FALSE(snap.title.optValue.has_value());
  }

  TEST_CASE("TrackDetailProjection with tags", "[projection][unit]")
  {
    auto env = Env{};
    auto const id1 = env.lib.addTrack(TrackSpec{.title = "Song A"});

    // Add tag
    auto const targetIds = std::vector{id1};
    auto const tagsToAdd = std::vector<std::string>{"MyTag"};
    env.mutation.editTags(targetIds, tagsToAdd, {});

    auto const projPtr =
      env.views.detailProjection(ExplicitSelectionTarget{std::vector{id1}}, env.workspace, env.mutation);
    auto const snap = projPtr->snapshot();
    REQUIRE(snap.selectionKind == SelectionKind::Single);
    CHECK(snap.commonTagIds.size() == 1);
  }
} // namespace ao::rt::test
