// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListTreeModelBuilder.h"

#include "../../TestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/liststore.h>
#include <glibmm/refptr.h>

namespace ao::gtk::test
{
  TEST_CASE("ListTreeModelBuilder - builds nested list tree rows", "[gtk][unit][list][builder]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    // 1. Add some lists to the library
    auto& writer = fixture.runtime().library().writer();
    auto const idA = ao::test::requireValue(writer.createList(rt::LibraryWriter::ListDraft{
      .kind = rt::LibraryWriter::ListKind::Manual,
      .name = "Manual List A",
    }));
    drainGtkEvents();
    auto const idB = ao::test::requireValue(writer.createList(rt::LibraryWriter::ListDraft{
      .kind = rt::LibraryWriter::ListKind::Smart,
      .parentId = idA,
      .name = "Smart Child B",
      .expression = "$genre = Rock",
    }));
    drainGtkEvents();

    // 2. Build the model
    auto const result = ListTreeModelBuilder::build(fixture.runtime().library());

    SECTION("Basic structure")
    {
      // Root store should contain exactly 1 item: "All Tracks"
      REQUIRE(result.storePtr->get_n_items() == 1);
      auto const allTracksPtr = result.storePtr->get_item(0);
      CHECK(allTracksPtr->row()->name() == "All Tracks");
      CHECK(allTracksPtr->listId() == rt::kAllTracksListId);

      // "All Tracks" should have "Manual List A" as child
      auto const allTracksChildrenPtr = allTracksPtr->children();
      REQUIRE(allTracksChildrenPtr->get_n_items() == 1);
      auto const itemAPtr = allTracksChildrenPtr->get_item(0);
      CHECK_FALSE(itemAPtr->row()->name().empty());
      CHECK_FALSE(itemAPtr->row()->isSmart());
      CHECK(itemAPtr->listId() == idA);

      // "Manual List A" should have "Smart Child B" as child
      auto const itemAChildrenPtr = itemAPtr->children();
      REQUIRE(itemAChildrenPtr->get_n_items() == 1);
      auto const itemBPtr = itemAChildrenPtr->get_item(0);
      CHECK_FALSE(itemBPtr->row()->name().empty());
      CHECK(itemBPtr->row()->isSmart());
      CHECK(itemBPtr->listId() == idB);
    }

    SECTION("Models are created")
    {
      REQUIRE(result.treeModelPtr);
      REQUIRE(result.selectionModelPtr);
      CHECK(result.selectionModelPtr->get_model() == result.treeModelPtr);
      CHECK(result.treeModelPtr->get_model() == result.storePtr);
    }

    SECTION("NodesById mapping")
    {
      CHECK(result.nodesById.contains(rt::kAllTracksListId));
      CHECK(result.nodesById.contains(idA));
      CHECK(result.nodesById.contains(idB));
      CHECK(result.nodesById.at(idA)->listId() == idA);
    }
  }
} // namespace ao::gtk::test
