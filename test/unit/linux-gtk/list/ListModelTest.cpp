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
  TEST_CASE("ListRowObject - basic properties", "[gtk][list][model]")
  {
    [[maybe_unused]] auto const app = ensureGtkApplication();

    auto const id = ListId{42};
    auto const parentId = ListId{10};
    auto const depth = 2;
    auto const isSmart = true;
    auto const* const name = "Test List";
    auto const* const filter = "artist:\"Test Artist\"";

    auto const row = ListRowObject::create(id, parentId, depth, isSmart, name, filter);

    SECTION("initial values are correct")
    {
      CHECK(row->listId() == id);
      CHECK(row->parentId() == parentId);
      CHECK(row->depth() == depth);
      CHECK(row->isSmart() == isSmart);
      CHECK(row->name() == name);
      CHECK(row->filter() == filter);
    }

    SECTION("setters work correctly")
    {
      auto const newId = ListId{100};
      auto const newParentId = ListId{200};
      auto const newDepth = 5;
      auto const newSmart = false;
      auto const* const newName = "Updated Name";

      row->setListId(newId);
      row->setParentId(newParentId);
      row->setDepth(newDepth);
      row->setSmart(newSmart);
      row->setName(newName);

      CHECK(row->listId() == newId);
      CHECK(row->parentId() == newParentId);
      CHECK(row->depth() == newDepth);
      CHECK(row->isSmart() == newSmart);
      CHECK(row->name() == newName);
    }
  }

  TEST_CASE("ListTreeItem - hierarchy", "[gtk][list][model]")
  {
    [[maybe_unused]] auto const app = ensureGtkApplication();

    auto const row1 = ListRowObject::create(ListId{1}, kInvalidListId, 0, false, "Parent");
    auto const item1 = ListTreeItem::create(row1);

    SECTION("initial state")
    {
      CHECK(item1->row() == row1);
      CHECK(item1->listId() == ListId{1});
      CHECK(item1->parent() == nullptr);
      CHECK(item1->nChildren() == 0);
      CHECK(item1->hasChildren() == false);
      CHECK(item1->child(0) == nullptr);
    }

    SECTION("adding children")
    {
      auto const row2 = ListRowObject::create(ListId{2}, ListId{1}, 1, false, "Child 1");
      auto const item2 = ListTreeItem::create(row2);
      item2->setParent(item1.get());

      auto const children = item1->children();
      children->append(item2);

      CHECK(item1->nChildren() == 1);
      CHECK(item1->hasChildren() == true);
      CHECK(item1->child(0) == item2);
      CHECK(item2->parent() == item1.get());
      CHECK(item2->row() == row2);
    }
  }
} // namespace ao::gtk::test
