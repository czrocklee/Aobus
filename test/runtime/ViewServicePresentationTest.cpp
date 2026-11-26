// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "runtime/RuntimeOperationProbe.h"
#include "test/TestFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/runtime/ViewServiceTestSupport.h"
#include <ao/Error.h>
#include <ao/i18n/IcuTextOrdering.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/ordering/TextOrderingPolicy.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    template<typename Service>
    concept ExposesRawPresentationMutation =
      requires(Service& service, ViewId viewId, TrackPresentationSpec const& presentation) {
        service.setPresentation(viewId, presentation);
      };

    static_assert(!ExposesRawPresentationMutation<ViewService>);
  } // namespace

  TEST_CASE("ViewService - live locale change preserves captured playbackPtr ordering")
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

  TEST_CASE("ViewService - an explicit Performers preset sets the grouping and order")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;

    auto const& preset = builtinTrackPresentationPreset(BuiltinPreset::Performers);
    auto const result = env.requireView({.optPresentation = preset.spec});
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

  TEST_CASE("ViewService - column sorting reports a removed view")
  {
    auto env = ViewServiceFixture{};
    auto const view = env.requireView();
    REQUIRE(env.workspace.closeView(view));

    auto const res = env.service.setColumnSort(view, TrackColumnSort{.column = TrackField::Title});
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotFound);
  }

  TEST_CASE("ViewService - an explicit Albums preset sets the grouping and order")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;

    auto const& preset = builtinTrackPresentationPreset(BuiltinPreset::Albums);
    auto const result = env.requireView({.optPresentation = preset.spec});
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

  TEST_CASE("ViewService - absent presentation uses the normal library default for saved Lists")
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

  TEST_CASE("ViewService - explicit presentation wins over the saved List default")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;
    auto const listId =
      ao::test::requireValue(env.commandsFixture.runTask(env.commandsFixture.commands().createListAsync(ListDraft{
        .name = "Explicit order",
      })));
    auto const& albums = builtinTrackPresentationPreset(BuiltinPreset::Albums).spec;

    auto const created = env.requireView({.listId = listId, .optPresentation = albums});
    auto const state = service.trackListState(created);

    auto const expected = normalizeTrackPresentationSpec(albums);
    CHECK(state.presentation == expected);
    CHECK(state.groupBy == expected.groupBy);
    CHECK(state.sortBy == expected.sortBy);
  }

  TEST_CASE("ViewService - saved Lists and All Tracks retain the normal default presentation")
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

  TEST_CASE("ViewService - playback launch capture contains exact list filter and sort only")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;
    auto const& genres = builtinTrackPresentationPreset(BuiltinPreset::Genres).spec;
    auto const created = env.requireView({.filterExpression = "$year > 2000", .optPresentation = genres});

    auto const capturedRes = service.capturePlaybackLaunchSpec(created);

    REQUIRE(capturedRes);
    CHECK(capturedRes->sourceListId == kAllTracksListId);
    CHECK(capturedRes->quickFilterExpression == "$year > 2000");
    CHECK(capturedRes->order.sortBy == genres.sortBy);

    auto const missingRes = service.capturePlaybackLaunchSpec(ViewId{999999});
    REQUIRE_FALSE(missingRes);
    CHECK(missingRes.error().code == Error::Code::NotFound);
  }

  TEST_CASE("ViewService - workspace presentation changes update state and projection")
  {
    auto env = ViewServiceFixture{};
    auto const rockTrack = env.addTrack(library::test::TrackSpec{.title = "A rock", .genre = "Rock"});
    auto const jazzTrack = env.addTrack(library::test::TrackSpec{.title = "Z jazz", .genre = "Jazz"});
    env.cachePtr->reloadAllTracks();
    auto& service = env.service;

    auto const viewId = env.requireView();

    auto const& genres = builtinTrackPresentationPreset(BuiltinPreset::Genres).spec;
    auto const expected = normalizeTrackPresentationSpec(genres);
    auto const beforeProjectionPtr = env.requireProjection(viewId);
    REQUIRE(beforeProjectionPtr->size() == 2);
    REQUIRE(beforeProjectionPtr->trackIdAt(0) == rockTrack);
    REQUIRE(beforeProjectionPtr->trackIdAt(1) == jazzTrack);
    REQUIRE(env.workspace.setActivePresentation(genres, {.recordHistory = false}));
    auto const snap = service.trackListState(viewId);
    auto const projectionPtr = env.requireProjection(viewId);

    CHECK(snap.presentation == expected);
    CHECK(snap.groupBy == expected.groupBy);
    CHECK(snap.sortBy == expected.sortBy);
    REQUIRE(projectionPtr->size() == 2);
    CHECK(projectionPtr->trackIdAt(0) == jazzTrack);
    CHECK(projectionPtr->trackIdAt(1) == rockTrack);
  }

  TEST_CASE("ViewService - workspace presentation updates can change fields without changing id or order")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;
    auto const result = env.requireView();
    auto presentation = defaultTrackPresentationSpec();
    presentation.presetId = CustomPresetId{1};
    presentation.visibleFields = {TrackField::Title};
    REQUIRE(env.workspace.setActivePresentation(presentation, {.recordHistory = false}));

    presentation.visibleFields = {TrackField::Title, TrackField::Artist};
    REQUIRE(env.workspace.setActivePresentation(presentation, {.recordHistory = false}));

    auto const state = service.trackListState(result);
    CHECK(state.presentation.visibleFields == presentation.visibleFields);
  }

  TEST_CASE("ViewService - workspace presentation updates publish the normalized spec")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;

    auto const result = env.requireView();

    auto received = std::vector<ViewService::PresentationChanged>{};
    auto const sub = service.onPresentationChanged([&](ViewService::PresentationChanged const& changed) noexcept
                                                   { received.push_back(changed); });

    auto const& albums = builtinTrackPresentationPreset(BuiltinPreset::Albums).spec;
    auto const expected = normalizeTrackPresentationSpec(albums);
    REQUIRE(env.workspace.setActivePresentation(albums, {.recordHistory = false}));
    env.drainCallbacks();

    REQUIRE(received.size() == 1);
    CHECK(received[0].viewId == result);
    CHECK(received[0].presentation == expected);
  }

  TEST_CASE("ViewService - repeating a column order does not publish another event")
  {
    auto env = ViewServiceFixture{};
    auto const viewId = env.requireView();
    std::int32_t callCount = 0;
    auto const sub = env.service.onPresentationChanged([&](auto const&) noexcept { ++callCount; });
    auto term = TrackColumnSort{.column = TrackField::Title, .ascending = false};

    REQUIRE(env.service.setColumnSort(viewId, term));
    env.drainCallbacks();
    REQUIRE(callCount == 1);

    REQUIRE(env.service.setColumnSort(viewId, term));
    env.drainCallbacks();
    CHECK(callCount == 1);
    CHECK(env.service.trackListState(viewId).presentation.optColumnSort == term);

    term.ascending = true;
    REQUIRE(env.service.setColumnSort(viewId, term));
    env.drainCallbacks();
    CHECK(callCount == 2);
    CHECK(env.service.trackListState(viewId).presentation.optColumnSort == term);
  }

  TEST_CASE("ViewService - column sorting preserves the custom preset without a workspace commit")
  {
    auto env = ViewServiceFixture{};
    auto const initial = TrackPresentationSpec{
      .presetId = CustomPresetId{1},
      .groupBy = TrackGroupKey::Artist,
      .sortBy = {{.field = TrackSortField::Year}},
      .visibleFields = {TrackField::Title, TrackField::Artist, TrackField::Duration},
      .redundantFields = {TrackField::Artist},
    };
    auto const viewId = env.requireView({.optPresentation = initial});
    auto const before = env.workspace.snapshot();
    REQUIRE(before.activeViewId == viewId);
    REQUIRE_FALSE(env.workspace.canGoBack());
    REQUIRE_FALSE(env.workspace.canGoForward());

    auto received = std::vector<ViewService::PresentationChanged>{};
    auto const presentationSub = env.service.onPresentationChanged(
      [&](ViewService::PresentationChanged const& changed) noexcept { received.push_back(changed); });
    std::int32_t workspaceChanges = 0;
    auto const workspaceSub = env.workspace.onChanged([&](WorkspaceChanged const&) noexcept { ++workspaceChanges; });
    auto const sort = TrackColumnSort{.column = TrackField::Title, .ascending = false};

    REQUIRE(env.service.setColumnSort(viewId, sort));
    env.drainCallbacks();

    auto expected = initial;
    expected.optColumnSort = sort;
    auto const order = std::vector<TrackSortTerm>{{TrackSortField::Title, false}, {TrackSortField::Year, true}};
    auto const state = env.service.trackListState(viewId);
    CHECK(state.presentation == expected);
    CHECK(state.presentation.isPreset(CustomPresetId{1}));
    CHECK(state.groupBy == initial.groupBy);
    CHECK(state.sortBy == order);
    CHECK(env.requireProjection(viewId)->presentation() == expected);
    REQUIRE(received.size() == 1);
    CHECK(received.front().viewId == viewId);
    CHECK(received.front().presentation == expected);
    CHECK(workspaceChanges == 0);
    CHECK(env.workspace.snapshot() == before);
    CHECK_FALSE(env.workspace.canGoBack());
    CHECK_FALSE(env.workspace.canGoForward());
  }

  TEST_CASE("ViewService - a new Songs column sort replaces the previous click and keeps preset ties")
  {
    auto env = ViewServiceFixture{};
    auto const& songs = builtinTrackPresentationPreset(BuiltinPreset::Songs).spec;
    auto const viewId = env.requireView({.optPresentation = songs});
    auto const duration = TrackColumnSort{TrackField::Duration, true};
    auto const artist = TrackColumnSort{TrackField::Artist, false};
    auto const title = TrackSortTerm{TrackSortField::Title, true};
    auto const album = TrackSortTerm{TrackSortField::Album, true};
    auto const durationOrder = std::vector{
      TrackSortTerm{TrackSortField::Duration, true}, title, TrackSortTerm{TrackSortField::Artist, true}, album};

    REQUIRE(env.service.setColumnSort(viewId, duration));
    CHECK(env.service.trackListState(viewId).sortBy == durationOrder);

    REQUIRE(env.service.setColumnSort(viewId, artist));

    auto const state = env.service.trackListState(viewId);
    CHECK(state.presentation.isPreset(BuiltinPreset::Songs));
    CHECK(state.presentation.sortBy == songs.sortBy);
    CHECK(state.presentation.optColumnSort == artist);
    auto const expectedOrder = std::vector{TrackSortTerm{TrackSortField::Artist, false}, title, album};
    CHECK(state.sortBy == expectedOrder);
    CHECK(state.presentation.effectiveSortBy() == expectedOrder);
    auto const launch = ao::test::requireValue(env.service.capturePlaybackLaunchSpec(viewId));
    CHECK(launch.order.sortBy == expectedOrder);
  }

  TEST_CASE("ViewService - column sorting keeps Albums groups ordered and contiguous")
  {
    auto env = ViewServiceFixture{};
    auto const zulu = env.addTrack(library::test::TrackSpec{.title = "Middle", .artist = "Band", .album = "Zulu"});
    auto const firstAlpha =
      env.addTrack(library::test::TrackSpec{.title = "Last", .artist = "Band", .album = "Alpha", .trackNumber = 1});
    auto const secondAlpha =
      env.addTrack(library::test::TrackSpec{.title = "First", .artist = "Band", .album = "Alpha", .trackNumber = 2});
    env.cachePtr->reloadAllTracks();
    auto const& albums = builtinTrackPresentationPreset(BuiltinPreset::Albums).spec;
    auto const viewId = env.requireView({.optPresentation = albums});
    auto term = TrackColumnSort{};
    auto expectedRows = std::vector{firstAlpha, secondAlpha, zulu};

    SECTION("the descending Album column does not reverse group headings")
    {
      term = {TrackField::Album, false};
    }

    SECTION("the Title column reorders tracks only within each group")
    {
      term = {TrackField::Title, true};
      expectedRows = {secondAlpha, firstAlpha, zulu};
    }

    REQUIRE(env.service.setColumnSort(viewId, term));

    auto const state = env.service.trackListState(viewId);
    CHECK(state.presentation.isPreset(BuiltinPreset::Albums));
    CHECK(state.presentation.optColumnSort == term);
    auto const projectionPtr = env.requireProjection(viewId);
    REQUIRE(projectionPtr->size() == expectedRows.size());
    REQUIRE(projectionPtr->groupCount() == 2);
    CHECK(projectionPtr->trackIdAt(0) == expectedRows[0]);
    CHECK(projectionPtr->trackIdAt(1) == expectedRows[1]);
    CHECK(projectionPtr->trackIdAt(2) == expectedRows[2]);
    CHECK(projectionPtr->groupIndexAt(0) == 0);
    CHECK(projectionPtr->groupIndexAt(1) == 0);
    CHECK(projectionPtr->groupIndexAt(2) == 1);
  }

  TEST_CASE("ViewService - column sorting changes an inactive view without focusing it")
  {
    auto env = ViewServiceFixture{};
    auto const inactiveView = env.requireView({.filterExpression = "$year > 2000"});
    auto const activeView = env.requireView();
    auto const before = env.workspace.snapshot();
    REQUIRE(inactiveView != activeView);
    REQUIRE(before.activeViewId == activeView);
    REQUIRE(env.workspace.canGoBack());
    auto const activePresentation = env.service.trackListState(activeView).presentation;
    auto const term = TrackColumnSort{.column = TrackField::Title, .ascending = false};

    REQUIRE(env.service.setColumnSort(inactiveView, term));
    env.drainCallbacks();

    CHECK(env.workspace.snapshot() == before);
    CHECK(env.workspace.canGoBack());
    CHECK(env.service.trackListState(activeView).presentation == activePresentation);
    auto const inactiveState = env.service.trackListState(inactiveView);
    CHECK(inactiveState.presentation.optColumnSort == term);
    CHECK(inactiveState.filterExpression == "$year > 2000");
  }

  TEST_CASE("ViewService - alias clicks share row ordering but toggle only the clicked column")
  {
    auto env = ViewServiceFixture{};
    auto const later = env.addTrack(library::test::TrackSpec{
      .title = "Alpha", .movement = "Alpha", .discNumber = 2, .trackNumber = 1, .movementNumber = 2});
    auto const earlier = env.addTrack(library::test::TrackSpec{
      .title = "Zulu", .movement = "Zulu", .discNumber = 1, .trackNumber = 2, .movementNumber = 1});
    env.cachePtr->reloadAllTracks();
    auto firstColumn = TrackField::TrackNumber;
    auto siblingColumn = TrackField::DisplayTrackNumber;

    SECTION("track number columns share disc and track ordering")
    {
    }

    SECTION("movement columns share movement-number ordering")
    {
      firstColumn = TrackField::Movement;
      siblingColumn = TrackField::MovementNumber;
    }

    auto presentation = builtinTrackPresentationPreset(BuiltinPreset::Songs).spec;
    presentation.visibleFields = {TrackField::Title, firstColumn, siblingColumn};
    auto const viewId = env.requireView({.optPresentation = presentation});
    auto const projectionPtr = env.requireProjection(viewId);
    REQUIRE(projectionPtr->size() == 2);

    auto const click = [&](TrackField column)
    {
      auto const spec = env.service.trackListState(viewId).presentation;
      REQUIRE(env.service.setColumnSort(viewId, spec.nextColumnSort(column)));
      return env.service.trackListState(viewId).presentation.optColumnSort;
    };

    REQUIRE(click(firstColumn) == TrackColumnSort{firstColumn, true});
    CHECK(projectionPtr->trackIdAt(0) == earlier);
    env.drainCallbacks();
    auto const before = rt::detail::RuntimeOperationProbe::counts(*projectionPtr);
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    auto const projectionSub = projectionPtr->subscribe([&](auto const& batch) noexcept { batches.push_back(batch); });
    batches.clear();
    auto changes = std::vector<ViewService::PresentationChanged>{};
    auto const presentationSub = env.service.onPresentationChanged(
      [&](ViewService::PresentationChanged const& change) noexcept { changes.push_back(change); });

    auto const siblingSort = TrackColumnSort{siblingColumn, true};
    REQUIRE(click(siblingColumn) == siblingSort);
    env.drainCallbacks();

    presentation.optColumnSort = siblingSort;
    CHECK(rt::detail::RuntimeOperationProbe::counts(*projectionPtr) == before);
    CHECK(projectionPtr->size() == 2);
    CHECK(projectionPtr->trackIdAt(0) == earlier);
    CHECK(projectionPtr->trackIdAt(1) == later);
    CHECK(projectionPtr->indexOf(later) == 1);
    CHECK(projectionPtr->presentation() == presentation);
    auto const* stored = env.service.findTrackListPresentation(viewId);
    REQUIRE(stored != nullptr);
    CHECK(*stored == presentation);
    CHECK(batches.empty());
    REQUIRE(changes.size() == 1);
    CHECK(changes.front().viewId == viewId);
    CHECK(changes.front().presentation == presentation);

    REQUIRE(click(siblingColumn) == TrackColumnSort{siblingColumn, false});
    CHECK(projectionPtr->trackIdAt(0) == later);
    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 1);
    CHECK(std::holds_alternative<ProjectionReset>(batches.front().deltas.front()));
    REQUIRE(click(firstColumn) == TrackColumnSort{firstColumn, true});
    CHECK(projectionPtr->trackIdAt(0) == earlier);
  }

  TEST_CASE("ViewService - adding and clearing a column marker for the primary base sort preserves rows")
  {
    auto env = ViewServiceFixture{};
    auto const zulu = env.addTrack(library::test::TrackSpec{.title = "Zulu"});
    auto const alpha = env.addTrack(library::test::TrackSpec{.title = "Alpha"});
    env.cachePtr->reloadAllTracks();
    auto const& songs = builtinTrackPresentationPreset(BuiltinPreset::Songs).spec;
    auto const viewId = env.requireView({.optPresentation = songs});
    auto const projectionPtr = env.requireProjection(viewId);
    auto batches = std::vector<TrackListProjectionDeltaBatch>{};
    auto const projectionSub = projectionPtr->subscribe([&](auto const& batch) noexcept { batches.push_back(batch); });
    batches.clear();
    auto changes = std::vector<ViewService::PresentationChanged>{};
    auto const presentationSub = env.service.onPresentationChanged(
      [&](ViewService::PresentationChanged const& change) noexcept { changes.push_back(change); });
    auto marked = songs;
    marked.optColumnSort = TrackColumnSort{TrackField::Title, true};

    REQUIRE(env.service.setColumnSort(viewId, *marked.optColumnSort));
    env.drainCallbacks();

    CHECK(projectionPtr->presentation() == marked);
    CHECK(batches.empty());
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].viewId == viewId);
    CHECK(changes[0].presentation == marked);

    REQUIRE(env.workspace.setActivePresentation(songs, {.recordHistory = false}));
    env.drainCallbacks();

    CHECK(projectionPtr->presentation() == songs);
    CHECK(batches.empty());
    REQUIRE(changes.size() == 2);
    CHECK(changes[1].viewId == viewId);
    CHECK(changes[1].presentation == songs);
    REQUIRE(projectionPtr->size() == 2);
    CHECK(projectionPtr->trackIdAt(0) == alpha);
    CHECK(projectionPtr->trackIdAt(1) == zulu);
  }

  TEST_CASE("ViewService - invalid and unsortable columns leave the current sort untouched")
  {
    auto env = ViewServiceFixture{};
    auto const viewId = env.requireView();
    REQUIRE(env.service.setColumnSort(viewId, TrackColumnSort{TrackField::Title, false}));
    env.drainCallbacks();
    auto const before = env.service.trackListState(viewId);
    auto const workspaceBefore = env.workspace.snapshot();
    auto const projectionPtr = env.requireProjection(viewId);
    auto const projectedBefore = projectionPtr->presentation();
    std::int32_t presentationChanges = 0;
    auto const presentationSub =
      env.service.onPresentationChanged([&](auto const&) noexcept { ++presentationChanges; });
    std::int32_t workspaceChanges = 0;
    auto const workspaceSub = env.workspace.onChanged([&](WorkspaceChanged const&) noexcept { ++workspaceChanges; });
    auto invalid = TrackField::MovementTotal;

    SECTION("known unsortable column")
    {
    }

    SECTION("unknown column")
    {
      invalid = static_cast<TrackField>(kTrackFieldCount);
    }

    auto const res = env.service.setColumnSort(viewId, TrackColumnSort{invalid});

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidInput);
    env.drainCallbacks();
    auto const after = env.service.trackListState(viewId);
    CHECK(after.presentation == before.presentation);
    CHECK(after.sortBy == before.sortBy);
    CHECK(projectionPtr->presentation() == projectedBefore);
    CHECK(env.workspace.snapshot() == workspaceBefore);
    CHECK_FALSE(env.workspace.canGoBack());
    CHECK(presentationChanges == 0);
    CHECK(workspaceChanges == 0);
  }

  TEST_CASE("ViewService - findTrackListPresentation borrows the stored spec and reports a miss")
  {
    auto env = ViewServiceFixture{};
    auto& service = env.service;

    auto const result = env.requireView();
    auto const& albums = builtinTrackPresentationPreset(BuiltinPreset::Albums).spec;
    REQUIRE(env.workspace.setActivePresentation(albums, {.recordHistory = false}));

    auto const* presentation = service.findTrackListPresentation(result);
    auto const* presentationAgain = service.findTrackListPresentation(result);

    // Both calls hand back the same stored object: an accessor to the view's spec,
    // not a per-call copy of the whole TrackListViewState.
    REQUIRE(presentation != nullptr);
    CHECK(presentation == presentationAgain);
    CHECK(*presentation == normalizeTrackPresentationSpec(albums));
    CHECK(*presentation == service.trackListState(result).presentation);
    CHECK(service.findTrackListPresentation(kInvalidViewId) == nullptr);
  }
} // namespace ao::rt::test
