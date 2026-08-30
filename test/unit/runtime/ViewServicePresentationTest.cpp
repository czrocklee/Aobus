// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/runtime/ViewServiceTestSupport.h"
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryCommands.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("ViewService - createView with groupBy applies effective sort", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;

    auto const result = env.requireView({.groupBy = TrackGroupKey::Artist});
    auto const snap = service.trackListState(result);

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

  TEST_CASE("ViewService - presentation mutation reports a removed view", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;
    auto const view = env.requireView();
    REQUIRE(env.workspace.closeView(view));

    auto const result = service.setPresentation(view, defaultTrackPresentationSpec());
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
  }

  TEST_CASE("ViewService - createView with Album groupBy applies album sort", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;

    auto const result = env.requireView({.groupBy = TrackGroupKey::Album});
    auto const snap = service.trackListState(result);

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

  TEST_CASE("ViewService - absent presentation uses the normal library default for saved Lists",
            "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;
    auto const listId =
      ao::test::requireValue(env.commandsFixture.runTask(env.commandsFixture.commands().createList(ListDraft{
        .name = "Saved List",
      })));

    auto const created = env.requireView({.listId = listId});
    auto const state = service.trackListState(created);

    CHECK(state.presentation.id == kDefaultTrackPresentationId);
    CHECK(state.groupBy == TrackGroupKey::None);
    CHECK_FALSE(state.sortBy.empty());
  }

  TEST_CASE("ViewService - explicit presentation wins over the saved List default",
            "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;
    auto const listId =
      ao::test::requireValue(env.commandsFixture.runTask(env.commandsFixture.commands().createList(ListDraft{
        .name = "Explicit order",
      })));
    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);

    auto const created = env.requireView({.listId = listId, .optPresentation = albumsPreset->spec});
    auto const state = service.trackListState(created);

    CHECK(state.presentation.id == "albums");
    CHECK(state.groupBy == TrackGroupKey::Album);
    CHECK_FALSE(state.sortBy.empty());
  }

  TEST_CASE("ViewService - saved Lists and All Tracks retain the normal default presentation",
            "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;
    auto const listId =
      ao::test::requireValue(env.commandsFixture.runTask(env.commandsFixture.commands().createList(ListDraft{
        .name = "Filtered List",
        .expression = "true",
      })));

    auto const allTracks = env.requireView();
    auto const savedList = env.requireView({.listId = listId});

    CHECK(service.trackListState(allTracks).presentation.id == kDefaultTrackPresentationId);
    CHECK(service.trackListState(savedList).presentation.id == kDefaultTrackPresentationId);
  }

  TEST_CASE("ViewService - playback launch capture contains exact list filter and sort only",
            "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;
    auto const* genresPreset = builtinTrackPresentationPreset("genres");
    REQUIRE(genresPreset != nullptr);
    auto const created = env.requireView({.filterExpression = "$year > 2000", .optPresentation = genresPreset->spec});

    auto const capturedRes = service.capturePlaybackLaunchSpec(created);

    REQUIRE(capturedRes);
    CHECK(capturedRes->sourceListId == kAllTracksListId);
    CHECK(capturedRes->quickFilterExpression == "$year > 2000");
    CHECK(capturedRes->order.sortBy == genresPreset->spec.sortBy);

    auto const missingRes = service.capturePlaybackLaunchSpec(ViewId{999999});
    REQUIRE_FALSE(missingRes);
    CHECK(missingRes.error().code == Error::Code::NotFound);
  }

  TEST_CASE("ViewService - setPresentation updates state and projection", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;

    auto const result = env.requireView();
    auto const viewId = ViewId{result};

    auto const* preset = builtinTrackPresentationPreset("genres");
    REQUIRE(preset != nullptr);
    REQUIRE(service.setPresentation(viewId, preset->spec));
    auto const snap = service.trackListState(viewId);

    CHECK(snap.groupBy == TrackGroupKey::Genre);
    CHECK(snap.presentation.id == "genres");
  }

  TEST_CASE("ViewService - setPresentation no-ops on same value", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;

    auto const* preset = builtinTrackPresentationPreset("years");
    REQUIRE(preset != nullptr);
    auto const result = env.requireView();
    std::int32_t published = 0;
    auto const sub = service.onPresentationChanged([&](auto const&) noexcept { ++published; });
    REQUIRE(service.setPresentation(result, preset->spec));
    env.drainCallbacks();
    CHECK(published == 1);

    REQUIRE(service.setPresentation(result, preset->spec));
    env.drainCallbacks();
    auto const snapAfter = service.trackListState(result);

    CHECK(published == 1);
    CHECK(snapAfter.groupBy == TrackGroupKey::Year);
  }

  TEST_CASE("ViewService - setPresentation applies field changes under the same id and order",
            "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;
    auto const result = env.requireView();
    auto presentation = defaultTrackPresentationSpec();
    presentation.id = "custom";
    presentation.visibleFields = {TrackField::Title};
    REQUIRE(service.setPresentation(result, presentation));

    presentation.visibleFields = {TrackField::Title, TrackField::Artist};
    REQUIRE(service.setPresentation(result, presentation));

    auto const state = service.trackListState(result);
    CHECK(state.presentation.visibleFields == presentation.visibleFields);
  }

  TEST_CASE("ViewService - setPresentation publishes PresentationChanged", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;

    auto const result = env.requireView();

    auto received = TrackPresentationSpec{};
    auto const sub = service.onPresentationChanged([&](auto const& ev) noexcept { received = ev.presentation; });

    auto const* preset = builtinTrackPresentationPreset("albums");
    REQUIRE(preset != nullptr);
    REQUIRE(service.setPresentation(result, preset->spec));
    env.drainCallbacks();

    CHECK(received.id == "albums");
    CHECK(received.groupBy == TrackGroupKey::Album);
  }

  TEST_CASE("ViewService - setPresentation no-op does not publish event", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;

    auto const result = env.requireView();

    std::int32_t callCount = 0;
    auto const sub = service.onPresentationChanged([&](auto const&) noexcept { ++callCount; });

    auto const* artistPreset = builtinTrackPresentationPreset("artists");
    REQUIRE(artistPreset != nullptr);
    REQUIRE(service.setPresentation(result, artistPreset->spec));
    env.drainCallbacks();
    CHECK(callCount == 1);

    REQUIRE(service.setPresentation(result, artistPreset->spec));
    env.drainCallbacks();
    CHECK(callCount == 1);

    auto const* albumPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumPreset != nullptr);
    REQUIRE(service.setPresentation(result, albumPreset->spec));
    env.drainCallbacks();
    CHECK(callCount == 2);
  }

  TEST_CASE("ViewService - findTrackListPresentation borrows the stored spec and reports a miss",
            "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;

    auto const result = env.requireView();
    auto const* preset = builtinTrackPresentationPreset("albums");
    REQUIRE(preset != nullptr);
    REQUIRE(service.setPresentation(result, preset->spec));

    auto const* presentation = service.findTrackListPresentation(result);
    auto const* presentationAgain = service.findTrackListPresentation(result);

    // Both calls hand back the same stored object: an accessor to the view's spec,
    // not a per-call copy of the whole TrackListViewState.
    REQUIRE(presentation != nullptr);
    CHECK(presentation == presentationAgain);
    CHECK(presentation->id == "albums");
    CHECK(presentation->id == service.trackListState(result).presentation.id);
    CHECK(service.findTrackListPresentation(kInvalidViewId) == nullptr);
  }
} // namespace ao::rt::test
