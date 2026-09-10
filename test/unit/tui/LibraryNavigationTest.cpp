// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/LibraryNavigation.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace ao::tui::test
{
  TEST_CASE("LibraryNavigation - chooser rows preserve tree preorder and display hierarchy", "[tui][unit][navigation]")
  {
    auto const parentId = ListId{2};
    auto const childId = ListId{3};
    auto const grandchildId = ListId{4};
    auto const otherRootId = ListId{5};
    auto const tree = uimodel::buildListTreeProjection(
      ao::test::englishMessageCatalog(),
      std::vector{
        rt::ListNode{.id = grandchildId, .parentId = childId, .name = "Grandchild"},
        rt::ListNode{.id = otherRootId, .name = "Other root"},
        rt::ListNode{.id = childId, .parentId = parentId, .name = "Smart child", .expression = "$artist = \"Aimer\""},
        rt::ListNode{.id = parentId, .name = "Parent"},
      });

    auto const entries = makeLibraryNavigation(ao::test::englishMessageCatalog(), tree);

    REQUIRE(entries.size() == 5);
    CHECK(entries[0].id == rt::kAllTracksListId);
    CHECK(entries[0].label == "All Tracks");
    CHECK(entries[0].detail == "library");
    CHECK(entries[1].id == parentId);
    CHECK(entries[1].label == "[L] Parent");
    CHECK(entries[1].detail.empty());
    CHECK(entries[2].id == childId);
    CHECK(entries[2].label == "  [L] Smart child");
    CHECK(entries[2].detail == "[$artist = \"Aimer\"]");
    CHECK(entries[3].id == grandchildId);
    CHECK(entries[3].label == "    [L] Grandchild");
    CHECK(entries[3].detail.empty());
    CHECK(entries[4].id == otherRootId);
    CHECK(entries[4].label == "[L] Other root");
    CHECK(entries[4].detail.empty());
  }

  TEST_CASE("LibraryNavigation - combined labels append only nonempty details", "[tui][unit][navigation]")
  {
    auto const labels = libraryNavigationLabels({
      LibraryNavEntry{.id = ListId{2}, .label = "[L] Plain"},
      LibraryNavEntry{.id = ListId{3}, .label = "[L] Smart", .detail = "[$year >= 2020]"},
    });

    CHECK(labels == std::vector<std::string>{"[L] Plain", "[L] Smart [$year >= 2020]"});
  }
} // namespace ao::tui::test
