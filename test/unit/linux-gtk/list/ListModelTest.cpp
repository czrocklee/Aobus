// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListRowObject.h"
#include "list/ListTreeItem.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/Type.h>

#include <catch2/catch_test_macros.hpp>
#include <glibmm/refptr.h>

namespace ao::gtk::test
{
  TEST_CASE("ListRowObject exposes list row identity and metadata", "[gtk][unit][list][model]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto const id = ListId{42};
    auto const parentId = ListId{10};
    int const depth = 2;
    bool const isSmart = true;
    auto const* const name = "Test List";
    auto const* const filter = "artist:\"Test Artist\"";

    auto const rowPtr = ListRowObject::create(id, parentId, depth, isSmart, name, filter);

    SECTION("initial values are correct")
    {
      CHECK(rowPtr->listId() == id);
      CHECK(rowPtr->parentId() == parentId);
      CHECK(rowPtr->depth() == depth);
      CHECK(rowPtr->isSmart() == isSmart);
      CHECK(rowPtr->name() == name);
      CHECK(rowPtr->filter() == filter);
    }

    SECTION("setters work correctly")
    {
      auto const newId = ListId{100};
      auto const newParentId = ListId{200};
      int const newDepth = 5;
      bool const newSmart = false;
      auto const* const newName = "Updated Name";

      rowPtr->setListId(newId);
      rowPtr->setParentId(newParentId);
      rowPtr->setDepth(newDepth);
      rowPtr->setSmart(newSmart);
      rowPtr->setName(newName);

      CHECK(rowPtr->listId() == newId);
      CHECK(rowPtr->parentId() == newParentId);
      CHECK(rowPtr->depth() == newDepth);
      CHECK(rowPtr->isSmart() == newSmart);
      CHECK(rowPtr->name() == newName);
    }
  }

  TEST_CASE("ListTreeItem exposes parent-child hierarchy state", "[gtk][unit][list][model]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto const row1Ptr = ListRowObject::create(ListId{1}, kInvalidListId, 0, false, "Parent");
    auto const item1Ptr = ListTreeItem::create(row1Ptr);

    SECTION("initial state")
    {
      CHECK(item1Ptr->row() == row1Ptr);
      CHECK(item1Ptr->listId() == ListId{1});
      CHECK(item1Ptr->parent() == nullptr);
      CHECK(item1Ptr->nChildren() == 0);
      CHECK(item1Ptr->hasChildren() == false);
      CHECK(item1Ptr->child(0) == nullptr);
    }

    SECTION("adding children")
    {
      auto const row2Ptr = ListRowObject::create(ListId{2}, ListId{1}, 1, false, "Child 1");
      auto const item2Ptr = ListTreeItem::create(row2Ptr);
      item2Ptr->setParent(item1Ptr.get());

      auto const childrenPtr = item1Ptr->children();
      childrenPtr->append(item2Ptr);

      CHECK(item1Ptr->nChildren() == 1);
      CHECK(item1Ptr->hasChildren() == true);
      CHECK(item1Ptr->child(0) == item2Ptr);
      CHECK(item2Ptr->parent() == item1Ptr.get());
      CHECK(item2Ptr->row() == row2Ptr);
    }
  }
} // namespace ao::gtk::test
