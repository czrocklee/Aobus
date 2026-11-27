// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListTreeModelBuilder.h"

#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include "test/linux-gtk/GtkRuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryCommands.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/liststore.h>
#include <glib.h>
#include <glibmm/refptr.h>
#include <gtkmm/treelistrow.h>

#include <memory>
#include <utility>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("ListTreeModelBuilder - builds nested list tree rows")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    // 1. Add some lists to the library
    auto& commands = fixture.owner().libraryCommands();
    auto const idA = ao::test::requireValue(
      runGtkTask(fixture.runtime(), commands.createListAsync(rt::ListDraft{.name = "Parent List A"})));
    drainGtkEvents();
    auto const idB = ao::test::requireValue(runGtkTask(fixture.runtime(),
                                                       commands.createListAsync(rt::ListDraft{
                                                         .parentId = idA,
                                                         .name = "Filtered Child B",
                                                         .expression = "$genre = Rock",
                                                       })));
    drainGtkEvents();
    auto const idC = ao::test::requireValue(
      runGtkTask(fixture.runtime(), commands.createListAsync(rt::ListDraft{.name = "Root List C"})));
    drainGtkEvents();

    // 2. Build the model
    auto const result = ListTreeModelBuilder::build(fixture.runtime().library(), ao::test::englishMessageCatalog());

    SECTION("Basic structure")
    {
      REQUIRE(result.storePtr->get_n_items() == 3);
      auto const allTracksPtr = result.storePtr->get_item(0);
      CHECK(allTracksPtr->row()->name() == "All Tracks");
      CHECK(allTracksPtr->row()->isSystem());
      CHECK(allTracksPtr->listId() == rt::kAllTracksListId);
      CHECK_FALSE(allTracksPtr->hasChildren());

      auto const itemAPtr = result.storePtr->get_item(1);
      CHECK_FALSE(itemAPtr->row()->name().empty());
      CHECK_FALSE(itemAPtr->row()->isSystem());
      CHECK(itemAPtr->listId() == idA);

      // Every saved List can parent another List, independent of expression or saved order.
      auto const itemAChildrenPtr = itemAPtr->children();
      REQUIRE(itemAChildrenPtr->get_n_items() == 1);
      auto const itemBPtr = itemAChildrenPtr->get_item(0);
      CHECK_FALSE(itemBPtr->row()->name().empty());
      CHECK_FALSE(itemBPtr->row()->isSystem());
      CHECK(itemBPtr->row()->filter() == "$genre = Rock");
      CHECK(itemBPtr->listId() == idB);

      auto const itemCPtr = result.storePtr->get_item(2);
      CHECK(itemCPtr->listId() == idC);
    }

    SECTION("Returned projection retains list relationships")
    {
      auto const& projection = result.projection;
      CHECK(projection.rootIds == std::vector{rt::kAllTracksListId, idA, idC});
      REQUIRE(projection.rowsById.contains(rt::kAllTracksListId));
      CHECK(projection.rowsById.at(rt::kAllTracksListId).childIds.empty());
      REQUIRE(projection.rowsById.contains(idA));
      REQUIRE(projection.rowsById.contains(idB));
      REQUIRE(projection.rowsById.contains(idC));
      CHECK(projection.rowsById.at(idA).childIds == std::vector{idB});
      CHECK(projection.rowsById.at(idB).parentId == idA);
      CHECK(projection.rowsById.at(idB).childIds.empty());
      CHECK(projection.rowsById.at(idC).parentId == kInvalidListId);
      CHECK(projection.rowsById.at(idC).childIds.empty());
    }

    SECTION("Models are created")
    {
      REQUIRE(result.treeModelPtr);
      REQUIRE(result.sectionModelPtr);
      REQUIRE(result.selectionModelPtr);
      CHECK(result.selectionModelPtr->get_model() == result.sectionModelPtr);
      CHECK(result.treeModelPtr->get_model() == result.storePtr);
      CHECK(result.sectionModelPtr->get_section(0) == std::make_pair(::guint{0}, ::guint{1}));
      CHECK(result.sectionModelPtr->get_section(1) == std::make_pair(::guint{1}, ::guint{4}));
      CHECK(result.sectionModelPtr->get_section(3) == std::make_pair(::guint{1}, ::guint{4}));
      CHECK(result.sectionModelPtr->get_section(4) == std::make_pair(::guint{4}, ::guint{G_MAXUINT}));
    }

    SECTION("Expanded descendants remain in the saved List section")
    {
      auto const parentRowPtr = std::dynamic_pointer_cast<Gtk::TreeListRow>(result.treeModelPtr->get_object(1));
      REQUIRE(parentRowPtr);
      REQUIRE(parentRowPtr->get_expanded());

      parentRowPtr->set_expanded(false);

      CHECK(result.treeModelPtr->get_n_items() == 3);
      CHECK(result.sectionModelPtr->get_n_items() == 3);
      CHECK(result.selectionModelPtr->get_n_items() == 3);
      CHECK(result.sectionModelPtr->get_section(2) == std::make_pair(::guint{1}, ::guint{3}));

      parentRowPtr->set_expanded(true);

      CHECK(result.treeModelPtr->get_n_items() == 4);
      CHECK(result.sectionModelPtr->get_n_items() == 4);
      CHECK(result.selectionModelPtr->get_n_items() == 4);
      CHECK(result.sectionModelPtr->get_section(2) == std::make_pair(::guint{1}, ::guint{4}));
    }
  }
} // namespace ao::gtk::test
