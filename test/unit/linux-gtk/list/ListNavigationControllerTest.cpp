// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListNavigationController.h"

#include "../../TestUtils.h"
#include "app/ThemeCoordinator.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackRowCache.h"
#include <ao/Type.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/TrackSource.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/simpleaction.h>
#include <giomm/simpleactiongroup.h>
#include <gtkmm/window.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace ao::gtk::test
{
  namespace
  {
    ListId createList(library::MusicLibrary& library, std::string const& name, ListId parentId = kInvalidListId)
    {
      auto txn = library.writeTransaction();
      auto writer = library.lists().writer(txn);
      auto builder = library::ListBuilder::createNew();
      builder.name(name).parentId(parentId);
      auto const listId = ao::test::requireValue(writer.create(builder.serialize())).first;
      txn.commit();
      return listId;
    }

    Glib::RefPtr<Gio::SimpleAction> simpleAction(Gio::ActionMap& actionMap, std::string const& name)
    {
      return std::dynamic_pointer_cast<Gio::SimpleAction>(actionMap.lookup_action(name));
    }

    std::optional<library::ListView> findList(library::MusicLibrary& library, ListId listId)
    {
      auto txn = library.readTransaction();
      auto reader = library.lists().reader(txn);
      return reader.get(listId);
    }
  } // namespace

  TEST_CASE("ListNavigationController - basic interactions", "[gtk][list][controller]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto cache = TrackRowCache{fixture.runtime().library()};

    auto selectedId = ListId{999};
    auto savedPresentationListId = kInvalidListId;
    auto savedPresentationId = std::string{};
    auto callbacks =
      ListNavigationController::Callbacks{.onListSelected = [&](ListId id) { selectedId = id; },
                                          .getListMembership = [&](ListId) -> rt::TrackSource* { return nullptr; },
                                          .onListPresentationSaved =
                                            [&](ListId id, std::string presentationId)
                                          {
                                            savedPresentationListId = id;
                                            savedPresentationId = std::move(presentationId);
                                          },
                                          .getListPresentation = {}};

    auto themeController = ThemeCoordinator{};
    auto controller = ListNavigationController{window, fixture.runtime(), std::move(callbacks), themeController};
    window.set_child(controller.widget());

    SECTION("rebuildTree populates the navigation panel")
    {
      createList(fixture.runtime().musicLibrary(), "Test List");

      controller.rebuildTree(cache);
      drainGtkEvents();

      // Navigation panel should contain "All Tracks" and "Test List"
    }

    SECTION("select triggers callback")
    {
      auto const testListId = createList(fixture.runtime().musicLibrary(), "Select Target");

      controller.rebuildTree(cache);
      drainGtkEvents();

      controller.select(testListId);
      drainGtkEvents();

      CHECK(selectedId == testListId);
    }

    SECTION("registered actions follow the selected list policy")
    {
      auto groupPtr = Gio::SimpleActionGroup::create();
      controller.addActionsTo(*groupPtr);

      auto const newActionPtr = simpleAction(*groupPtr, "new");
      auto const editActionPtr = simpleAction(*groupPtr, "edit");
      auto const deleteActionPtr = simpleAction(*groupPtr, "delete");
      REQUIRE(newActionPtr);
      REQUIRE(editActionPtr);
      REQUIRE(deleteActionPtr);

      CHECK_FALSE(newActionPtr->get_enabled());
      CHECK_FALSE(editActionPtr->get_enabled());
      CHECK_FALSE(deleteActionPtr->get_enabled());

      auto& library = fixture.runtime().musicLibrary();
      auto const leafListId = createList(library, "Leaf List");
      auto const parentListId = createList(library, "Parent List");
      createList(library, "Child List", parentListId);

      controller.rebuildTree(cache);
      drainGtkEvents();

      controller.select(leafListId);
      drainGtkEvents();
      CHECK(newActionPtr->get_enabled());
      CHECK(editActionPtr->get_enabled());
      CHECK(deleteActionPtr->get_enabled());

      controller.select(parentListId);
      drainGtkEvents();
      CHECK(newActionPtr->get_enabled());
      CHECK(editActionPtr->get_enabled());
      CHECK_FALSE(deleteActionPtr->get_enabled());

      controller.select(rt::kAllTracksListId);
      drainGtkEvents();
      CHECK(newActionPtr->get_enabled());
      CHECK_FALSE(editActionPtr->get_enabled());
      CHECK_FALSE(deleteActionPtr->get_enabled());
    }

    SECTION("submitListDraft creates a list and selects it on rebuild")
    {
      auto draft = rt::LibraryWriter::ListDraft{};
      draft.name = "Recently Played";
      draft.description = "Tracks touched this week";
      draft.expression = "$lastPlayed >= 7d";

      auto const listId = controller.submitListDraft(draft, "compact");

      auto const optList = findList(fixture.runtime().musicLibrary(), listId);
      REQUIRE(optList);
      CHECK(optList->name() == "Recently Played");
      CHECK(optList->filter() == "$lastPlayed >= 7d");
      CHECK(savedPresentationListId == listId);
      CHECK(savedPresentationId == "compact");

      controller.rebuildTree(cache);
      drainGtkEvents();

      CHECK(selectedId == listId);
    }

    SECTION("submitListDraft updates an existing list and preserves the presentation callback")
    {
      auto const listId = createList(fixture.runtime().musicLibrary(), "Old Name");

      auto draft = rt::LibraryWriter::ListDraft{};
      draft.listId = listId;
      draft.name = "High Energy";
      draft.description = "Updated description";
      draft.expression = "$bpm >= 130";

      auto const savedId = controller.submitListDraft(draft, "wide");

      auto const optList = findList(fixture.runtime().musicLibrary(), listId);
      REQUIRE(optList);
      CHECK(savedId == listId);
      CHECK(optList->name() == "High Energy");
      CHECK(optList->filter() == "$bpm >= 130");
      CHECK(savedPresentationListId == listId);
      CHECK(savedPresentationId == "wide");

      controller.rebuildTree(cache);
      drainGtkEvents();

      CHECK(selectedId == listId);
    }

    SECTION("delete action removes the selected leaf list")
    {
      auto groupPtr = Gio::SimpleActionGroup::create();
      controller.addActionsTo(*groupPtr);

      auto const deleteActionPtr = simpleAction(*groupPtr, "delete");
      REQUIRE(deleteActionPtr);

      auto& library = fixture.runtime().musicLibrary();
      auto const listId = createList(library, "Delete Target");

      controller.rebuildTree(cache);
      drainGtkEvents();

      controller.select(listId);
      drainGtkEvents();
      REQUIRE(deleteActionPtr->get_enabled());

      deleteActionPtr->activate();

      CHECK_FALSE(findList(library, listId).has_value());

      controller.rebuildTree(cache);
      drainGtkEvents();

      CHECK(selectedId == rt::kAllTracksListId);
    }
  }
} // namespace ao::gtk::test
