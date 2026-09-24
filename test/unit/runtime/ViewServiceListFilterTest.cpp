// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/runtime/ViewServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
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

    auto projectionChanges = std::vector<TrackListProjectionChanged>{};
    auto projSub = service.onProjectionChanged([&](TrackListProjectionChanged const& changed) noexcept
                                               { projectionChanges.push_back(changed); });

    SECTION("setting a new filter expression creates adHocSource")
    {
      REQUIRE(service.setFilter(result, "$year > 2000"));
      env.drainCallbacks();
      auto const snap = service.trackListState(result);
      auto const filteredProjectionPtr = env.requireProjection(result);

      REQUIRE(filteredProjectionPtr != nullptr);
      CHECK(snap.filterExpression == "$year > 2000");
      CHECK_FALSE(snap.optFilterError);
      REQUIRE(projectionChanges.size() == 1);
      CHECK(projectionChanges[0].viewId == result);
      CHECK(projectionChanges[0].projectionPtr == filteredProjectionPtr);
      REQUIRE(filteredProjectionPtr->size() == 1);
      CHECK(filteredProjectionPtr->trackIdAt(0) == newTrackId);

      REQUIRE(service.setFilter(result, "$year > 2000"));
      env.drainCallbacks();
      auto const stateAfterNoOp = service.trackListState(result);
      CHECK(stateAfterNoOp.id == snap.id);
      CHECK(stateAfterNoOp.listId == snap.listId);
      CHECK(stateAfterNoOp.filterExpression == snap.filterExpression);
      CHECK(stateAfterNoOp.optFilterError.has_value() == snap.optFilterError.has_value());
      CHECK(stateAfterNoOp.groupBy == snap.groupBy);
      CHECK(stateAfterNoOp.sortBy == snap.sortBy);
      CHECK(stateAfterNoOp.selection == snap.selection);
      CHECK(stateAfterNoOp.presentation == snap.presentation);
      CHECK(env.requireProjection(result) == filteredProjectionPtr);
      CHECK(projectionChanges.size() == 1);

      REQUIRE(service.setFilter(result, "$year > 2025"));
      env.drainCallbacks();
      auto const snap2 = service.trackListState(result);
      auto const updatedFilteredProjectionPtr = env.requireProjection(result);
      CHECK(snap2.filterExpression == "$year > 2025");
      CHECK_FALSE(snap2.optFilterError);
      REQUIRE(projectionChanges.size() == 2);
      CHECK(projectionChanges[1].viewId == result);
      CHECK(projectionChanges[1].projectionPtr == updatedFilteredProjectionPtr);
      REQUIRE(updatedFilteredProjectionPtr != nullptr);
      CHECK(updatedFilteredProjectionPtr != filteredProjectionPtr);
      CHECK(updatedFilteredProjectionPtr->size() == 0);

      REQUIRE(service.setFilter(result, ""));
      env.drainCallbacks();
      auto const snap3 = service.trackListState(result);
      auto const unfilteredProjectionPtr = env.requireProjection(result);
      CHECK(snap3.filterExpression.empty());
      CHECK_FALSE(snap3.optFilterError);
      REQUIRE(projectionChanges.size() == 3);
      CHECK(projectionChanges[2].viewId == result);
      CHECK(projectionChanges[2].projectionPtr == unfilteredProjectionPtr);
      REQUIRE(unfilteredProjectionPtr != nullptr);
      CHECK(unfilteredProjectionPtr != filteredProjectionPtr);
      REQUIRE(unfilteredProjectionPtr->size() == 2);
      CHECK(unfilteredProjectionPtr->indexOf(oldTrackId).has_value());
      CHECK(unfilteredProjectionPtr->indexOf(newTrackId).has_value());
    }

    SECTION("invalid expression is retained with its synchronous error")
    {
      REQUIRE(service.setFilter(result, "$year >"));
      env.drainCallbacks();
      auto const snap = service.trackListState(result);
      auto const filteredProjectionPtr = env.requireProjection(result);

      CHECK(snap.filterExpression == "$year >");
      REQUIRE(snap.optFilterError);
      CHECK(snap.optFilterError->code == Error::Code::FormatRejected);
      REQUIRE(filteredProjectionPtr != nullptr);
      CHECK(filteredProjectionPtr->size() == 0);
      REQUIRE(projectionChanges.size() == 1);
      CHECK(projectionChanges[0].viewId == result);
      CHECK(projectionChanges[0].projectionPtr == filteredProjectionPtr);
    }

    SECTION("invalid view ID is safe")
    {
      auto const missingViewRes = service.setFilter(ViewId{999}, "foo");
      REQUIRE_FALSE(missingViewRes);
      CHECK(missingViewRes.error().code == Error::Code::NotFound);
    }
  }

  TEST_CASE("ViewService - queued projection change retains its payload after the view closes",
            "[runtime][unit][view][filter][async]")
  {
    auto env = ViewServiceFixture{};
    auto const oldTrackId = env.addTrack(library::test::TrackSpec{.title = "Old", .year = 1999});
    auto const newTrackId = env.addTrack(library::test::TrackSpec{.title = "New", .year = 2021});
    env.cachePtr->reloadAllTracks();
    auto const viewId = env.requireView();
    auto changes = std::vector<TrackListProjectionChanged>{};
    auto const sub = env.service.onProjectionChanged([&](TrackListProjectionChanged const& changed) noexcept
                                                     { changes.push_back(changed); });

    REQUIRE(env.service.setFilter(viewId, "$year > 2000"));
    CHECK(changes.empty());
    REQUIRE(env.workspace.closeView(viewId));
    CHECK(changes.empty());
    auto const missingStateRes = env.service.findTrackListState(viewId);
    auto const missingProjectionRes = env.service.findTrackListProjection(viewId);
    REQUIRE_FALSE(missingStateRes);
    CHECK(missingStateRes.error().code == Error::Code::NotFound);
    REQUIRE_FALSE(missingProjectionRes);
    CHECK(missingProjectionRes.error().code == Error::Code::NotFound);

    env.drainCallbacks();

    REQUIRE(changes.size() == 1);
    CHECK(changes[0].viewId == viewId);
    REQUIRE(changes[0].projectionPtr != nullptr);
    REQUIRE(changes[0].projectionPtr->size() == 1);
    CHECK(changes[0].projectionPtr->trackIdAt(0) == newTrackId);
    CHECK_FALSE(changes[0].projectionPtr->indexOf(oldTrackId));
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

  TEST_CASE("ViewService - repairing a stored parent filter refreshes live child error state",
            "[runtime][unit][view][filter]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto parentId = kInvalidListId;
    auto childId = kInvalidListId;

    {
      auto transaction = library::test::writeTransaction(libraryFixture.library());
      auto parentBuilder = library::ListBuilder::makeEmpty().name("Parent").filter("(");
      parentId = ao::test::requireValue(transaction.apply([&parentBuilder](library::LibraryWrite& write)
                                                          { return write.lists().create(parentBuilder); }));
      auto childBuilder = library::ListBuilder::makeEmpty().name("Child").parentId(parentId);
      childId = ao::test::requireValue(transaction.apply([&childBuilder](library::LibraryWrite& write)
                                                         { return write.lists().create(childBuilder); }));
      REQUIRE(transaction.commit());
    }

    auto executor = async::LoopExecutor{};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes, executor};
    auto sources = TrackSourceCache{libraryFixture.library(), changes};
    auto service = ViewService{executor, libraryFixture.library(), sources, changes};
    auto workspace = WorkspaceService{executor, service, changes};
    auto const viewId = ao::test::requireValue(workspace.navigate(NavigationRequest{
      .target = FilteredListTarget{.listId = childId, .filterExpression = {}},
    }));

    while (executor.tryRunReadyTurn())
    {
    }

    auto state = service.trackListState(viewId);
    REQUIRE(state.optFilterError);
    CHECK(state.optFilterError->code == Error::Code::FormatRejected);

    auto changedErrors = std::vector<ViewService::FilterErrorChanged>{};
    auto subscription = service.onFilterErrorChanged([&changedErrors](ViewService::FilterErrorChanged const& changed)
                                                     { changedErrors.push_back(changed); });
    REQUIRE(commandsFixture.runTask(commandsFixture.commands().updateListAsync(
      ListDraft{.listId = parentId, .name = "Parent", .expression = "true"})));

    state = service.trackListState(viewId);
    CHECK_FALSE(state.optFilterError);
    REQUIRE(changedErrors.size() == 1);
    CHECK(changedErrors.back().viewId == viewId);
    CHECK_FALSE(changedErrors.back().optFilterError);
  }
} // namespace ao::rt::test
