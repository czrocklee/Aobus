// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListTreeModelBuilder.h"

#include "../../TestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/Type.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/CorePrimitives.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/liststore.h>
#include <glibmm/refptr.h>

namespace ao::gtk::test
{
  TEST_CASE("ListTreeModelBuilder builds nested list tree rows", "[gtk][unit][list][builder]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& library = fixture.runtime().musicLibrary();

    // 1. Add some lists to the library
    auto idA = ListId{kInvalidListId};
    auto idB = ListId{kInvalidListId};

    {
      auto txn = library.writeTransaction();
      auto writer = library.lists().writer(txn);

      // List A (Manual)
      auto builderA = library::ListBuilder::createNew();
      builderA.name("Manual List A");
      auto [id, _] = ao::test::requireValue(writer.create(builderA.serialize()));
      idA = id;

      // List B (Smart, child of A)
      auto builderB = library::ListBuilder::createNew();
      builderB.name("Smart Child B").parentId(idA).filter("genre:rock");
      builderB.tracks().isSmart(true);
      auto [id2, _] = ao::test::requireValue(writer.create(builderB.serialize()));
      idB = id2;

      REQUIRE(txn.commit());
    }

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
      REQUIRE(allTracksPtr->nChildren() == 1);
      auto const itemAPtr = allTracksPtr->child(0);
      CHECK_FALSE(itemAPtr->row()->name().empty());
      CHECK(itemAPtr->listId() == idA);
      CHECK(itemAPtr->parent() == allTracksPtr.get());

      // "Manual List A" should have "Smart Child B" as child
      REQUIRE(itemAPtr->nChildren() == 1);
      auto const itemBPtr = itemAPtr->child(0);
      CHECK_FALSE(itemBPtr->row()->name().empty());
      CHECK(itemBPtr->listId() == idB);
      CHECK(itemBPtr->parent() == itemAPtr.get());
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
