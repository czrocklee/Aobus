// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListNavigationController.h"

#include "../../TestUtils.h"
#include "app/ThemeCoordinator.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackRowCache.h"
#include <ao/CoreIds.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/library/list/ListEditWorkflow.h>

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
    ListId createList(rt::Library& library, std::string const& name, ListId parentId = kInvalidListId)
    {
      return ao::test::requireValue(uimodel::ListEditWorkflow{library}.create(rt::LibraryListDraft{
        .kind = rt::LibraryListKind::Manual,
        .parentId = parentId,
        .name = name,
      }));
    }

    Glib::RefPtr<Gio::SimpleAction> simpleAction(Gio::ActionMap& actionMap, std::string const& name)
    {
      return std::dynamic_pointer_cast<Gio::SimpleAction>(actionMap.lookup_action(name));
    }

    std::optional<library::ListView> findList(library::MusicLibrary const& library, ListId listId)
    {
      auto transaction = library.readTransaction();
      auto reader = library.lists().reader(transaction);
      return reader.get(listId);
    }
  } // namespace

  TEST_CASE("ListNavigationController - binds navigation actions to library state", "[gtk][unit][list]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto cache = TrackRowCache{fixture.runtime().library()};

    auto selectedId = ListId{999};
    auto savedPresentationListId = kInvalidListId;
    auto savedPresentationId = std::string{};
    auto callbacks = ListNavigationController::Callbacks{.onListSelected = [&](ListId id) { selectedId = id; },
                                                         .onListPresentationSaved =
                                                           [&](ListId id, std::string presentationId)
                                                         {
                                                           savedPresentationListId = id;
                                                           savedPresentationId = std::move(presentationId);
                                                         }};

    auto themeCoordinator = ThemeCoordinator{};
    auto controller = ListNavigationController{window, fixture.runtime(), std::move(callbacks), themeCoordinator};
    window.set_child(controller.widget());

    SECTION("rebuildTree populates the navigation panel")
    {
      auto const testListId = createList(fixture.runtime().library(), "Test List");

      controller.rebuildTree(cache);
      drainGtkEvents();

      controller.select(testListId);
      drainGtkEvents();
      CHECK(selectedId == testListId);
    }

    SECTION("select triggers callback")
    {
      auto const testListId = createList(fixture.runtime().library(), "Select Target");

      controller.rebuildTree(cache);
      drainGtkEvents();

      controller.select(testListId);
      drainGtkEvents();

      CHECK(selectedId == testListId);
    }

    SECTION("registered actions update from the currently selected list")
    {
      auto groupPtr = Gio::SimpleActionGroup::create();
      controller.addActionsTo(*groupPtr);

      auto const newActionPtr = simpleAction(*groupPtr, "list-new-smart-list");
      auto const editActionPtr = simpleAction(*groupPtr, "list-edit");
      auto const deleteActionPtr = simpleAction(*groupPtr, "list-delete");
      REQUIRE(newActionPtr);
      REQUIRE(editActionPtr);
      REQUIRE(deleteActionPtr);

      CHECK_FALSE(newActionPtr->get_enabled());
      CHECK_FALSE(editActionPtr->get_enabled());
      CHECK_FALSE(deleteActionPtr->get_enabled());

      auto const leafListId = createList(fixture.runtime().library(), "Leaf List");

      controller.rebuildTree(cache);
      drainGtkEvents();

      controller.select(leafListId);
      drainGtkEvents();
      CHECK(newActionPtr->get_enabled());
      CHECK(editActionPtr->get_enabled());
      CHECK(deleteActionPtr->get_enabled());
    }

    SECTION("presentation changes do not re-drive list selection")
    {
      auto const activeListId = createList(fixture.runtime().library(), "Active List");
      auto const browsedListId = createList(fixture.runtime().library(), "Browsed List");
      controller.rebuildTree(cache);
      REQUIRE(fixture.runtime().workspace().navigateTo(activeListId));
      drainGtkEvents();

      controller.select(browsedListId);
      drainGtkEvents();
      REQUIRE(selectedId == browsedListId);
      selectedId = kInvalidListId;
      auto const* const albums = rt::builtinTrackPresentationPreset("albums");
      REQUIRE(albums != nullptr);

      REQUIRE(fixture.runtime().workspace().setActivePresentation(albums->spec));
      drainGtkEvents();

      CHECK(selectedId == kInvalidListId);
    }

    SECTION("submitListDraft creates a list and selects it on rebuild")
    {
      auto draft = rt::LibraryListDraft{};
      draft.kind = rt::LibraryListKind::Smart;
      draft.name = "Recently Played";
      draft.description = "Tracks touched this week";
      draft.expression = "$title ~ \"Recent\"";

      auto const listId = controller.submitListDraft(draft, "compact");

      auto const optList = findList(fixture.runtime().musicLibrary(), listId);
      REQUIRE(optList);
      CHECK_FALSE(optList->name().empty());
      CHECK(savedPresentationListId == listId);
      CHECK(savedPresentationId == "compact");

      controller.rebuildTree(cache);
      drainGtkEvents();

      CHECK(selectedId == listId);
    }

    SECTION("submitListDraft updates an existing list and preserves the presentation callback")
    {
      auto const listId = createList(fixture.runtime().library(), "Old Name");

      auto draft = rt::LibraryListDraft{};
      draft.kind = rt::LibraryListKind::Smart;
      draft.listId = listId;
      draft.name = "High Energy";
      draft.description = "Updated description";
      draft.expression = "$title ~ \"Energy\"";

      auto const savedId = controller.submitListDraft(draft, "wide");

      auto const optList = findList(fixture.runtime().musicLibrary(), listId);
      REQUIRE(optList);
      CHECK(savedId == listId);
      CHECK_FALSE(optList->name().empty());
      CHECK(savedPresentationListId == listId);
      CHECK(savedPresentationId == "wide");

      controller.rebuildTree(cache);
      drainGtkEvents();

      CHECK(selectedId == listId);
    }

    SECTION("submitListDraft rejects invalid drafts without saving presentation")
    {
      auto draft = rt::LibraryListDraft{};
      draft.kind = rt::LibraryListKind::Smart;
      draft.name = "Invalid";
      draft.expression = "(";

      auto const listId = controller.submitListDraft(draft, "wide");

      CHECK(listId == kInvalidListId);
      CHECK(savedPresentationListId == kInvalidListId);
      CHECK(savedPresentationId.empty());
    }

    SECTION("delete action removes the selected leaf list")
    {
      auto groupPtr = Gio::SimpleActionGroup::create();
      controller.addActionsTo(*groupPtr);

      auto const deleteActionPtr = simpleAction(*groupPtr, "list-delete");
      REQUIRE(deleteActionPtr);

      auto const& library = fixture.runtime().musicLibrary();
      auto const listId = createList(fixture.runtime().library(), "Delete Target");

      controller.rebuildTree(cache);
      drainGtkEvents();

      controller.select(listId);
      drainGtkEvents();
      REQUIRE(deleteActionPtr->get_enabled());

      deleteActionPtr->activate();

      CHECK(!findList(library, listId));

      controller.rebuildTree(cache);
      drainGtkEvents();

      CHECK(selectedId == rt::kAllTracksListId);
    }
  }
} // namespace ao::gtk::test
