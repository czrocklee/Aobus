// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/ViewServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

namespace ao::rt::test
{
  TEST_CASE("ViewService - openListInView retargets view state", "[runtime][unit][view][list]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();
    auto const trackId = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "List Track"});
    env.cachePtr->reloadAllTracks();
    auto const listId = ao::test::requireValue(env.writer().createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Manual",
      .trackIds = {trackId},
    }));

    auto const result = env.requireView(service);

    auto listChanged = kInvalidListId;
    auto sub = service.onListChanged([&](auto const& ev) { listChanged = ev.listId; });

    auto projectionChanged = TrackListProjectionChanged{};
    auto projectionSub = service.onProjectionChanged([&](auto const& ev) { projectionChanged = ev; });

    REQUIRE(service.openListInView(result, listId));
    auto const snap = service.trackListState(result);
    auto const projectionPtr = service.trackListProjection(result);

    REQUIRE(projectionPtr != nullptr);
    CHECK(snap.listId == listId);
    CHECK(listChanged == listId);
    CHECK(projectionChanged.viewId == result);
    CHECK(projectionChanged.projectionPtr == projectionPtr);
    REQUIRE(projectionPtr->size() == 1);
    CHECK(projectionPtr->trackIdAt(0) == trackId);

    auto const missingView = service.openListInView(ViewId{999}, listId);
    REQUIRE_FALSE(missingView);
    CHECK(missingView.error().code == Error::Code::NotFound);
  }

  TEST_CASE("ViewService - failed list switch leaves state projection and events unchanged",
            "[runtime][unit][view][list]")
  {
    auto env = ViewServiceFixture{};
    auto service = env.makeService();
    auto const created = env.requireView(service);
    auto const before = service.trackListState(created);
    auto const projectionPtr = service.trackListProjection(created);
    std::int32_t listChangedCount = 0;
    std::int32_t projectionChangedCount = 0;
    auto const listSub = service.onListChanged([&](auto const&) { ++listChangedCount; });
    auto const projectionSub = service.onProjectionChanged([&](auto const&) { ++projectionChangedCount; });

    auto const result = service.openListInView(created, ListId{999999});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
    auto const after = service.trackListState(created);
    CHECK(after.listId == before.listId);
    CHECK(after.filterExpression == before.filterExpression);
    CHECK(after.presentation == before.presentation);
    CHECK(service.trackListProjection(created) == projectionPtr);
    CHECK(listChangedCount == 0);
    CHECK(projectionChangedCount == 0);
    CHECK(service.listViews().size() == 1);
  }

  TEST_CASE("ViewService - setFilter updates filter state and projection", "[runtime][unit][view][filter]")
  {
    auto env = ViewServiceFixture{};
    auto const oldTrackId = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "Old", .year = 1999});
    auto const newTrackId = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "New", .year = 2021});
    env.cachePtr->reloadAllTracks();

    auto service = env.makeService();
    auto const result = env.requireView(service);

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
      REQUIRE(service.setFilter(result, "$year > 2000"));
      auto const snap = service.trackListState(result);
      auto const filteredProjectionPtr = service.trackListProjection(result);

      REQUIRE(filteredProjectionPtr != nullptr);
      CHECK(snap.filterExpression == "$year > 2000");
      CHECK_FALSE(snap.optFilterError);
      CHECK(projView == result);
      CHECK(projectionChangedCount == 1);
      REQUIRE(filteredProjectionPtr->size() == 1);
      CHECK(filteredProjectionPtr->trackIdAt(0) == newTrackId);

      REQUIRE(service.setFilter(result, "$year > 2025"));
      auto const snap2 = service.trackListState(result);
      auto const updatedFilteredProjectionPtr = service.trackListProjection(result);
      CHECK(snap2.filterExpression == "$year > 2025");
      CHECK_FALSE(snap2.optFilterError);
      CHECK(projectionChangedCount == 2);
      REQUIRE(updatedFilteredProjectionPtr != nullptr);
      CHECK(updatedFilteredProjectionPtr != filteredProjectionPtr);
      CHECK(updatedFilteredProjectionPtr->size() == 0);

      REQUIRE(service.setFilter(result, ""));
      auto const snap3 = service.trackListState(result);
      auto const unfilteredProjectionPtr = service.trackListProjection(result);
      CHECK(snap3.filterExpression.empty());
      CHECK_FALSE(snap3.optFilterError);
      CHECK(projectionChangedCount == 3);
      REQUIRE(unfilteredProjectionPtr != nullptr);
      CHECK(unfilteredProjectionPtr != filteredProjectionPtr);
      REQUIRE(unfilteredProjectionPtr->size() == 2);
      CHECK(unfilteredProjectionPtr->indexOf(oldTrackId).has_value());
      CHECK(unfilteredProjectionPtr->indexOf(newTrackId).has_value());
    }

    SECTION("invalid expression is retained with its synchronous error")
    {
      REQUIRE(service.setFilter(result, "$year >"));
      auto const snap = service.trackListState(result);
      auto const filteredProjectionPtr = service.trackListProjection(result);

      CHECK(snap.filterExpression == "$year >");
      REQUIRE(snap.optFilterError);
      CHECK(snap.optFilterError->code == Error::Code::FormatRejected);
      REQUIRE(filteredProjectionPtr != nullptr);
      CHECK(filteredProjectionPtr->size() == 0);
      CHECK(projectionChangedCount == 1);
    }

    SECTION("invalid view ID is safe")
    {
      auto const missingView = service.setFilter(ViewId{999}, "foo");
      REQUIRE_FALSE(missingView);
      CHECK(missingView.error().code == Error::Code::NotFound);
    }
  }

  TEST_CASE("ViewService - openListInView with active filter preserves filter state", "[runtime][unit][view][filter]")
  {
    auto env = ViewServiceFixture{};
    auto const oldTrackId = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "Old", .year = 1999});
    auto const newTrackId = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "New", .year = 2021});
    env.cachePtr->reloadAllTracks();

    auto const oldListId = ao::test::requireValue(env.writer().createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Old only",
      .trackIds = {oldTrackId},
    }));

    auto service = env.makeService();
    auto const result = env.requireView(service, {.filterExpression = "$year > 2000"});
    auto const initialProjectionPtr = service.trackListProjection(result);

    REQUIRE(initialProjectionPtr != nullptr);
    REQUIRE(initialProjectionPtr->size() == 1);
    CHECK(initialProjectionPtr->trackIdAt(0) == newTrackId);

    REQUIRE(service.openListInView(result, oldListId));
    auto const snap = service.trackListState(result);
    auto const projectionPtr = service.trackListProjection(result);

    REQUIRE(projectionPtr != nullptr);
    CHECK(snap.listId == oldListId);
    CHECK(snap.filterExpression == "$year > 2000");
    CHECK(projectionPtr != initialProjectionPtr);
    CHECK(projectionPtr->size() == 0);
  }
} // namespace ao::rt::test
