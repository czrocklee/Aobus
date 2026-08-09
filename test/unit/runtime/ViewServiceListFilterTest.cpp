// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "runtime/library/LibraryMutationService.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/ViewServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("ViewService - setFilter updates filter state and projection", "[runtime][unit][view][filter]")
  {
    auto env = ViewServiceFixture{};
    auto const oldTrackId = env.addTrack(library::test::TrackSpec{.title = "Old", .year = 1999});
    auto const newTrackId = env.addTrack(library::test::TrackSpec{.title = "New", .year = 2021});
    env.cachePtr->reloadAllTracks();

    auto& service = env.service;
    auto const result = env.requireView();

    auto projView = kInvalidViewId;
    std::int32_t projectionChangedCount = 0;
    auto projSub = service.onProjectionChanged(
      [&](auto const& ev) noexcept
      {
        projView = ev.viewId;
        ++projectionChangedCount;
      });

    SECTION("setting a new filter expression creates adHocSource")
    {
      REQUIRE(service.setFilter(result, "$year > 2000"));
      auto const snap = service.trackListState(result);
      auto const filteredProjectionPtr = env.requireProjection(result);

      REQUIRE(filteredProjectionPtr != nullptr);
      CHECK(snap.filterExpression == "$year > 2000");
      CHECK_FALSE(snap.optFilterError);
      CHECK(projView == result);
      CHECK(projectionChangedCount == 1);
      REQUIRE(filteredProjectionPtr->size() == 1);
      CHECK(filteredProjectionPtr->trackIdAt(0) == newTrackId);

      REQUIRE(service.setFilter(result, "$year > 2025"));
      auto const snap2 = service.trackListState(result);
      auto const updatedFilteredProjectionPtr = env.requireProjection(result);
      CHECK(snap2.filterExpression == "$year > 2025");
      CHECK_FALSE(snap2.optFilterError);
      CHECK(projectionChangedCount == 2);
      REQUIRE(updatedFilteredProjectionPtr != nullptr);
      CHECK(updatedFilteredProjectionPtr != filteredProjectionPtr);
      CHECK(updatedFilteredProjectionPtr->size() == 0);

      REQUIRE(service.setFilter(result, ""));
      auto const snap3 = service.trackListState(result);
      auto const unfilteredProjectionPtr = env.requireProjection(result);
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
      auto const filteredProjectionPtr = env.requireProjection(result);

      CHECK(snap.filterExpression == "$year >");
      REQUIRE(snap.optFilterError);
      CHECK(snap.optFilterError->code == Error::Code::FormatRejected);
      REQUIRE(filteredProjectionPtr != nullptr);
      CHECK(filteredProjectionPtr->size() == 0);
      CHECK(projectionChangedCount == 1);
    }

    SECTION("invalid view ID is safe")
    {
      auto const missingViewRes = service.setFilter(ViewId{999}, "foo");
      REQUIRE_FALSE(missingViewRes);
      CHECK(missingViewRes.error().code == Error::Code::NotFound);
    }
  }

  TEST_CASE("ViewService - stored parent filter error reaches child view state", "[runtime][unit][view][filter]")
  {
    auto env = ViewServiceFixture{};
    auto parentId = kInvalidListId;
    auto childId = kInvalidListId;

    {
      auto transaction = library::test::writeTransaction(env.libraryFixture.library());
      auto parentBuilder = library::ListBuilder::makeEmpty().name("Invalid parent").filter("(");
      parentId = ao::test::requireValue(transaction.apply([&parentBuilder](library::LibraryWrite& write)
                                                          { return write.lists().create(parentBuilder); }));
      auto childBuilder = library::ListBuilder::makeEmpty().name("Child").parentId(parentId);
      childId = ao::test::requireValue(transaction.apply([&childBuilder](library::LibraryWrite& write)
                                                         { return write.lists().create(childBuilder); }));
      REQUIRE(transaction.commit());
    }

    auto const viewId = env.requireView(TrackListViewConfig{.listId = childId});
    auto const state = env.service.trackListState(viewId);
    auto const projectionPtr = env.requireProjection(viewId);

    REQUIRE(state.optFilterError);
    CHECK(state.optFilterError->code == Error::Code::FormatRejected);
    CHECK(state.optFilterError->message.contains("List " + std::to_string(parentId.raw()) + " stored filter"));
    REQUIRE(projectionPtr != nullptr);
    CHECK(projectionPtr->size() == 0);
  }

  TEST_CASE("ViewService - stored parent filter mutations refresh live child error state",
            "[runtime][unit][view][filter]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto parentId = kInvalidListId;
    auto childId = kInvalidListId;

    {
      auto transaction = library::test::writeTransaction(libraryFixture.library());
      auto parentBuilder = library::ListBuilder::makeEmpty().name("Parent").filter("true");
      parentId = ao::test::requireValue(transaction.apply([&parentBuilder](library::LibraryWrite& write)
                                                          { return write.lists().create(parentBuilder); }));
      auto childBuilder = library::ListBuilder::makeEmpty().name("Child").parentId(parentId);
      childId = ao::test::requireValue(transaction.apply([&childBuilder](library::LibraryWrite& write)
                                                         { return write.lists().create(childBuilder); }));
      REQUIRE(transaction.commit());
    }

    auto executor = InlineExecutor{};
    auto readTransaction = libraryFixture.library().readTransaction();
    auto changes = LibraryChanges{executor, libraryFixture.library().libraryRevision(readTransaction), "test-library"};
    auto mutationService =
      LibraryMutationService{executor, library::test::requireWritableLibrary(libraryFixture.library()), changes};
    auto sources = TrackSourceCache{libraryFixture.library(), changes};
    auto service = ViewService{executor, libraryFixture.library(), sources, changes};
    auto workspace = WorkspaceService{executor, service, changes};
    auto const viewId = ao::test::requireValue(workspace.navigate(NavigationRequest{
      .target = FilteredListTarget{.listId = childId, .filterExpression = {}},
    }));

    auto changedErrors = std::vector<ViewService::FilterErrorChanged>{};
    auto subscription = service.onFilterErrorChanged([&changedErrors](ViewService::FilterErrorChanged const& changed)
                                                     { changedErrors.push_back(changed); });

    auto updateParentFilter = [&](std::string filter)
    {
      auto mutationRes = mutationService.beginInteractiveMutation();
      REQUIRE(mutationRes);
      auto builder = library::ListBuilder::makeEmpty().name("Parent").filter(std::move(filter));
      REQUIRE(
        mutationRes->apply([&](library::LibraryWrite& write) { return write.lists().update(parentId, builder); }));
      REQUIRE(mutationRes->commit(LibraryChangeSet{.listsUpserted = {parentId}}));
    };

    updateParentFilter("(");

    auto state = service.trackListState(viewId);
    REQUIRE(state.optFilterError);
    CHECK(state.optFilterError->code == Error::Code::FormatRejected);
    REQUIRE(changedErrors.size() == 1);
    CHECK(changedErrors.front().viewId == viewId);
    REQUIRE(changedErrors.front().optFilterError);
    CHECK(changedErrors.front().optFilterError->code == Error::Code::FormatRejected);

    updateParentFilter("true");

    state = service.trackListState(viewId);
    CHECK_FALSE(state.optFilterError);
    REQUIRE(changedErrors.size() == 2);
    CHECK(changedErrors.back().viewId == viewId);
    CHECK_FALSE(changedErrors.back().optFilterError);
  }
} // namespace ao::rt::test
