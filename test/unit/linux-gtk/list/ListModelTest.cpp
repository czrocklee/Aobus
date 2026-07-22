// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListRowObject.h"
#include "list/ListTreeItem.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/CoreIds.h>

#include <catch2/catch_test_macros.hpp>
#include <glibmm/refptr.h>

namespace ao::gtk::test
{
  TEST_CASE("ListRowObject - exposes list row identity and metadata", "[gtk][unit][list][model]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto const id = ListId{42};
    bool const isSmart = true;
    auto const* const name = "Test List";
    auto const* const filter = "artist:\"Test Artist\"";

    auto const rowPtr = ListRowObject::create(id, isSmart, name, filter);

    CHECK(rowPtr->listId() == id);
    CHECK(rowPtr->isSmart() == isSmart);
    CHECK(rowPtr->name() == name);
    CHECK(rowPtr->filter() == filter);
  }

  TEST_CASE("ListTreeItem - exposes child hierarchy", "[gtk][unit][list][model]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto const row1Ptr = ListRowObject::create(ListId{1}, false, "Parent");
    auto const item1Ptr = ListTreeItem::create(row1Ptr);

    SECTION("initial state")
    {
      CHECK(item1Ptr->row() == row1Ptr);
      CHECK(item1Ptr->listId() == ListId{1});
      CHECK(item1Ptr->children()->get_n_items() == 0);
      CHECK(item1Ptr->hasChildren() == false);
    }

    SECTION("adding children")
    {
      auto const row2Ptr = ListRowObject::create(ListId{2}, false, "Child 1");
      auto const item2Ptr = ListTreeItem::create(row2Ptr);

      auto const childrenPtr = item1Ptr->children();
      childrenPtr->append(item2Ptr);

      REQUIRE(childrenPtr->get_n_items() == 1);
      CHECK(item1Ptr->hasChildren() == true);
      CHECK(childrenPtr->get_item(0) == item2Ptr);
      CHECK(item2Ptr->row() == row2Ptr);
    }
  }
} // namespace ao::gtk::test
