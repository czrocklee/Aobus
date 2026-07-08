// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/LibraryNavigation.h"

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
      {.id = ListId{2}, .parentId = ListId{1}, .name = "Favorites", .kind = rt::ListNodeKind::Manual},
      {.id = ListId{1}, .name = "Playlists", .kind = rt::ListNodeKind::Folder},
      {.id = ListId{3}, .name = "Live", .kind = rt::ListNodeKind::Smart, .smartExpression = "$title ~ \"Live\""},
    };

    auto items = makeLibraryNavigation(lists);
    auto labels = libraryNavigationLabels(items);

    REQUIRE(items.size() == 4);
    CHECK(items[0].id == rt::kAllTracksListId);
    CHECK(items[0].label == "All Tracks");
    CHECK(items[1].label == "[?] Live");
    CHECK(items[1].detail == "[$title ~ \"Live\"]");
    CHECK(items[2].label == "[+] Playlists");
    CHECK(items[3].label == "  [#] Favorites");
    CHECK(labels[1] == "[?] Live [$title ~ \"Live\"]");
  }

  TEST_CASE("LibraryNavigation - caps cyclic parent depth", "[tui][regression][library-navigation]")
  {
    auto lists = std::vector<rt::ListNode>{
      {.id = ListId{1}, .parentId = ListId{2}, .name = "One", .kind = rt::ListNodeKind::Folder},
      {.id = ListId{2}, .parentId = ListId{1}, .name = "Two", .kind = rt::ListNodeKind::Folder},
    };

    auto items = makeLibraryNavigation(lists);

    REQUIRE(items.size() == 3);
    CHECK((items[1].id == ListId{1} || items[2].id == ListId{1}));
    CHECK((items[1].id == ListId{2} || items[2].id == ListId{2}));
  }
} // namespace ao::tui::test
