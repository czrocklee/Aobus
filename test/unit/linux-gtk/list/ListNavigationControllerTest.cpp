// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListNavigationController.h"

#include "../../TestUtils.h"
#include "app/AppDialog.h"
#include "app/ThemeCoordinator.h"
#include "list/ListNavigationPanel.h"
#include "list/SmartListDialog.h"
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
#include <ao/rt/library/Library.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/simpleaction.h>
#include <giomm/simpleactiongroup.h>
#include <gtkmm/dialog.h>
#include <gtkmm/label.h>
#include <gtkmm/listview.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/window.h>

#include <cstdint>
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
      return ao::test::requireValue(library.createList(rt::LibraryListDraft{
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

    AppDialog* findAppDialog(std::string const& title)
    {
      for (auto* const window : Gtk::Window::list_toplevels())
      {
        if (auto* const dialog = dynamic_cast<AppDialog*>(window); dialog != nullptr && dialog->get_title() == title)
        {
          return dialog;
        }
      }

      return nullptr;
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
      REQUIRE(fixture.runtime().workspace().navigate({.target = activeListId}));
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

      auto const listResult = controller.submitListDraft(draft, "compact");
      REQUIRE(listResult);
      auto const listId = *listResult;

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

      auto const savedResult = controller.submitListDraft(draft, "wide");
      REQUIRE(savedResult);

      auto const optList = findList(fixture.runtime().musicLibrary(), listId);
      REQUIRE(optList);
      CHECK(*savedResult == listId);
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

      auto const listResult = controller.submitListDraft(draft, "wide");

      REQUIRE_FALSE(listResult);
      CHECK(savedPresentationListId == kInvalidListId);
      CHECK(savedPresentationId.empty());
    }

    SECTION("stale edit response keeps the dialog and draft visible")
    {
      auto groupPtr = Gio::SimpleActionGroup::create();
      controller.addActionsTo(*groupPtr);
      auto const editActionPtr = simpleAction(*groupPtr, "list-edit");
      REQUIRE(editActionPtr);
      auto const listId = createList(fixture.runtime().library(), "Draft to Preserve");
      controller.rebuildTree(cache);
      drainGtkEvents();
      controller.select(listId);
      drainGtkEvents();

      editActionPtr->activate();
      drainGtkEvents();
      auto* const dialog = dynamic_cast<SmartListDialog*>(findAppDialog("Edit List"));
      REQUIRE(dialog != nullptr);
      REQUIRE(dialog->get_visible());
      CHECK(dialog->draft().name == "Draft to Preserve");
      REQUIRE(fixture.runtime().library().deleteList(listId));

      dialog->response(Gtk::ResponseType::OK);
      drainGtkEvents();

      CHECK(dialog->get_visible());
      CHECK(dialog->draft().name == "Draft to Preserve");
      bool visibleError = false;

      for (auto* const label : collectAll<Gtk::Label>(*dialog))
      {
        visibleError = visibleError ||
                       (label->get_visible() && label->has_css_class("ao-layout-error") && !label->get_text().empty());
      }

      CHECK(visibleError);
      dialog->close();
      drainGtkEvents();
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

    SECTION("failed delete shows a parent-bound dialog and keeps the selected tree row")
    {
      auto groupPtr = Gio::SimpleActionGroup::create();
      controller.addActionsTo(*groupPtr);
      auto const deleteActionPtr = simpleAction(*groupPtr, "list-delete");
      REQUIRE(deleteActionPtr);
      auto const listId = createList(fixture.runtime().library(), "Stale Delete Target");
      controller.rebuildTree(cache);
      drainGtkEvents();
      controller.select(listId);
      drainGtkEvents();
      REQUIRE(selectedId == listId);
      REQUIRE(fixture.runtime().library().deleteList(listId));

      deleteActionPtr->activate();
      drainGtkEvents();

      auto* const dialog = findAppDialog("Unable to Delete List");
      REQUIRE(dialog != nullptr);
      CHECK(dialog->get_transient_for() == &window);
      CHECK(selectedId == listId);
      dialog->response(Gtk::ResponseType::CLOSE);
      drainGtkEvents();
    }
  }

  TEST_CASE("ListNavigationPanel - retired selection model no longer drives callbacks", "[gtk][regression][list]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    [[maybe_unused]] auto const listId = createList(fixture.runtime().library(), "Retired Selection Source");
    std::int32_t selectionChangedCount = 0;
    auto panel = ListNavigationPanel{
      {.onSelectionChanged = [&](ListId) { ++selectionChangedCount; }, .onContextMenuRequested = {}}};

    panel.rebuildTree(fixture.runtime().library());
    auto* const scrolledWindow = dynamic_cast<Gtk::ScrolledWindow*>(&panel.widget());
    REQUIRE(scrolledWindow != nullptr);
    auto* const listView = dynamic_cast<Gtk::ListView*>(scrolledWindow->get_child());
    REQUIRE(listView != nullptr);
    auto const retiredSelectionPtr = std::dynamic_pointer_cast<Gtk::SingleSelection>(listView->get_model());
    REQUIRE(retiredSelectionPtr);
    REQUIRE(retiredSelectionPtr->get_n_items() > 1);

    panel.rebuildTree(fixture.runtime().library());
    selectionChangedCount = 0;
    auto const replacementPosition = retiredSelectionPtr->get_selected() == 0 ? 1U : 0U;
    retiredSelectionPtr->set_selected(replacementPosition);

    CHECK(selectionChangedCount == 0);
  }
} // namespace ao::gtk::test
