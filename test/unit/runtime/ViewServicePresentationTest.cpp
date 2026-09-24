// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/ViewServiceTestSupport.h"
#include <ao/i18n/IcuTextOrdering.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/ordering/TextOrderingPolicy.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("ViewService - live locale change preserves captured playbackPtr ordering",
            "[runtime][unit][view][collation]")
  {
    auto env = ViewServiceFixture{};
    auto const umlaut = env.addTrack(library::test::TrackSpec{.title = "ä"});
    auto const zed = env.addTrack(library::test::TrackSpec{.title = "z"});
    auto germanPtr =
      std::shared_ptr<TextOrderingPolicy const>{ao::test::requireValue(i18n::createIcuTextOrderingPolicy("de"))};
    auto oldPolicyPtr = std::weak_ptr<TextOrderingPolicy const>{germanPtr};
    env.service.setTextOrderingPolicy(germanPtr);
    auto const presentation = TrackPresentationSpec{
      .groupBy = TrackGroupKey::None, .sortBy = {{.field = TrackSortField::Title, .ascending = true}}};
    auto const viewId = env.requireView({.optPresentation = presentation});
    auto const browsePtr = env.requireProjection(viewId);
    auto playbackPtr = env.service.createTransientTrackListProjection(
      ao::test::requireValue(env.cachePtr->acquire(kAllTracksListId)), TrackOrderSpec{.sortBy = presentation.sortBy});
    CHECK(browsePtr->trackIdAt(0) == umlaut);
    CHECK(playbackPtr->trackIdAt(0) == umlaut);
    germanPtr.reset();
    env.service.setTextOrderingPolicy(
      std::shared_ptr<TextOrderingPolicy const>{ao::test::requireValue(i18n::createIcuTextOrderingPolicy("sv"))});
    CHECK(browsePtr->trackIdAt(0) == zed);
    CHECK(playbackPtr->trackIdAt(0) == umlaut);
    CHECK_FALSE(oldPolicyPtr.expired());
    playbackPtr.reset();
    CHECK(oldPolicyPtr.expired());
    auto const next = env.requireView({.optPresentation = presentation});
    CHECK(env.requireProjection(next)->trackIdAt(0) == zed);
  }

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

    auto const res = service.setPresentation(view, defaultTrackPresentationSpec());
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotFound);
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
      CHECK(snap.sortBy[i].ascending);
    }
  }

  TEST_CASE("ViewService - absent presentation uses the normal library default for saved Lists",
            "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;
    auto const listId =
      ao::test::requireValue(env.commandsFixture.runTask(env.commandsFixture.commands().createListAsync(ListDraft{
        .name = "Saved List",
      })));

    auto const created = env.requireView({.listId = listId});
    auto const state = service.trackListState(created);

    auto const expected = normalizeTrackPresentationSpec(defaultTrackPresentationSpec());
    CHECK(state.presentation == expected);
    CHECK(state.groupBy == expected.groupBy);
    CHECK(state.sortBy == expected.sortBy);
  }

  TEST_CASE("ViewService - explicit presentation wins over the saved List default",
            "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;
    auto const listId =
      ao::test::requireValue(env.commandsFixture.runTask(env.commandsFixture.commands().createListAsync(ListDraft{
        .name = "Explicit order",
      })));
    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);

    auto const created = env.requireView({.listId = listId, .optPresentation = albumsPreset->spec});
    auto const state = service.trackListState(created);

    auto const expected = normalizeTrackPresentationSpec(albumsPreset->spec);
    CHECK(state.presentation == expected);
    CHECK(state.groupBy == expected.groupBy);
    CHECK(state.sortBy == expected.sortBy);
  }

  TEST_CASE("ViewService - saved Lists and All Tracks retain the normal default presentation",
            "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;
    auto const listId =
      ao::test::requireValue(env.commandsFixture.runTask(env.commandsFixture.commands().createListAsync(ListDraft{
        .name = "Filtered List",
        .expression = "true",
      })));

    auto const allTracks = env.requireView();
    auto const savedList = env.requireView({.listId = listId});

    auto const expected = normalizeTrackPresentationSpec(defaultTrackPresentationSpec());
    CHECK(service.trackListState(allTracks).presentation == expected);
    CHECK(service.trackListState(savedList).presentation == expected);
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
    auto const rockTrack = env.addTrack(library::test::TrackSpec{.title = "A rock", .genre = "Rock"});
    auto const jazzTrack = env.addTrack(library::test::TrackSpec{.title = "Z jazz", .genre = "Jazz"});
    env.cachePtr->reloadAllTracks();
    auto& service = env.service;

    auto const result = env.requireView();
    auto const viewId = ViewId{result};

    auto const* preset = builtinTrackPresentationPreset("genres");
    REQUIRE(preset != nullptr);
    auto const expected = normalizeTrackPresentationSpec(preset->spec);
    auto const beforeProjectionPtr = env.requireProjection(viewId);
    REQUIRE(beforeProjectionPtr->size() == 2);
    REQUIRE(beforeProjectionPtr->trackIdAt(0) == rockTrack);
    REQUIRE(beforeProjectionPtr->trackIdAt(1) == jazzTrack);
    REQUIRE(service.setPresentation(viewId, preset->spec));
    auto const snap = service.trackListState(viewId);
    auto const projectionPtr = env.requireProjection(viewId);

    CHECK(snap.presentation == expected);
    CHECK(snap.groupBy == expected.groupBy);
    CHECK(snap.sortBy == expected.sortBy);
    REQUIRE(projectionPtr->size() == 2);
    CHECK(projectionPtr->trackIdAt(0) == jazzTrack);
    CHECK(projectionPtr->trackIdAt(1) == rockTrack);
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

    auto received = std::vector<ViewService::PresentationChanged>{};
    auto const sub = service.onPresentationChanged([&](ViewService::PresentationChanged const& changed) noexcept
                                                   { received.push_back(changed); });

    auto const* preset = builtinTrackPresentationPreset("albums");
    REQUIRE(preset != nullptr);
    auto const expected = normalizeTrackPresentationSpec(preset->spec);
    REQUIRE(service.setPresentation(result, preset->spec));
    env.drainCallbacks();

    REQUIRE(received.size() == 1);
    CHECK(received[0].viewId == result);
    CHECK(received[0].presentation == expected);
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
    CHECK(*presentation == normalizeTrackPresentationSpec(preset->spec));
    CHECK(*presentation == service.trackListState(result).presentation);
    CHECK(service.findTrackListPresentation(kInvalidViewId) == nullptr);
  }
} // namespace ao::rt::test
