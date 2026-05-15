// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include "TestUtils.h"
#include <runtime/ConfigStore.h>
#include <runtime/LibraryMutationService.h>
#include <runtime/ListSourceStore.h>
#include <runtime/PlaybackService.h>
#include <runtime/ProjectionTypes.h>
#include <runtime/StateTypes.h>
#include <runtime/TrackDetailProjection.h>
#include <runtime/ViewService.h>
#include <runtime/WorkspaceService.h>

#include <ao/Type.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>

namespace ao::rt::test
{
  namespace
  {
    class MockControlExecutor final : public IControlExecutor
    {
    public:
      bool isCurrent() const noexcept override { return true; }
      void dispatch(std::move_only_function<void()> task) override { task(); }
      void defer(std::move_only_function<void()> task) override { task(); }
    };

    struct Env final
    {
      TestMusicLibrary lib;
      MockControlExecutor executor;
      LibraryMutationService mutation;
      ListSourceStore sources;
      ViewService views;
      std::shared_ptr<ConfigStore> config;
      PlaybackService playback;
      WorkspaceService workspace;

      Env()
        : mutation{executor, lib.library()}
        , sources{lib.library(), mutation}
        , views{executor, lib.library(), sources}
        , config{std::make_shared<ConfigStore>(lib.library().rootPath() / "config.json")}
        , playback{executor, views, lib.library()}
        , workspace{views, playback, mutation, lib.library(), config}
      {
      }
    };

    ListId allTracksId()
    {
      return ListId{std::numeric_limits<std::uint32_t>::max()};
    }
  }

  TEST_CASE("TrackDetailProjection refreshes on TracksMutated", "[projection]")
  {
    auto env = Env{};

    auto const id1 = env.lib.addTrack(TrackSpec{.title = "Before", .artist = "ArtistA", .album = "AlbumX"});

    auto const reply = env.views.createView(TrackListViewConfig{.listId = allTracksId()});
    env.views.setSelection(reply.viewId, {id1});

    auto proj = env.views.detailProjection(ExplicitViewTarget{reply.viewId}, env.workspace, env.mutation);

    auto snap = proj->snapshot();
    REQUIRE(snap.selectionKind == SelectionKind::Single);
    REQUIRE(snap.title.optValue.has_value());
    CHECK(snap.title.optValue.value() == "Before");

    // Mutate the track in the library using the service
    {
      auto const patch = MetadataPatch{.optTitle = "After"};
      env.mutation.updateMetadata({id1}, patch);
    }

    // Mutation service already published the signal

    snap = proj->snapshot();
    CHECK(snap.title.optValue.value() == "After");
  }

  TEST_CASE("TrackDetailProjection ignores non-intersecting TracksMutated", "[projection]")
  {
    auto env = Env{};

    auto const id1 = env.lib.addTrack("Selected");
    auto const id2 = env.lib.addTrack("Other");

    auto const reply = env.views.createView(TrackListViewConfig{.listId = allTracksId()});
    env.views.setSelection(reply.viewId, {id1});

    auto proj = env.views.detailProjection(ExplicitViewTarget{reply.viewId}, env.workspace, env.mutation);

    auto const revBefore = proj->snapshot().revision;

    // Mutate a track not in the selection
    env.mutation.updateMetadata({id2}, MetadataPatch{.optTitle = "Something Else"});

    // Revision should NOT change because the mutated track is not selected
    CHECK(proj->snapshot().revision == revBefore);
  }

  TEST_CASE("TrackDetailProjection aggregates metadata for multi-select", "[projection]")
  {
    auto env = Env{};

    auto const id1 = env.lib.addTrack(TrackSpec{.title = "Song A", .artist = "Same", .album = "AlbumX"});
    auto const id2 = env.lib.addTrack(TrackSpec{.title = "Song B", .artist = "Same", .album = "AlbumY"});

    auto const reply = env.views.createView(TrackListViewConfig{.listId = allTracksId()});
    env.views.setSelection(reply.viewId, {id1, id2});

    auto const proj = env.views.detailProjection(ExplicitViewTarget{reply.viewId}, env.workspace, env.mutation);
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
} // namespace ao::rt::test
