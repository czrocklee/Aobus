// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include "TestUtils.h"
#include <runtime/EventBus.h>
#include <runtime/EventTypes.h>
#include <runtime/ListSourceStore.h>
#include <runtime/StateTypes.h>
#include <runtime/TrackDetailProjection.h>
#include <runtime/ViewService.h>

#include <ao/library/TrackBuilder.h>

#include <limits>

using namespace ao::app;
using namespace ao::app::test;

namespace
{
  struct Env final
  {
    TestMusicLibrary lib;
    EventBus events;
    ListSourceStore sources{lib.library(), events};
    ViewService views{lib.library(), sources, events};
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

  // Mutate the track in the library
  {
    auto txn = env.lib.library().writeTransaction();
    auto writer = env.lib.library().tracks().writer(txn);
    auto reader = env.lib.library().tracks().reader(txn);
    auto const optView = reader.get(id1, ao::library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView.has_value());
    auto builder = ao::library::TrackBuilder::fromView(*optView, env.lib.library().dictionary());
    builder.metadata().title("After");
    auto hot = builder.serializeHot(txn, env.lib.library().dictionary());
    auto cold = builder.serializeCold(txn, env.lib.library().dictionary(), env.lib.library().resources());
    writer.updateHot(id1, hot);
    writer.updateCold(id1, cold);
    txn.commit();
  }

  // Publish TracksMutated — projection should refresh
  env.events.publish(TracksMutated{.trackIds = {id1}});

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
  env.events.publish(TracksMutated{.trackIds = {id2}});

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
