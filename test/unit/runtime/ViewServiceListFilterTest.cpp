// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/ViewServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/projection/ProjectionTypes.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

namespace ao::rt::test
{
  TEST_CASE("ViewService - openListInView retargets view state", "[runtime][unit][view][list]")
  {
    auto env = ViewServiceTestEnv{};
    auto service = env.makeService();
    auto const trackId = env.library.addTrack(library::test::TrackSpec{.title = "List Track"});
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

    REQUIRE_NOTHROW(service.openListInView(ViewId{999}, listId));
  }

  TEST_CASE("ViewService - setFilter updates filter state and projection", "[runtime][unit][view][filter]")
  {
    auto env = ViewServiceTestEnv{};
    auto const oldTrackId = env.library.addTrack(library::test::TrackSpec{.title = "Old", .year = 1999});
    auto const newTrackId = env.library.addTrack(library::test::TrackSpec{.title = "New", .year = 2021});
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

      service.setFilter(result.viewId, "$year > 2025");
      auto const snap2 = service.trackListState(result.viewId);
      auto const updatedFilteredProjectionPtr = service.trackListProjection(result.viewId);
      CHECK(snap2.filterExpression == "$year > 2025");
      CHECK(filterStr == "$year > 2025");
      CHECK(projectionChangedCount == 1);
      REQUIRE(updatedFilteredProjectionPtr != nullptr);
      CHECK(updatedFilteredProjectionPtr == filteredProjectionPtr);
      CHECK(updatedFilteredProjectionPtr->size() == 0);

      service.setFilter(result.viewId, "");
      auto const snap3 = service.trackListState(result.viewId);
      auto const unfilteredProjectionPtr = service.trackListProjection(result.viewId);
      CHECK(snap3.filterExpression.empty());
      CHECK(filterStr.empty());
      CHECK(projectionChangedCount == 2);
      REQUIRE(unfilteredProjectionPtr != nullptr);
      CHECK(unfilteredProjectionPtr != filteredProjectionPtr);
      REQUIRE(unfilteredProjectionPtr->size() == 2);
      CHECK(unfilteredProjectionPtr->indexOf(oldTrackId).has_value());
      CHECK(unfilteredProjectionPtr->indexOf(newTrackId).has_value());
    }

    SECTION("invalid view ID is safe")
    {
      REQUIRE_NOTHROW(service.setFilter(ViewId{999}, "foo"));
    }
  }

  TEST_CASE("ViewService - openListInView with active filter preserves filter state",
            "[runtime][unit][view][filter][list]")
  {
    auto env = ViewServiceTestEnv{};
    auto const oldTrackId = env.library.addTrack(library::test::TrackSpec{.title = "Old", .year = 1999});
    auto const newTrackId = env.library.addTrack(library::test::TrackSpec{.title = "New", .year = 2021});
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
    CHECK(snap.filterExpression == "$year > 2000");
    CHECK(projectionPtr != initialProjectionPtr);
    CHECK(projectionPtr->size() == 0);
  }
} // namespace ao::rt::test
