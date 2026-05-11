// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include "TestUtils.h"
#include <runtime/ConfigStore.h>
#include <runtime/LibraryMutationService.h>
#include <runtime/ListSourceStore.h>
#include <runtime/PlaybackService.h>
#include <runtime/StateTypes.h>
#include <runtime/TrackDetailProjection.h>
#include <runtime/ViewService.h>
#include <runtime/WorkspaceService.h>

#include <ao/library/TrackBuilder.h>

#include <limits>

using namespace ao::rt;
using namespace ao::rt::test;

namespace
{
  class MockControlExecutor final : public IControlExecutor
  {
  public:
    bool isCurrent() const noexcept override { return true; }
    void dispatch(std::move_only_function<void()> task) override { task(); }
  };

  struct Env final
  {
    TestMusicLibrary lib;
    MockControlExecutor executor;
    ao::rt::LibraryMutationService mutation;
    ao::rt::ListSourceStore sources;
    ao::rt::ViewService views;
    std::shared_ptr<ao::rt::ConfigStore> config;
    ao::rt::PlaybackService playback;
    ao::rt::WorkspaceService workspace;

    Env()
      : mutation{executor, lib.library()}
      , sources{lib.library(), mutation}
      , views{lib.library(), sources}
      , config{std::make_shared<ao::rt::ConfigStore>(lib.library().rootPath() / "config.json")}
      , playback{executor, views, lib.library()}
      , workspace{views, playback, mutation, lib.library(), config}
    {
      views.setWorkspaceService(workspace);
      views.setLibraryMutationService(mutation);
    }
  };

  ao::ListId allTracksId()
  {
    return ao::ListId{std::numeric_limits<std::uint32_t>::max()};
  }
}

TEST_CASE("TrackDetailProjection refreshes on TracksMutated", "[projection]")
{
  Env env;

  auto const id1 = env.lib.addTrack(TrackSpec{.title = "Before", .artist = "ArtistA", .album = "AlbumX"});

  auto const reply = env.views.createView(TrackListViewConfig{.listId = allTracksId()});
  env.views.setSelection(reply.viewId, {id1});

  auto proj = env.views.detailProjection(ExplicitViewTarget{reply.viewId});

  auto snap = proj->snapshot();
  REQUIRE(snap.selectionKind == SelectionKind::Single);
  REQUIRE(snap.title.optValue.has_value());
  CHECK(snap.title.optValue.value() == "Before");

  // Mutate the track in the library using the service
  {
    MetadataPatch patch{.optTitle = "After"};
    env.mutation.updateMetadata({id1}, patch);
  }

  // Mutation service already published the signal

  snap = proj->snapshot();
  CHECK(snap.title.optValue.value() == "After");
}

TEST_CASE("TrackDetailProjection ignores non-intersecting TracksMutated", "[projection]")
{
  Env env;

  auto const id1 = env.lib.addTrack("Selected");
  auto const id2 = env.lib.addTrack("Other");

  auto const reply = env.views.createView(TrackListViewConfig{.listId = allTracksId()});
  env.views.setSelection(reply.viewId, {id1});

  auto proj = env.views.detailProjection(ExplicitViewTarget{reply.viewId});

  auto const revBefore = proj->snapshot().revision;

  // Mutate a track not in the selection
  env.mutation.updateMetadata({id2}, MetadataPatch{.optTitle = "Something Else"});

  // Revision should NOT change because the mutated track is not selected
  CHECK(proj->snapshot().revision == revBefore);
}

TEST_CASE("TrackDetailProjection aggregates metadata for multi-select", "[projection]")
{
  Env env;

  auto const id1 = env.lib.addTrack(TrackSpec{.title = "Song A", .artist = "Same", .album = "AlbumX"});
  auto const id2 = env.lib.addTrack(TrackSpec{.title = "Song B", .artist = "Same", .album = "AlbumY"});

  auto const reply = env.views.createView(TrackListViewConfig{.listId = allTracksId()});
  env.views.setSelection(reply.viewId, {id1, id2});

  auto proj = env.views.detailProjection(ExplicitViewTarget{reply.viewId});
  auto const snap = proj->snapshot();

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
