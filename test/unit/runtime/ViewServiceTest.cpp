// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include <ao/Type.h>
#include <ao/async/Runtime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/rt/source/ListSourceStore.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct TestEnv final
    {
      TestMusicLibrary library;
      MockExecutor executor;
      async::Runtime runtime;
      LibraryChanges changes;
      LibraryWriter writer;
      std::unique_ptr<ListSourceStore> storePtr;

      TestEnv()
        : runtime{executor}
        , changes{}
        , writer{library.library(), changes}
        , storePtr{std::make_unique<ListSourceStore>(library.library(), changes)}
      {
      }

      ViewService makeService() { return ViewService{executor, library.library(), *storePtr}; }
    };
  } // namespace

  TEST_CASE("ViewService - listViews starts empty", "[app][unit][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

    CHECK(service.listViews().empty());
  }

  TEST_CASE("ViewService - create view via direct API", "[app][unit][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

    SECTION("creating a track list view returns ViewId")
    {
      auto const result = service.createView({.listId = kInvalidListId}, true);
      CHECK(result.viewId != rt::kInvalidViewId);
    }

    SECTION("creating multiple views returns distinct ViewIds")
    {
      auto const r1 = service.createView({}, true);
      auto const r2 = service.createView({}, true);

      CHECK(r1.viewId != r2.viewId);
    }

    SECTION("created view appears in listViews")
    {
      auto const result = service.createView({}, true);
      auto const views = service.listViews();
      CHECK(views.size() == 1);
      CHECK(views[0].id == result.viewId);
      CHECK(views[0].lifecycle == ViewLifecycleState::Attached);
    }

    SECTION("detached view creates in Detached state")
    {
      service.createView({}, false);
      auto const views = service.listViews();
      REQUIRE(views.size() == 1);
      CHECK(views[0].lifecycle == ViewLifecycleState::Detached);
    }
  }

  TEST_CASE("ViewService - destroy view", "[app][unit][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

    auto const result = service.createView({}, true);
    auto const viewId = ViewId{result.viewId};

    SECTION("destroying a view removes it from listViews")
    {
      service.destroyView(viewId);
      auto const views = service.listViews();
      CHECK(views.empty());
    }

    SECTION("destroying non-existent view is safe")
    {
      REQUIRE_NOTHROW(service.destroyView(ViewId{99999}));
    }

    SECTION("state after destroy shows Destroyed lifecycle")
    {
      service.destroyView(viewId);

      auto const snap = service.trackListState(viewId);
      CHECK(snap.lifecycle == ViewLifecycleState::Destroyed);
    }

    SECTION("destroy publishes ViewDestroyed event")
    {
      auto received = kInvalidViewId;
      auto const sub = service.onDestroyed([&](auto viewId) { received = viewId; });

      service.destroyView(viewId);
      CHECK(received == viewId);
    }

    SECTION("destroy releases the owned projection")
    {
      auto projectionPtr = service.trackListProjection(viewId);
      REQUIRE(projectionPtr != nullptr);

      service.destroyView(viewId);

      CHECK(service.trackListProjection(viewId) == nullptr);
      CHECK(projectionPtr->viewId() == viewId);
    }
  }

  TEST_CASE("ViewService - trackListState access", "[app][unit][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

    auto const result = service.createView({.filterExpression = "$year > 2000"}, true);
    auto const snap = service.trackListState(result.viewId);

    CHECK(snap.id == result.viewId);
    CHECK(snap.filterExpression == "$year > 2000");
    CHECK(snap.lifecycle == ViewLifecycleState::Attached);
    CHECK(snap.groupBy == TrackGroupKey::None);

    auto const expectedNone = std::vector{TrackSortField::Artist,
                                          TrackSortField::Album,
                                          TrackSortField::DiscNumber,
                                          TrackSortField::TrackNumber,
                                          TrackSortField::Title};
    REQUIRE(snap.sortBy.size() == expectedNone.size());

    for (std::size_t i = 0; i < expectedNone.size(); ++i)
    {
      CHECK(snap.sortBy[i].field == expectedNone[i]);
      CHECK(snap.sortBy[i].ascending == true);
    }
  }

  TEST_CASE("ViewService - trackListProjection access", "[app][unit][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

    auto const result = service.createView({}, true);
    auto const projectionPtr = service.trackListProjection(result.viewId);
    REQUIRE(projectionPtr != nullptr);
    CHECK(projectionPtr->viewId() == result.viewId);
    CHECK(projectionPtr->size() == 0);
  }

  TEST_CASE("ViewService - projection subscription", "[app][unit][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

    auto const result = service.createView({}, true);
    auto const projectionPtr = service.trackListProjection(result.viewId);
    REQUIRE(projectionPtr != nullptr);

    bool received = false;
    auto const sub = projectionPtr->subscribe(
      [&](TrackListProjectionDeltaBatch const& batch)
      {
        CHECK(std::holds_alternative<ProjectionReset>(batch.deltas[0]));
        received = true;
      });

    CHECK(received);
  }

  TEST_CASE("ViewService - createView with groupBy applies effective sort", "[app][unit][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

    auto const result = service.createView({.groupBy = TrackGroupKey::Artist}, true);
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

  TEST_CASE("ViewService - createView with Album groupBy", "[app][unit][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

    auto const result = service.createView({.groupBy = TrackGroupKey::Album}, true);
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

  TEST_CASE("ViewService - setPresentation updates state and projection", "[app][unit][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

    auto const result = service.createView({}, true);
    auto const viewId = ViewId{result.viewId};

    auto const* preset = builtinTrackPresentationPreset("genres");
    REQUIRE(preset != nullptr);
    service.setPresentation(viewId, preset->spec);
    auto const snap = service.trackListState(viewId);

    CHECK(snap.groupBy == TrackGroupKey::Genre);
    CHECK(snap.presentation.id == "genres");
  }

  TEST_CASE("ViewService - setPresentation no-ops on same value", "[app][unit][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

    auto const* preset = builtinTrackPresentationPreset("years");
    REQUIRE(preset != nullptr);
    auto const result = service.createView({}, true);
    service.setPresentation(result.viewId, preset->spec);
    auto const snap = service.trackListState(result.viewId);
    auto const revBefore = snap.revision;

    service.setPresentation(result.viewId, preset->spec);
    auto const snapAfter = service.trackListState(result.viewId);

    CHECK(snapAfter.revision == revBefore);
    CHECK(snapAfter.groupBy == TrackGroupKey::Year);
  }

  TEST_CASE("ViewService - setPresentation publishes PresentationChanged", "[app][unit][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

    auto const result = service.createView({}, true);

    auto received = TrackPresentationSpec{};
    auto const sub = service.onPresentationChanged([&](auto const& ev) { received = ev.presentation; });

    auto const* preset = builtinTrackPresentationPreset("albums");
    REQUIRE(preset != nullptr);
    service.setPresentation(result.viewId, preset->spec);

    CHECK(received.id == "albums");
    CHECK(received.groupBy == TrackGroupKey::Album);
  }

  TEST_CASE("ViewService - setPresentation no-ops does not publish event", "[app][unit][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();

    auto const result = service.createView({}, true);

    std::int32_t callCount = 0;
    auto const sub = service.onPresentationChanged([&](auto const&) { ++callCount; });

    // First change should publish
    auto const* artistPreset = builtinTrackPresentationPreset("artists");
    REQUIRE(artistPreset != nullptr);
    service.setPresentation(result.viewId, artistPreset->spec);
    CHECK(callCount == 1);

    // No-op should not publish again
    service.setPresentation(result.viewId, artistPreset->spec);
    CHECK(callCount == 1);

    // Different change should publish
    auto const* albumPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumPreset != nullptr);
    service.setPresentation(result.viewId, albumPreset->spec);
    CHECK(callCount == 2);
  }

  TEST_CASE("ViewService - setPresentation with preset string", "[app][unit][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();
    auto const result = service.createView({}, true);

    auto const spec = service.setPresentation(result.viewId, "artists");
    auto const snap = service.trackListState(result.viewId);

    CHECK(spec.id == "artists");
    CHECK(snap.groupBy == TrackGroupKey::Artist);

    // Invalid viewId
    auto const specInv = service.setPresentation(ViewId{999}, "artists");
    CHECK(specInv.id.empty());
  }

  TEST_CASE("ViewService - openListInView", "[app][unit][runtime][view]")
  {
    auto env = TestEnv{};
    auto service = env.makeService();
    auto const trackId = env.library.addTrack(TrackSpec{.title = "List Track"});
    auto const listId = env.writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Manual",
      .trackIds = {trackId},
    });

    auto const result = service.createView({}, true);

    auto listChanged = kInvalidListId;
    auto sub = service.onListChanged([&](auto const& ev) { listChanged = ev.listId; });

    auto projectionChanged = TrackListProjectionChanged{};
    auto projectionSub = service.onProjectionChanged([&](auto const& ev) { projectionChanged = ev; });

    service.openListInView(result.viewId, listId);
    auto const snap = service.trackListState(result.viewId);
    auto const projectionPtr = service.trackListProjection(result.viewId);

    REQUIRE(projectionPtr != nullptr);
    CHECK(snap.listId == listId);
    CHECK(listChanged == listId);
    CHECK(projectionChanged.viewId == result.viewId);
    CHECK(projectionChanged.projectionPtr == projectionPtr);
    REQUIRE(projectionPtr->size() == 1);
    CHECK(projectionPtr->trackIdAt(0) == trackId);

    // Invalid viewId does not crash
    REQUIRE_NOTHROW(service.openListInView(ViewId{999}, listId));
  }

  TEST_CASE("ViewService - selectionDuration sums selected track durations", "[app][unit][runtime][view][selection]")
  {
    auto env = TestEnv{};
    auto const trackA = env.library.addTrack(TrackSpec{.title = "A", .duration = std::chrono::seconds{200}});
    auto const trackB = env.library.addTrack(TrackSpec{.title = "B", .duration = std::chrono::seconds{100}});

    auto service = env.makeService();
    auto const result = service.createView({}, true);

    SECTION("an empty selection has zero duration")
    {
      CHECK(service.selectionDuration(result.viewId) == std::chrono::milliseconds{0});
    }

    SECTION("the selection's durations are summed")
    {
      service.setSelection(result.viewId, {trackA, trackB});
      CHECK(service.selectionDuration(result.viewId) == std::chrono::seconds{300});
    }

    SECTION("ids missing from the library are skipped")
    {
      service.setSelection(result.viewId, {trackA, TrackId{9999}});
      CHECK(service.selectionDuration(result.viewId) == std::chrono::seconds{200});
    }

    SECTION("an unknown view has zero duration")
    {
      CHECK(service.selectionDuration(ViewId{999}) == std::chrono::milliseconds{0});
    }
  }

  TEST_CASE("ViewService - setFilter", "[app][unit][runtime][view]")
  {
    auto env = TestEnv{};
    auto const oldTrackId = env.library.addTrack(TrackSpec{.title = "Old", .year = 1999});
    auto const newTrackId = env.library.addTrack(TrackSpec{.title = "New", .year = 2021});
    env.storePtr->reloadAllTracks();

    auto service = env.makeService();
    auto const result = service.createView({}, true);

    auto filterStr = std::string{};
    auto filterSub = service.onFilterChanged([&](auto const& ev) { filterStr = ev.filterExpression; });

    auto statusStr = std::string{};
    bool statusHasError = true;
    auto statusSub = service.onFilterStatusChanged(
      [&](auto const& ev)
      {
        statusStr = ev.expression;
        statusHasError = static_cast<bool>(ev.optError);
      });

    auto projView = kInvalidViewId;
    std::int32_t projectionChangedCount = 0;
    auto projSub = service.onProjectionChanged(
      [&](auto const& ev)
      {
        projView = ev.viewId;
        ++projectionChangedCount;
      });

    SECTION("setting a new filter expression creates adHocSource")
    {
      service.setFilter(result.viewId, "$year > 2000");
      auto const snap = service.trackListState(result.viewId);
      auto const filteredProjectionPtr = service.trackListProjection(result.viewId);

      REQUIRE(filteredProjectionPtr != nullptr);
      CHECK(snap.filterExpression == "$year > 2000");
      CHECK(filterStr == "$year > 2000");
      CHECK(statusStr == "$year > 2000");
      CHECK_FALSE(statusHasError);
      CHECK(projView == result.viewId);
      CHECK(projectionChangedCount == 1);
      REQUIRE(filteredProjectionPtr->size() == 1);
      CHECK(filteredProjectionPtr->trackIdAt(0) == newTrackId);

      // setting another filter updates adHocSource
      service.setFilter(result.viewId, "$year > 2025");
      auto const snap2 = service.trackListState(result.viewId);
      auto const updatedFilteredProjectionPtr = service.trackListProjection(result.viewId);
      CHECK(snap2.filterExpression == "$year > 2025");
      CHECK(filterStr == "$year > 2025");
      CHECK(projectionChangedCount == 1);
      CHECK(updatedFilteredProjectionPtr == filteredProjectionPtr);
      REQUIRE(updatedFilteredProjectionPtr != nullptr);
      CHECK(updatedFilteredProjectionPtr->size() == 0);

      // clearing filter removes adHocSource
      service.setFilter(result.viewId, "");
      auto const snap3 = service.trackListState(result.viewId);
      auto const unfilteredProjectionPtr = service.trackListProjection(result.viewId);
      CHECK(snap3.filterExpression.empty());
      CHECK(filterStr.empty());
      CHECK(projectionChangedCount == 2);
      CHECK(unfilteredProjectionPtr != filteredProjectionPtr);
      REQUIRE(unfilteredProjectionPtr != nullptr);
      REQUIRE(unfilteredProjectionPtr->size() == 2);
      CHECK(unfilteredProjectionPtr->indexOf(oldTrackId).has_value());
      CHECK(unfilteredProjectionPtr->indexOf(newTrackId).has_value());
    }

    SECTION("invalid view ID is safe")
    {
      REQUIRE_NOTHROW(service.setFilter(ViewId{999}, "foo"));
    }
  }

  TEST_CASE("ViewService - openListInView with active filter", "[app][unit][runtime][view]")
  {
    auto env = TestEnv{};
    auto const oldTrackId = env.library.addTrack(TrackSpec{.title = "Old", .year = 1999});
    auto const newTrackId = env.library.addTrack(TrackSpec{.title = "New", .year = 2021});
    env.storePtr->reloadAllTracks();

    auto const oldListId = env.writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Old only",
      .trackIds = {oldTrackId},
    });

    auto service = env.makeService();
    auto const result = service.createView({.filterExpression = "$year > 2000"}, true);
    auto const initialProjectionPtr = service.trackListProjection(result.viewId);

    REQUIRE(initialProjectionPtr != nullptr);
    REQUIRE(initialProjectionPtr->size() == 1);
    CHECK(initialProjectionPtr->trackIdAt(0) == newTrackId);

    service.openListInView(result.viewId, oldListId);
    auto const snap = service.trackListState(result.viewId);
    auto const projectionPtr = service.trackListProjection(result.viewId);

    REQUIRE(projectionPtr != nullptr);
    CHECK(snap.listId == oldListId);
    CHECK(snap.filterExpression == "$year > 2000"); // retains filter
    CHECK(projectionPtr != initialProjectionPtr);
    CHECK(projectionPtr->size() == 0);
  }
} // namespace ao::rt::test
