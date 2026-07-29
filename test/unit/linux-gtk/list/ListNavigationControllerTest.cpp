// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListNavigationController.h"

#include "../../TestFixtureSupport.h"
#include "app/AppDialog.h"
#include "app/ThemeCoordinator.h"
#include "list/ListNavigationPanel.h"
#include "list/ListTreeItem.h"
#include "list/SmartListDialog.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "track/TrackRowCache.h"
#include <ao/CoreIds.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/simpleaction.h>
#include <giomm/simpleactiongroup.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dialog.h>
#include <gtkmm/label.h>
#include <gtkmm/listview.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/treelistrow.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk::test
{
  namespace
  {
    ListId createList(rt::Library& library,
                      std::string const& name,
                      ListId parentId = kInvalidListId,
                      std::string expression = {})
    {
      return ao::test::requireValue(library.createList(rt::LibraryListDraft{
        .parentId = parentId,
        .name = name,
        .expression = std::move(expression),
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

    bool trackHasTag(library::MusicLibrary const& library, TrackId const trackId, std::string_view const tag)
    {
      auto transaction = library.readTransaction();
      auto const optView =
        library.tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
      REQUIRE(optView);
      auto builder = library::TrackBuilder::fromView(*optView, library.dictionary());
      return std::ranges::contains(builder.tags().names(), tag);
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

    ListId selectedNavigationListId(ListNavigationController& controller)
    {
      auto* const scrolledWindow = dynamic_cast<Gtk::ScrolledWindow*>(&controller.widget());
      auto* const listView =
        scrolledWindow != nullptr ? dynamic_cast<Gtk::ListView*>(scrolledWindow->get_child()) : nullptr;
      auto const selectionPtr =
        listView != nullptr ? std::dynamic_pointer_cast<Gtk::SingleSelection>(listView->get_model()) : nullptr;
      auto const treeRowPtr = selectionPtr != nullptr
                                ? std::dynamic_pointer_cast<Gtk::TreeListRow>(selectionPtr->get_selected_item())
                                : nullptr;
      auto const itemPtr =
        treeRowPtr != nullptr ? std::dynamic_pointer_cast<ListTreeItem>(treeRowPtr->get_item()) : nullptr;
      return itemPtr != nullptr ? itemPtr->listId() : kInvalidListId;
    }
  } // namespace

  TEST_CASE("ListNavigationController - binds navigation actions to library state", "[gtk][unit][list]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto cache = TrackRowCache{fixture.runtime().library()};

    auto selectedId = ListId{999};
    bool rejectSelection = false;
    std::size_t selectionAttemptCount = 0;
    auto savedPresentationListId = kInvalidListId;
    auto savedPresentationId = std::string{};
    auto callbacks = ListNavigationController::Callbacks{.onListSelected =
                                                           [&](ListId id)
                                                         {
                                                           ++selectionAttemptCount;

                                                           if (rejectSelection)
                                                           {
                                                             return false;
                                                           }

                                                           selectedId = id;
                                                           return true;
                                                         },
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

    SECTION("rebuildTree selects the active workspace list restored before the navigation model exists")
    {
      auto const restoredListId = createList(fixture.runtime().library(), "Restored Selection");
      REQUIRE(fixture.runtime().workspace().navigate({.target = restoredListId}));
      drainGtkEvents();

      controller.rebuildTree(cache);
      drainGtkEvents();

      CHECK(selectedNavigationListId(controller) == restoredListId);
    }

    SECTION("rebuildTree refreshes actions when the restored workspace list is already selected")
    {
      auto groupPtr = Gio::SimpleActionGroup::create();
      controller.addActionsTo(*groupPtr);
      auto const newActionPtr = simpleAction(*groupPtr, "list-new-smart-list");
      REQUIRE(newActionPtr);
      REQUIRE(fixture.runtime().workspace().navigate({.target = rt::kAllTracksListId}));
      drainGtkEvents();

      controller.rebuildTree(cache);
      drainGtkEvents();

      CHECK(selectedNavigationListId(controller) == rt::kAllTracksListId);
      CHECK(newActionPtr->get_enabled());
    }

    SECTION("registered actions update from the currently selected list")
    {
      auto groupPtr = Gio::SimpleActionGroup::create();
      controller.addActionsTo(*groupPtr);

      auto const newActionPtr = simpleAction(*groupPtr, "list-new-smart-list");
      auto const newPlaylistActionPtr = simpleAction(*groupPtr, "list-new-playlist");
      auto const editActionPtr = simpleAction(*groupPtr, "list-edit");
      auto const deleteActionPtr = simpleAction(*groupPtr, "list-delete");
      auto const deleteSubtreeActionPtr = simpleAction(*groupPtr, "list-delete-subtree");
      REQUIRE(newActionPtr);
      REQUIRE(newPlaylistActionPtr);
      REQUIRE(editActionPtr);
      REQUIRE(deleteActionPtr);
      REQUIRE(deleteSubtreeActionPtr);

      CHECK_FALSE(newActionPtr->get_enabled());
      CHECK_FALSE(newPlaylistActionPtr->get_enabled());
      CHECK_FALSE(editActionPtr->get_enabled());
      CHECK_FALSE(deleteActionPtr->get_enabled());
      CHECK_FALSE(deleteSubtreeActionPtr->get_enabled());

      auto const leafListId = createList(fixture.runtime().library(), "Leaf List");

      controller.rebuildTree(cache);
      drainGtkEvents();

      controller.select(leafListId);
      drainGtkEvents();
      CHECK(newActionPtr->get_enabled());
      CHECK(newPlaylistActionPtr->get_enabled());
      CHECK(editActionPtr->get_enabled());
      CHECK(deleteActionPtr->get_enabled());
      CHECK_FALSE(deleteSubtreeActionPtr->get_enabled());
    }

    SECTION("New Playlist action opens the visible-tag template")
    {
      auto groupPtr = Gio::SimpleActionGroup::create();
      controller.addActionsTo(*groupPtr);
      auto const newPlaylistActionPtr = simpleAction(*groupPtr, "list-new-playlist");
      REQUIRE(newPlaylistActionPtr);

      controller.rebuildTree(cache);
      drainGtkEvents();
      controller.select(rt::kAllTracksListId);
      drainGtkEvents();
      REQUIRE(newPlaylistActionPtr->get_enabled());

      newPlaylistActionPtr->activate();
      drainGtkEvents();

      auto* const dialog = dynamic_cast<SmartListDialog*>(findAppDialog("New Playlist"));
      REQUIRE(dialog != nullptr);
      CHECK(dialog->presentationId() == "list-order");
      dialog->close();
      drainGtkEvents();
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

    SECTION("a failed pending selection is retried after the next complete rebuild")
    {
      auto draft = rt::LibraryListDraft{};
      draft.name = "Retry selection";
      draft.expression = "true";
      auto const listResult = controller.submitListDraft(draft, {});
      REQUIRE(listResult);
      auto const listId = *listResult;
      rejectSelection = true;

      controller.rebuildTree(cache);
      drainGtkEvents();

      CHECK(selectedId != listId);
      CHECK(selectionAttemptCount == 1);

      rejectSelection = false;
      controller.rebuildTree(cache);
      drainGtkEvents();

      CHECK(selectedId == listId);
      CHECK(selectionAttemptCount == 2);
    }

    SECTION("a newer successful selection supersedes an earlier failed one")
    {
      auto const rejectedListId = createList(fixture.runtime().library(), "Rejected Target");
      auto const acceptedListId = createList(fixture.runtime().library(), "Accepted Target");

      controller.rebuildTree(cache);
      drainGtkEvents();

      rejectSelection = true;
      controller.select(rejectedListId);
      drainGtkEvents();
      REQUIRE(selectedId != rejectedListId);

      rejectSelection = false;
      controller.select(acceptedListId);
      drainGtkEvents();
      REQUIRE(selectedId == acceptedListId);

      auto const attemptsBeforeRebuild = selectionAttemptCount;

      controller.rebuildTree(cache);
      drainGtkEvents();

      CHECK(selectedId == acceptedListId);
      CHECK(selectionAttemptCount == attemptsBeforeRebuild);
    }

    SECTION("an authoritative workspace selection supersedes an earlier failed one")
    {
      auto const rejectedListId = createList(fixture.runtime().library(), "Rejected Target");
      auto const navigatedListId = createList(fixture.runtime().library(), "Navigated Target");

      controller.rebuildTree(cache);
      drainGtkEvents();

      rejectSelection = true;
      controller.select(rejectedListId);
      drainGtkEvents();
      REQUIRE(selectedId != rejectedListId);

      // External navigation is authoritative: the panel syncs to it silently,
      // so the stale pending selection must not survive into the next rebuild.
      rejectSelection = false;
      REQUIRE(fixture.runtime().workspace().navigate({.target = navigatedListId}));
      drainGtkEvents();

      auto const attemptsBeforeRebuild = selectionAttemptCount;

      controller.rebuildTree(cache);
      drainGtkEvents();

      CHECK(selectedId != rejectedListId);
      CHECK(selectionAttemptCount == attemptsBeforeRebuild);
    }

    SECTION("closing the last workspace view discards an earlier failed selection")
    {
      auto const rejectedListId = createList(fixture.runtime().library(), "Rejected Target");
      auto const activeViewId =
        ao::test::requireValue(fixture.runtime().workspace().navigate({.target = rt::kAllTracksListId}));
      controller.rebuildTree(cache);
      drainGtkEvents();

      rejectSelection = true;
      controller.select(rejectedListId);
      drainGtkEvents();
      REQUIRE(selectedId != rejectedListId);

      rejectSelection = false;
      REQUIRE(fixture.runtime().workspace().closeView(activeViewId));
      drainGtkEvents();
      REQUIRE(fixture.runtime().workspace().snapshot().activeViewId == rt::kInvalidViewId);
      auto const attemptsBeforeRebuild = selectionAttemptCount;

      controller.rebuildTree(cache);
      drainGtkEvents();

      CHECK(selectedId != rejectedListId);
      CHECK(selectionAttemptCount == attemptsBeforeRebuild);
    }

    SECTION("submitListDraft updates an existing list and preserves the presentation callback")
    {
      auto const listId = createList(fixture.runtime().library(), "Old Name");

      auto draft = rt::LibraryListDraft{};
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
      drainGtkEvents();

      CHECK(findList(library, listId));
      auto* const confirmation = findAppDialog("Delete List?");
      REQUIRE(confirmation != nullptr);
      confirmation->response(Gtk::ResponseType::YES);
      drainGtkEvents();

      CHECK(!findList(library, listId));

      controller.rebuildTree(cache);
      drainGtkEvents();

      CHECK(selectedId == rt::kAllTracksListId);
    }

    SECTION("subtree delete previews and atomically removes the selected derived tree")
    {
      auto groupPtr = Gio::SimpleActionGroup::create();
      controller.addActionsTo(*groupPtr);
      auto const deleteActionPtr = simpleAction(*groupPtr, "list-delete");
      auto const deleteSubtreeActionPtr = simpleAction(*groupPtr, "list-delete-subtree");
      REQUIRE(deleteActionPtr);
      REQUIRE(deleteSubtreeActionPtr);

      auto const& library = fixture.runtime().musicLibrary();
      auto const parentId = createList(fixture.runtime().library(), "Delete Tree");
      auto const childId = createList(fixture.runtime().library(), "Delete Child", parentId);
      auto const grandchildId = createList(fixture.runtime().library(), "Delete Grandchild", childId);
      controller.rebuildTree(cache);
      drainGtkEvents();
      controller.select(parentId);
      drainGtkEvents();

      CHECK_FALSE(deleteActionPtr->get_enabled());
      REQUIRE(deleteSubtreeActionPtr->get_enabled());
      deleteSubtreeActionPtr->activate();
      drainGtkEvents();

      auto* const confirmation = findAppDialog("Delete List and Descendants?");
      REQUIRE(confirmation != nullptr);
      auto const labels = collectAll<Gtk::Label>(*confirmation);
      auto previewText = std::string{};

      for (auto* const label : labels)
      {
        previewText.append(label->get_text());
      }

      CHECK(previewText.contains("Delete Tree"));
      CHECK(previewText.contains("Delete Child"));
      CHECK(previewText.contains("Delete Grandchild"));
      confirmation->response(Gtk::ResponseType::YES);
      drainGtkEvents();

      CHECK_FALSE(findList(library, parentId));
      CHECK_FALSE(findList(library, childId));
      CHECK_FALSE(findList(library, grandchildId));
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

  TEST_CASE("ListNavigationController - writable-tag delete offers optional tag cleanup",
            "[gtk][unit][list-navigation][list-delete]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& library)
      { trackId = library::test::addTrack(library, {.title = "Tagged Track", .tags = {"road-trip"}}); }};
    auto window = Gtk::Window{};
    auto cache = TrackRowCache{fixture.runtime().library()};
    auto themeCoordinator = ThemeCoordinator{};
    auto controller = ListNavigationController{window, fixture.runtime(), {}, themeCoordinator};
    window.set_child(controller.widget());
    auto groupPtr = Gio::SimpleActionGroup::create();
    controller.addActionsTo(*groupPtr);
    auto const deleteActionPtr = simpleAction(*groupPtr, "list-delete");
    REQUIRE(deleteActionPtr);
    auto const listId = createList(fixture.runtime().library(), "Road Trip", kInvalidListId, R"(#"road-trip")");

    controller.rebuildTree(cache);
    drainGtkEvents();
    controller.select(listId);
    drainGtkEvents();
    REQUIRE(deleteActionPtr->get_enabled());
    deleteActionPtr->activate();
    drainGtkEvents();

    auto* const confirmation = findAppDialog("Delete List?");
    REQUIRE(confirmation != nullptr);
    auto const checkButtons = collectAll<Gtk::CheckButton>(*confirmation);
    REQUIRE(checkButtons.size() == 1);
    CHECK_FALSE(checkButtons.front()->get_active());
    CHECK(checkButtons.front()->get_label().find("#") != Glib::ustring::npos);
    checkButtons.front()->set_active(true);
    confirmation->response(Gtk::ResponseType::YES);
    drainGtkEvents();

    CHECK_FALSE(findList(fixture.runtime().musicLibrary(), listId));
    CHECK_FALSE(trackHasTag(fixture.runtime().musicLibrary(), trackId, "road-trip"));
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

  TEST_CASE("ListNavigationPanel - physical separator follows saved List section", "[gtk][regression][list]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto panel = ListNavigationPanel{{}};
    auto host = GtkWindowFixture{};
    host.mount(panel.widget());

    auto* const scrolledWindow = dynamic_cast<Gtk::ScrolledWindow*>(&panel.widget());
    REQUIRE(scrolledWindow != nullptr);
    auto* const listView = dynamic_cast<Gtk::ListView*>(scrolledWindow->get_child());
    REQUIRE(listView != nullptr);
    CHECK(hasCssClass(*listView, "ao-list-navigation"));

    auto visibleSeparatorCount = [&panel]
    {
      std::size_t count = 0;

      for (auto* const separator : collectAll<Gtk::Separator>(panel.widget()))
      {
        if (separator->get_visible() && hasCssClass(*separator, "ao-saved-list-separator"))
        {
          ++count;
        }
      }

      return count;
    };

    panel.rebuildTree(fixture.runtime().library());
    host.present();
    CHECK(visibleSeparatorCount() == 0);

    auto const listId = createList(fixture.runtime().library(), "Separated List");
    panel.rebuildTree(fixture.runtime().library());
    host.drain();
    CHECK(visibleSeparatorCount() == 1);

    REQUIRE(fixture.runtime().library().deleteList(listId));
    panel.rebuildTree(fixture.runtime().library());
    host.drain();
    CHECK(visibleSeparatorCount() == 0);
  }
} // namespace ao::gtk::test
