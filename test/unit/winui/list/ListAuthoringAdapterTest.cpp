// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/list/ListAuthoringAdapter.h>

#include "test/unit/uimodel/library/presentation/TrackPresentationTestSupport.h"
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <vector>

namespace ao::winui::test
{
  TEST_CASE("List authoring presentation - applies an explicit preference or the saved-List recommendation",
            "[winui][regression][list-authoring]")
  {
    auto fixture = uimodel::test::TrackPresentationFixture{};
    auto const listId = ListId{71};

    fixture.listPresentations.setPresentationIdForList(listId, rt::kListOrderTrackPresentationId);
    CHECK(resolveListAuthoringPresentation(fixture.listPresentations, listId, "$composer = 'Bach'").id ==
          rt::kListOrderTrackPresentationId);

    fixture.listPresentations.clearPresentationForList(listId);
    CHECK(resolveListAuthoringPresentation(fixture.listPresentations, listId, "$composer = 'Bach'").id ==
          "classical-composers");
  }

  TEST_CASE("List tree invalidation - ignores track-only publications", "[winui][unit][list-authoring]")
  {
    CHECK_FALSE(listTreeChangeRequiresRebuild(rt::LibraryChangeSet{.tracksMutated = {TrackId{4}}}));
    CHECK_FALSE(listTreeChangeRequiresRebuild(
      rt::LibraryChangeSet{.listOrderChanges = {{.listId = ListId{7}, .operation = rt::ListOrderReset{}}}}));
    CHECK(listTreeChangeRequiresRebuild(rt::LibraryChangeSet{.listsUpserted = {ListId{7}}}));
    CHECK(listTreeChangeRequiresRebuild(rt::LibraryChangeSet{.listsDeleted = {ListId{7}}}));
    CHECK(listTreeChangeRequiresRebuild(rt::LibraryChangeSet{.libraryReset = true}));
  }

  TEST_CASE("List tree restoration - preserves surviving expansion and chooses a stable fallback",
            "[winui][unit][list-authoring]")
  {
    auto projection = uimodel::ListTreeProjection{};
    projection.rootIds = {rt::kAllTracksListId, ListId{10}};
    projection.rowsById.emplace(
      rt::kAllTracksListId, uimodel::ListTreeProjectionRow{.id = rt::kAllTracksListId, .name = "All Tracks"});
    projection.rowsById.emplace(
      ListId{10}, uimodel::ListTreeProjectionRow{.id = ListId{10}, .name = "Parent", .childIds = {ListId{11}}});
    projection.rowsById.emplace(ListId{11}, uimodel::ListTreeProjectionRow{.id = ListId{11}, .name = "Child"});

    auto const state =
      restoreListTreeState(projection, ListId{99}, std::map<ListId, bool>{{ListId{10}, false}, {ListId{99}, true}});

    CHECK(state.selectedListId == rt::kAllTracksListId);
    CHECK(state.expandedById ==
          std::map<ListId, bool>{{ListId{10}, false}, {ListId{11}, false}, {rt::kAllTracksListId, false}});
  }

  TEST_CASE("List tree restoration - keeps an active surviving List and expands a new parent",
            "[winui][unit][list-authoring]")
  {
    auto projection = uimodel::ListTreeProjection{};
    projection.rootIds = {ListId{20}};
    projection.rowsById.emplace(
      ListId{20}, uimodel::ListTreeProjectionRow{.id = ListId{20}, .name = "New parent", .childIds = {ListId{21}}});
    projection.rowsById.emplace(ListId{21}, uimodel::ListTreeProjectionRow{.id = ListId{21}, .name = "New child"});

    auto const state = restoreListTreeState(projection, ListId{21}, {});
    auto const expectedExpansion = std::map<ListId, bool>{{ListId{20}, true}, {ListId{21}, false}};
    CHECK(state.selectedListId == ListId{21});
    CHECK(state.expandedById == expectedExpansion);
  }

  TEST_CASE("List tree restoration - reveals an active nested List beneath a previously collapsed parent",
            "[winui][unit][list-authoring]")
  {
    auto projection = uimodel::ListTreeProjection{};
    projection.rootIds = {ListId{30}};
    projection.rowsById.emplace(
      ListId{30}, uimodel::ListTreeProjectionRow{.id = ListId{30}, .name = "Parent", .childIds = {ListId{31}}});
    projection.rowsById.emplace(
      ListId{31}, uimodel::ListTreeProjectionRow{.id = ListId{31}, .parentId = ListId{30}, .name = "Active child"});

    auto const state =
      restoreListTreeState(projection, ListId{31}, std::map<ListId, bool>{{ListId{30}, false}, {ListId{31}, false}});
    auto const expectedExpansion = std::map<ListId, bool>{{ListId{30}, true}, {ListId{31}, false}};

    CHECK(state.selectedListId == ListId{31});
    CHECK(state.expandedById == expectedExpansion);
  }
} // namespace ao::winui::test
