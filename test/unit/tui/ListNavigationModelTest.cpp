// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/ListNavigationModel.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    uimodel::ListTreeProjection navigationTree(std::string childName = "Child")
    {
      auto tree = uimodel::ListTreeProjection{.rootIds = {rt::kAllTracksListId, ListId{1}}, .rowsById = {}};
      tree.rowsById.emplace(
        rt::kAllTracksListId,
        uimodel::ListTreeProjectionRow{.id = rt::kAllTracksListId, .name = "All Tracks", .isSystem = true});
      tree.rowsById.emplace(
        ListId{1}, uimodel::ListTreeProjectionRow{.id = ListId{1}, .name = "Parent", .childIds = {ListId{2}}});
      tree.rowsById.emplace(
        ListId{2},
        uimodel::ListTreeProjectionRow{
          .id = ListId{2}, .parentId = ListId{1}, .name = std::move(childName), .localExpression = "genre:rock"});
      return tree;
    }

    uimodel::ListTreeProjection flatTree()
    {
      auto tree =
        uimodel::ListTreeProjection{.rootIds = {rt::kAllTracksListId, ListId{1}, ListId{2}, ListId{3}}, .rowsById = {}};
      tree.rowsById.emplace(
        rt::kAllTracksListId,
        uimodel::ListTreeProjectionRow{.id = rt::kAllTracksListId, .name = "All Tracks", .isSystem = true});

      for (std::uint32_t id = 1; id <= 3; ++id)
      {
        tree.rowsById.emplace(
          ListId{id}, uimodel::ListTreeProjectionRow{.id = ListId{id}, .name = "List " + std::to_string(id)});
      }

      return tree;
    }

    uimodel::ListTreeProjection contextDenseTree()
    {
      auto tree = uimodel::ListTreeProjection{};

      for (std::uint32_t index = 0; index < 4; ++index)
      {
        auto const parentId = ListId{10 + index};
        auto const childId = ListId{20 + index};
        tree.rootIds.push_back(parentId);
        tree.rowsById.emplace(parentId,
                              uimodel::ListTreeProjectionRow{
                                .id = parentId, .name = "Group " + std::to_string(index), .childIds = {childId}});
        tree.rowsById.emplace(childId,
                              uimodel::ListTreeProjectionRow{
                                .id = childId, .parentId = parentId, .name = "Needle " + std::to_string(index)});
      }

      return tree;
    }
  } // namespace

  TEST_CASE("ListNavigationModel - initial active path is visible with shared preorder", "[tui][unit][list-navigation]")
  {
    auto model = ListNavigationModel{};
    model.setTree(navigationTree(), ListId{2});

    REQUIRE(model.rows().size() == 3);
    CHECK(model.rows()[0].id == rt::kAllTracksListId);
    CHECK(model.rows()[0].depth == 0);
    CHECK(model.rows()[1].id == ListId{1});
    CHECK(model.rows()[1].depth == 0);
    CHECK(model.rows()[1].expanded);
    CHECK(model.rows()[2].id == ListId{2});
    CHECK(model.rows()[2].depth == 1);
    CHECK(model.rows()[2].detail == "genre:rock");
    CHECK(model.rows()[2].path == "Parent / Child");
    CHECK(model.cursor() == ListId{2});
    CHECK(model.selectedIndex() == 2);
    CHECK(model.activationTarget() == ListId{2});
  }

  TEST_CASE("ListNavigationModel - tree replacement preserves IDs and ancestor fallback",
            "[tui][unit][list-navigation]")
  {
    auto model = ListNavigationModel{};
    model.setTree(navigationTree(), ListId{2});

    auto renamed = navigationTree("Renamed");
    renamed.rootIds = {ListId{1}, rt::kAllTracksListId};
    model.setTree(std::move(renamed), rt::kAllTracksListId);

    REQUIRE(model.rows().size() == 3);
    CHECK(model.cursor() == ListId{2});
    CHECK(model.rows()[1].name == "Renamed");
    CHECK(model.rows()[1].path == "Parent / Renamed");

    auto withoutChild = navigationTree();
    withoutChild.rowsById.at(ListId{1}).childIds.clear();
    withoutChild.rowsById.erase(ListId{2});
    model.setTree(std::move(withoutChild), rt::kAllTracksListId);

    CHECK(model.cursor() == ListId{1});
    CHECK(model.activationTarget() == ListId{1});

    auto activeFallback = flatTree();
    activeFallback.rowsById.erase(ListId{1});
    activeFallback.rootIds = {rt::kAllTracksListId, ListId{2}, ListId{3}};
    model.setTree(std::move(activeFallback), ListId{3});

    CHECK(model.cursor() == ListId{3});

    auto allTracksFallback = flatTree();
    allTracksFallback.rowsById.erase(ListId{3});
    allTracksFallback.rootIds = {rt::kAllTracksListId, ListId{1}, ListId{2}};
    model.setTree(std::move(allTracksFallback), kInvalidListId);

    CHECK(model.cursor() == rt::kAllTracksListId);

    auto firstCandidate = uimodel::ListTreeProjection{.rootIds = {ListId{8}}, .rowsById = {}};
    firstCandidate.rowsById.emplace(
      ListId{8}, uimodel::ListTreeProjectionRow{.id = ListId{8}, .name = "Only remaining List"});
    model.setTree(std::move(firstCandidate), kInvalidListId);

    CHECK(model.cursor() == ListId{8});
  }

  TEST_CASE("ListNavigationModel - expand and collapse follow cursor semantics", "[tui][unit][list-navigation]")
  {
    auto model = ListNavigationModel{};
    model.setTree(navigationTree(), rt::kAllTracksListId);
    CHECK_FALSE(model.trySelect(ListId{2}));
    REQUIRE(model.trySelect(ListId{1}));

    model.expand();

    REQUIRE(model.rows().size() == 3);
    CHECK(model.cursor() == ListId{1});
    CHECK(model.rows()[1].expanded);

    model.expand();

    CHECK(model.cursor() == ListId{2});
    model.expand();
    CHECK(model.cursor() == ListId{2});

    model.collapse();

    CHECK(model.cursor() == ListId{1});
    model.collapse();
    CHECK(model.cursor() == ListId{1});
    CHECK(model.rows().size() == 2);

    model.collapse();

    CHECK(model.cursor() == ListId{1});
    REQUIRE(model.trySelect(rt::kAllTracksListId));
    model.expand();
    model.collapse();
    CHECK(model.cursor() == rt::kAllTracksListId);
  }

  TEST_CASE("ListNavigationModel - path expansion preserves cursor and reveal selects by ID",
            "[tui][unit][list-navigation]")
  {
    auto model = ListNavigationModel{};
    model.setTree(navigationTree(), rt::kAllTracksListId);

    model.expandPath(ListId{2});

    REQUIRE(model.rows().size() == 3);
    CHECK(model.cursor() == rt::kAllTracksListId);
    CHECK(model.rows()[1].expanded);

    model.toggleExpanded(ListId{1});

    CHECK(model.rows().size() == 2);
    CHECK(model.cursor() == rt::kAllTracksListId);

    model.reveal(ListId{2});

    CHECK(model.rows().size() == 3);
    CHECK(model.cursor() == ListId{2});
    CHECK(model.activationTarget() == ListId{2});
    model.reveal(ListId{999});
    CHECK(model.cursor() == ListId{2});
  }

  TEST_CASE("ListNavigationModel - collapsing an ancestor moves a descendant cursor to the visible row",
            "[tui][unit][list-navigation]")
  {
    auto model = ListNavigationModel{};
    model.setTree(navigationTree(), ListId{2});

    model.toggleExpanded(ListId{1});

    REQUIRE(model.rows().size() == 2);
    CHECK(model.cursor() == ListId{1});
    CHECK(model.selectedIndex() == 1);
    CHECK(model.activationTarget() == ListId{1});
  }

  TEST_CASE("ListNavigationModel - tree replacement reveals a surviving cursor under its new parent",
            "[tui][unit][list-navigation]")
  {
    auto model = ListNavigationModel{};
    model.setTree(navigationTree(), ListId{2});

    auto moved = navigationTree();
    moved.rootIds.emplace_back(3);
    moved.rowsById.at(ListId{1}).childIds.clear();
    moved.rowsById.at(ListId{2}).parentId = ListId{3};
    moved.rowsById.emplace(
      ListId{3}, uimodel::ListTreeProjectionRow{.id = ListId{3}, .name = "New Parent", .childIds = {ListId{2}}});

    model.setTree(std::move(moved), rt::kAllTracksListId);

    REQUIRE(model.rows().size() == 4);
    CHECK(model.rows()[2].id == ListId{3});
    CHECK(model.rows()[2].expanded);
    CHECK(model.rows()[3].id == ListId{2});
    CHECK(model.rows()[3].path == "New Parent / Child");
    CHECK(model.cursor() == ListId{2});
    CHECK(model.selectedIndex() == 3);
    CHECK(model.activationTarget() == ListId{2});
  }

  TEST_CASE("ListNavigationModel - search reveals hidden matches with ancestor context", "[tui][unit][list-navigation]")
  {
    auto model = ListNavigationModel{};
    model.setTree(navigationTree("Needle"), rt::kAllTracksListId);
    REQUIRE(model.rows().size() == 2);

    REQUIRE(model.trySearchEvent(ftxui::Event::Character("/")));
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("needle")));

    REQUIRE(model.rows().size() == 2);
    CHECK(model.rows()[0].id == ListId{1});
    CHECK_FALSE(model.rows()[0].matching);
    CHECK(model.rows()[0].expanded);
    CHECK(model.rows()[1].id == ListId{2});
    CHECK(model.rows()[1].matching);
    CHECK_FALSE(model.trySelect(ListId{1}));
    CHECK(model.cursor() == ListId{2});
    CHECK(model.activationTarget() == ListId{2});

    REQUIRE(model.trySearchEvent(ftxui::Event::Escape));

    CHECK_FALSE(model.search().isActive());
    REQUIRE(model.rows().size() == 3);
    CHECK(model.rows()[1].expanded);
    CHECK(model.cursor() == ListId{2});
  }

  TEST_CASE("ListNavigationModel - search uses paths and rejects empty results", "[tui][unit][list-navigation]")
  {
    auto model = ListNavigationModel{};
    model.setTree(navigationTree("Live"), rt::kAllTracksListId);
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("/")));
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("parent")));

    REQUIRE(model.rows().size() == 2);
    CHECK(model.rows()[0].matching);
    CHECK(model.rows()[1].matching);
    CHECK(model.rows()[1].path == "Parent / Live");

    model.clearSearch();
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("/")));
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("genre:rock")));

    CHECK(model.rows().empty());
    CHECK_FALSE(model.activationTarget());

    model.clearSearch();
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("/")));
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("missing")));

    CHECK(model.rows().empty());
    CHECK(model.selectedIndex() == -1);
    CHECK_FALSE(model.activationTarget());
    auto const retainedCursor = model.cursor();
    model.move(std::numeric_limits<std::int32_t>::max());
    CHECK(model.cursor() == retainedCursor);

    REQUIRE(model.trySearchEvent(ftxui::Event::Escape));
    CHECK_FALSE(model.rows().empty());
    CHECK(model.activationTarget() == retainedCursor);
  }

  TEST_CASE("ListNavigationModel - tree replacement refreshes folded names and paths during search",
            "[tui][regression][list-navigation]")
  {
    auto model = ListNavigationModel{};
    model.setTree(navigationTree("Straße"), rt::kAllTracksListId);
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("/")));
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("STRASSE")));
    CHECK(model.activationTarget() == ListId{2});

    model.setTree(navigationTree("Café"), rt::kAllTracksListId);
    CHECK(model.rows().empty());
    CHECK_FALSE(model.activationTarget());
    model.clearSearch();
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("/")));
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("CAFE\u0301")));
    CHECK(model.activationTarget() == ListId{2});

    model.clearSearch();
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("/")));
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("STRASSE / CAFÉ")));
    CHECK(model.rows().empty());
    auto renamed = navigationTree("Café");
    renamed.rowsById.at(ListId{1}).name = "Straße";
    model.setTree(std::move(renamed), rt::kAllTracksListId);
    REQUIRE(model.rows().size() == 2);
    CHECK(model.rows()[1].path == "Straße / Café");
    CHECK(model.activationTarget() == ListId{2});

    auto moved = navigationTree("Café");
    moved.rowsById.at(ListId{1}).childIds.clear();
    moved.rowsById.at(ListId{2}).parentId = ListId{3};
    moved.rootIds.emplace_back(3);
    moved.rowsById.emplace(
      ListId{3}, uimodel::ListTreeProjectionRow{.id = ListId{3}, .name = "Elsewhere", .childIds = {ListId{2}}});
    model.setTree(std::move(moved), rt::kAllTracksListId);
    CHECK(model.rows().empty());
    model.clearSearch();
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("/")));
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("ELSEWHERE / CAFE\u0301")));
    REQUIRE(model.rows().size() == 2);
    CHECK(model.rows()[1].path == "Elsewhere / Café");
    CHECK(model.activationTarget() == ListId{2});
  }

  TEST_CASE("ListNavigationModel - search caret and list movement events retain distinct ownership",
            "[tui][unit][list-navigation]")
  {
    auto model = ListNavigationModel{};
    model.setTree(navigationTree("Needle"), rt::kAllTracksListId);
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("/")));
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("needle")));
    REQUIRE(model.activationTarget() == ListId{2});
    auto const expectedRows = model.rows().size();
    auto const expectedRevision = model.revision();

    CHECK_FALSE(model.trySearchEvent(ftxui::Event::ArrowDown));
    CHECK(model.rows().size() == expectedRows);
    CHECK(model.cursor() == ListId{2});
    CHECK(model.revision() == expectedRevision);

    for (auto const& event : {ftxui::Event::ArrowLeft, ftxui::Event::ArrowRight})
    {
      REQUIRE(model.trySearchEvent(event));
      CHECK(model.search().query() == "needle");
      CHECK(model.rows().size() == expectedRows);
      CHECK(model.activationTarget() == ListId{2});
      CHECK(model.revision() == expectedRevision);
    }
  }

  TEST_CASE("ListNavigationModel - visible row selection follows raw search rows and directional context",
            "[tui][unit][list-navigation]")
  {
    auto model = ListNavigationModel{};
    model.setTree(contextDenseTree(), ListId{10});
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("/")));
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("needle")));

    REQUIRE(model.rows().size() == 8);
    CHECK(model.rows()[1].matching);
    CHECK(model.rows()[3].matching);
    CHECK(model.rows()[5].matching);
    CHECK(model.rows()[7].matching);

    model.selectVisibleRow(3);
    REQUIRE(model.selectedIndex() == 3);
    model.selectVisibleRow(5);
    CHECK(model.selectedIndex() == 5);

    model.selectVisibleRow(3);
    model.selectVisibleRow(4);
    CHECK(model.selectedIndex() == 5);

    model.selectVisibleRow(3);
    model.selectVisibleRow(2);
    CHECK(model.selectedIndex() == 1);

    model.selectVisibleRow(3);
    model.selectVisibleRow(std::numeric_limits<std::int32_t>::min());
    CHECK(model.selectedIndex() == 1);

    model.selectVisibleRow(3);
    model.selectVisibleRow(std::numeric_limits<std::int32_t>::max());
    CHECK(model.selectedIndex() == 7);

    model.clearSearch();
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("/")));
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("absent")));
    CHECK(model.rows().empty());
    CHECK_FALSE(model.activationTarget());
    model.selectVisibleRow(0);
    CHECK_FALSE(model.activationTarget());
  }

  TEST_CASE("ListNavigationModel - search disclosure leaves normal expansion unchanged", "[tui][unit][list-navigation]")
  {
    auto model = ListNavigationModel{};
    model.setTree(navigationTree(), rt::kAllTracksListId);
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("/")));
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("parent")));
    REQUIRE(model.rows().size() == 2);
    REQUIRE(model.rows()[0].id == ListId{1});
    REQUIRE(model.rows()[0].expanded);
    auto const expectedRevision = model.revision();

    model.toggleExpanded(ListId{1});

    CHECK(model.revision() == expectedRevision);
    CHECK(model.rows()[0].expanded);

    model.clearSearch();

    REQUIRE(model.rows().size() == 2);
    CHECK(model.rows()[1].id == ListId{1});
    CHECK_FALSE(model.rows()[1].expanded);
  }

  TEST_CASE("ListNavigationModel - movement clamps at candidate bounds", "[tui][unit][list-navigation]")
  {
    auto model = ListNavigationModel{};
    model.setTree(flatTree(), rt::kAllTracksListId);

    model.move(std::numeric_limits<std::int32_t>::max());
    CHECK(model.cursor() == ListId{3});
    model.move(1);
    CHECK(model.cursor() == ListId{3});
    model.move(std::numeric_limits<std::int32_t>::min());
    CHECK(model.cursor() == rt::kAllTracksListId);
    model.move(-1);
    CHECK(model.cursor() == rt::kAllTracksListId);
    model.move(2);
    CHECK(model.cursor() == ListId{2});
  }

  TEST_CASE("ListNavigationModel - shared cycle repair remains finite and ordered",
            "[tui][regression][list-navigation]")
  {
    auto const projection =
      uimodel::buildListTreeProjection(ao::test::englishMessageCatalog(),
                                       std::vector{
                                         rt::ListNode{.id = ListId{7}, .parentId = ListId{4}, .name = "Higher"},
                                         rt::ListNode{.id = ListId{2}, .parentId = ListId{7}, .name = "Descendant"},
                                         rt::ListNode{.id = ListId{4}, .parentId = ListId{7}, .name = "Lower"},
                                       });
    auto model = ListNavigationModel{};

    model.setTree(projection, ListId{2});

    REQUIRE(model.rows().size() == 4);
    CHECK(model.rows()[0].id == rt::kAllTracksListId);
    CHECK(model.rows()[1].id == ListId{4});
    CHECK(model.rows()[2].id == ListId{7});
    CHECK(model.rows()[3].id == ListId{2});
    CHECK(model.rows()[3].depth == 2);
    CHECK(model.cursor() == ListId{2});
  }

  TEST_CASE("ListNavigationModel - deep trees rebuild without recursive traversal", "[tui][unit][list-navigation]")
  {
    constexpr std::uint32_t kDepth = 2048;
    auto tree = uimodel::ListTreeProjection{.rootIds = {ListId{1}}, .rowsById = {}};

    for (std::uint32_t id = 1; id <= kDepth; ++id)
    {
      auto const childId = id == kDepth ? kInvalidListId : ListId{id + 1};
      tree.rowsById.emplace(ListId{id},
                            uimodel::ListTreeProjectionRow{
                              .id = ListId{id},
                              .parentId = id == 1 ? kInvalidListId : ListId{id - 1},
                              .name = "n",
                              .childIds = childId == kInvalidListId ? std::vector<ListId>{} : std::vector{childId}});
    }

    auto model = ListNavigationModel{};
    model.setTree(std::move(tree), ListId{kDepth});

    REQUIRE(model.rows().size() == kDepth);
    CHECK(model.rows().back().id == ListId{kDepth});
    CHECK(model.rows().back().depth == kDepth - 1);
    CHECK(model.cursor() == ListId{kDepth});
  }
} // namespace ao::tui::test
