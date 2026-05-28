// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListTreeModelBuilder.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/Type.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/CorePrimitives.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/liststore.h>
#include <glibmm/refptr.h>

namespace ao::gtk::test
{
  TEST_CASE("ListTreeModelBuilder - building tree", "[gtk][list][builder]")
  {
    [[maybe_unused]] auto const app = ensureGtkApplication();
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
      auto [id, _] = writer.create(builderA.serialize());
      idA = id;

      // List B (Smart, child of A)
      auto builderB = library::ListBuilder::createNew();
      builderB.name("Smart Child B").parentId(idA).filter("genre:rock");
      builderB.tracks().isSmart(true);
      auto [id2, _] = writer.create(builderB.serialize());
      idB = id2;

      txn.commit();
    }

    // 2. Build the model
    auto txn = library.readTransaction();
    auto const result = ListTreeModelBuilder::build(fixture.runtime(), txn);

    SECTION("Basic structure")
    {
      // Root store should contain exactly 1 item: "All Tracks"
      REQUIRE(result.store->get_n_items() == 1);
      auto const allTracks = result.store->get_item(0);
      CHECK(allTracks->row()->name() == "All Tracks");
      CHECK(allTracks->listId() == rt::kAllTracksListId);

      // "All Tracks" should have "Manual List A" as child
      REQUIRE(allTracks->nChildren() == 1);
      auto const itemA = allTracks->child(0);
      CHECK(itemA->row()->name() == "Manual List A");
      CHECK(itemA->listId() == idA);
      CHECK(itemA->parent() == allTracks.get());

      // "Manual List A" should have "Smart Child B" as child
      REQUIRE(itemA->nChildren() == 1);
      auto const itemB = itemA->child(0);
      CHECK(itemB->row()->name() == "Smart Child B");
      CHECK(itemB->listId() == idB);
      CHECK(itemB->parent() == itemA.get());
      CHECK(itemB->row()->isSmart() == true);
      CHECK(itemB->row()->filter() == "genre:rock");
    }

    SECTION("Models are created")
    {
      CHECK(result.treeModel);
      CHECK(result.selectionModel);
      CHECK(result.selectionModel->get_model() == result.treeModel);
      CHECK(result.treeModel->get_model() == result.store);
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
