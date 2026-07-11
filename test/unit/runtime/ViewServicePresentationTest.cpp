// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/runtime/ViewServiceTestSupport.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("ViewService - createView with groupBy applies effective sort", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();

    auto const result = env.requireView(service, {.groupBy = TrackGroupKey::Artist});
    auto const snap = service.trackListState(result.viewId);

    CHECK(snap.groupBy == TrackGroupKey::Artist);

    auto const expected = std::vector{TrackSortField::Artist,
                                      TrackSortField::Year,
                                      TrackSortField::Album,
                                      TrackSortField::DiscNumber,
                                      TrackSortField::TrackNumber,
                                      TrackSortField::Title};
    REQUIRE(snap.sortBy.size() == expected.size());

    for (std::size_t i = 0; i < expected.size(); ++i)
    {
      CHECK(snap.sortBy[i].field == expected[i]);
      CHECK(snap.sortBy[i].ascending == true);
    }
  }

  TEST_CASE("ViewService - createView with Album groupBy applies album sort", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();

    auto const result = env.requireView(service, {.groupBy = TrackGroupKey::Album});
    auto const snap = service.trackListState(result.viewId);

    CHECK(snap.groupBy == TrackGroupKey::Album);

    auto const expected = std::vector{TrackSortField::AlbumArtist,
                                      TrackSortField::Album,
                                      TrackSortField::DiscNumber,
                                      TrackSortField::TrackNumber,
                                      TrackSortField::Title};
    REQUIRE(snap.sortBy.size() == expected.size());

    for (std::size_t i = 0; i < expected.size(); ++i)
    {
      CHECK(snap.sortBy[i].field == expected[i]);
    }
  }

  TEST_CASE("ViewService - absent presentation defaults exact manual lists to List Order",
            "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();
    auto const manualListId = ao::test::requireValue(env.writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Manual order",
    }));

    auto const created = env.requireView(service, {.listId = manualListId});
    auto const state = service.trackListState(created.viewId);

    CHECK(state.presentation.id == kListOrderTrackPresentationId);
    CHECK(state.groupBy == TrackGroupKey::None);
    CHECK(state.sortBy.empty());
  }

  TEST_CASE("ViewService - explicit presentation wins over the manual List Order default",
            "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();
    auto const manualListId = ao::test::requireValue(env.writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Explicit order",
    }));
    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);

    auto const created = env.requireView(service, {.listId = manualListId, .optPresentation = albumsPreset->spec});
    auto const state = service.trackListState(created.viewId);

    CHECK(state.presentation.id == "albums");
    CHECK(state.groupBy == TrackGroupKey::Album);
    CHECK_FALSE(state.sortBy.empty());
  }

  TEST_CASE("ViewService - smart and All Tracks sources retain the normal default presentation",
            "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();
    auto const smartListId = ao::test::requireValue(env.writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Smart,
      .name = "Smart order",
      .expression = "true",
    }));

    auto const allTracks = env.requireView(service);
    auto const smart = env.requireView(service, {.listId = smartListId});

    CHECK(service.trackListState(allTracks.viewId).presentation.id == kDefaultTrackPresentationId);
    CHECK(service.trackListState(smart.viewId).presentation.id == kDefaultTrackPresentationId);
  }

  TEST_CASE("ViewService - playback launch capture contains exact list filter and sort only",
            "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();
    auto const* genresPreset = builtinTrackPresentationPreset("genres");
    REQUIRE(genresPreset != nullptr);
    auto const created =
      env.requireView(service, {.filterExpression = "$year > 2000", .optPresentation = genresPreset->spec});

    auto const captured = service.capturePlaybackLaunchContext(created.viewId);

    REQUIRE(captured);
    CHECK(captured->sourceListId == kAllTracksListId);
    CHECK(captured->quickFilterExpression == "$year > 2000");
    CHECK(captured->order.sortBy == genresPreset->spec.sortBy);

    auto const missing = service.capturePlaybackLaunchContext(ViewId{999999});
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == Error::Code::NotFound);
  }

  TEST_CASE("ViewService - setPresentation updates state and projection", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();

    auto const result = env.requireView(service);
    auto const viewId = ViewId{result.viewId};

    auto const* preset = builtinTrackPresentationPreset("genres");
    REQUIRE(preset != nullptr);
    service.setPresentation(viewId, preset->spec);
    auto const snap = service.trackListState(viewId);

    CHECK(snap.groupBy == TrackGroupKey::Genre);
    CHECK(snap.presentation.id == "genres");
  }

  TEST_CASE("ViewService - setPresentation no-ops on same value", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();

    auto const* preset = builtinTrackPresentationPreset("years");
    REQUIRE(preset != nullptr);
    auto const result = env.requireView(service);
    service.setPresentation(result.viewId, preset->spec);
    auto const snap = service.trackListState(result.viewId);
    auto const revBefore = snap.revision;

    service.setPresentation(result.viewId, preset->spec);
    auto const snapAfter = service.trackListState(result.viewId);

    CHECK(snapAfter.revision == revBefore);
    CHECK(snapAfter.groupBy == TrackGroupKey::Year);
  }

  TEST_CASE("ViewService - setPresentation publishes PresentationChanged", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();

    auto const result = env.requireView(service);

    auto received = TrackPresentationSpec{};
    auto const sub = service.onPresentationChanged([&](auto const& ev) { received = ev.presentation; });

    auto const* preset = builtinTrackPresentationPreset("albums");
    REQUIRE(preset != nullptr);
    service.setPresentation(result.viewId, preset->spec);

    CHECK(received.id == "albums");
    CHECK(received.groupBy == TrackGroupKey::Album);
  }

  TEST_CASE("ViewService - setPresentation no-op does not publish event", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();

    auto const result = env.requireView(service);

    std::int32_t callCount = 0;
    auto const sub = service.onPresentationChanged([&](auto const&) { ++callCount; });

    auto const* artistPreset = builtinTrackPresentationPreset("artists");
    REQUIRE(artistPreset != nullptr);
    service.setPresentation(result.viewId, artistPreset->spec);
    CHECK(callCount == 1);

    service.setPresentation(result.viewId, artistPreset->spec);
    CHECK(callCount == 1);

    auto const* albumPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumPreset != nullptr);
    service.setPresentation(result.viewId, albumPreset->spec);
    CHECK(callCount == 2);
  }

  TEST_CASE("ViewService - setPresentation with preset string", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();
    auto const result = env.requireView(service);

    auto const spec = service.setPresentation(result.viewId, "artists");
    auto const snap = service.trackListState(result.viewId);

    CHECK(spec.id == "artists");
    CHECK(snap.groupBy == TrackGroupKey::AlbumArtist);

    auto const specInv = service.setPresentation(ViewId{999}, "artists");
    CHECK(specInv.id.empty());
  }
} // namespace ao::rt::test
