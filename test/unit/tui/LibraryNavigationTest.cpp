// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/LibraryNavigation.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/VirtualListIds.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::tui::test
{
  TEST_CASE("LibraryNavigation - includes all tracks and list hierarchy", "[tui][unit][library-navigation]")
  {
    auto lists = std::vector<rt::ListNode>{
      {.id = ListId{2}, .parentId = ListId{1}, .name = "Favorites"},
      {.id = ListId{1}, .name = "Playlists"},
      {.id = ListId{3}, .name = "Live", .expression = "$title ~ \"Live\""},
    };

    auto items = makeLibraryNavigation(ao::test::englishMessageCatalog(), lists);
    auto labels = libraryNavigationLabels(items);

    REQUIRE(items.size() == 4);
    CHECK(items[0].id == rt::kAllTracksListId);
    CHECK(items[0].label == "All Tracks");
    CHECK(items[1].label == "[L] Playlists");
    CHECK(items[2].label == "  [L] Favorites");
    CHECK(items[3].label == "[L] Live");
    CHECK(items[3].detail == "[$title ~ \"Live\"]");
    CHECK(labels[3] == "[L] Live [$title ~ \"Live\"]");
  }

  TEST_CASE("LibraryNavigation - uses shared fallback for invalid parents", "[tui][unit][library-navigation]")
  {
    auto lists = std::vector<rt::ListNode>{
      {.id = ListId{9}, .parentId = ListId{999}, .name = "Orphan"},
      {.id = ListId{5}, .name = "Root"},
      {.id = ListId{7}, .parentId = ListId{7}, .name = "Self"},
    };

    auto const items = makeLibraryNavigation(ao::test::englishMessageCatalog(), lists);

    REQUIRE(items.size() == 4);
    CHECK(items[1].label == "[L] Root");
    CHECK(items[2].label == "[L] Self");
    CHECK(items[3].label == "[L] Orphan");
  }

  TEST_CASE("LibraryNavigation - renders the shared cyclic-parent recovery", "[tui][regression][library-navigation]")
  {
    auto lists = std::vector<rt::ListNode>{
      {.id = ListId{1}, .parentId = ListId{2}, .name = "One"},
      {.id = ListId{2}, .parentId = ListId{1}, .name = "Two"},
    };

    auto items = makeLibraryNavigation(ao::test::englishMessageCatalog(), lists);

    REQUIRE(items.size() == 3);
    CHECK(items[1].id == ListId{1});
    CHECK(items[1].label == "[L] One");
    CHECK(items[2].id == ListId{2});
    CHECK(items[2].label == "  [L] Two");
  }
} // namespace ao::tui::test
