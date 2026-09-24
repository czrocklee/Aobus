// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListNavigationController.h"

#include "../../TestFixtureSupport.h"
#include "app/AppDialog.h"
#include "app/ThemeCoordinator.h"
#include "list/ListNavigationPanel.h"
#include "list/ListTreeItem.h"
#include "list/SmartListDialog.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "track/TrackRowCache.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/library/list/ListAuthoring.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/simpleaction.h>
#include <giomm/simpleactiongroup.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dialog.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/listview.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/treelistrow.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    ListId createList(rt::AppRuntime& runtime,
                      std::string const& name,
                      ListId parentId = kInvalidListId,
                      std::string expression = {})
    {
      return ao::test::requireValue(runGtkTask(runtime,
                                               uimodel::saveListAsync(&runtime.library(),
                                                                      rt::ListDraft{
                                                                        .parentId = parentId,
                                                                        .name = name,
                                                                        .expression = std::move(expression),
                                                                      })));
    }

    void deleteList(rt::AppRuntime& runtime, ListId const listId)
    {
      REQUIRE(runGtkTask(runtime, uimodel::deleteListAsync(&runtime.library(), listId, false)));
    }

    Glib::RefPtr<Gio::SimpleAction> simpleAction(Gio::ActionMap& actionMap, std::string const& name)
    {
      return std::dynamic_pointer_cast<Gio::SimpleAction>(actionMap.lookup_action(name));
    }

    std::optional<rt::ListNode> findList(rt::AppRuntime& runtime, ListId listId)
    {
      return runtime.library().snapshot().listNode(listId);
    }

    std::size_t countListsNamed(rt::AppRuntime& runtime, std::string_view const name)
    {
      return std::ranges::count_if(
        runtime.library().snapshot().lists(), [name](rt::ListNode const& node) { return node.name == name; });
    }

    Gtk::Entry* listNameEntry(SmartListDialog& dialog)
    {
      for (auto* const entry : collectAll<Gtk::Entry>(dialog))
      {
        if (entry->get_placeholder_text() == "List name")
        {
          return entry;
        }
      }

      return nullptr;
    }

    SmartListDialog* openNewListDialog(ListNavigationController& controller, TrackRowCache& cache)
    {
      auto groupPtr = Gio::SimpleActionGroup::create();
      auto registration = controller.addActionsTo(*groupPtr);
      auto const actionPtr = simpleAction(*groupPtr, "list-new-smart-list");

      controller.rebuildTree(cache);
      drainGtkEvents();
      controller.select(rt::kAllTracksListId);
      drainGtkEvents();

      if (!actionPtr || !actionPtr->get_enabled())
      {
        return nullptr;
      }

      actionPtr->activate();
      drainGtkEvents();
      return dynamic_cast<SmartListDialog*>(findAppDialogByTitle("New List"));
    }

    bool hasTrackTag(rt::AppRuntime& runtime, TrackId const trackId, std::string_view const tag)
    {
      auto const tags = runtime.library().snapshot().selectionTags(std::span{&trackId, std::size_t{1}});
      return std::ranges::contains(tags, tag);
    }

    utility::ScopedRegistration retireNavigationWidgets(Gtk::Window& parent)
    {
      return utility::ScopedRegistration{[&parent]
                                         {
                                           auto dialogs = std::vector<Gtk::Window*>{};

                                           for (auto* const window : Gtk::Window::list_toplevels())
                                           {
                                             if (window->get_transient_for() == &parent)
                                             {
                                               dialogs.push_back(window);
                                             }
                                           }

                                           for (auto* const dialog : dialogs)
                                           {
                                             dialog->close();
                                           }

                                           parent.unset_child();
                                           drainGtkEvents();
                                         }};
    }

    struct NavigationFixture final
    {
      NavigationFixture() { window.set_child(controller.widget()); }

      Glib::RefPtr<Gtk::Application> appPtr = ensureGtkApplication();
      GtkRuntimeFixture runtimeFixture{};
      Gtk::Window window;
      TrackRowCache cache{runtimeFixture.runtime().library(), ao::test::englishMessageCatalog()};
      ListId selectedId{999};
      bool rejectSelection = false;
      std::size_t selectionAttemptCount = 0;
      ListId savedPresentationListId = kInvalidListId;
      std::string savedPresentationId;
      ThemeCoordinator themeCoordinator;
      ListNavigationController controller{window,
                                          runtimeFixture.runtime(),
                                          ao::test::englishMessageCatalog(),
                                          {.onListSelected =
                                             [this](ListId id)
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
                                             [this](ListId id, std::string presentationId)
                                           {
                                             savedPresentationListId = id;
                                             savedPresentationId = std::move(presentationId);
                                           }},
                                          themeCoordinator};
      utility::ScopedRegistration widgetRetirement = retireNavigationWidgets(window);
    };

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

  TEST_CASE("ListNavigationController - routes saved-list selection", "[gtk][unit][list]")
  {
    auto state = NavigationFixture{};

    auto const testListId = createList(state.runtimeFixture.runtime(), "Select Target");

    state.controller.rebuildTree(state.cache);
    drainGtkEvents();

    state.controller.select(testListId);
    drainGtkEvents();

    CHECK(state.selectedId == testListId);
  }

  TEST_CASE("ListNavigationController - restores workspace selection and action state", "[gtk][unit][list]")
  {
    auto state = NavigationFixture{};

    SECTION("rebuildTree selects the active workspace list restored before the navigation model exists")
    {
      auto const restoredListId = createList(state.runtimeFixture.runtime(), "Restored Selection");
      REQUIRE(state.runtimeFixture.runtime().workspace().navigate({.target = restoredListId}));
      drainGtkEvents();

      state.controller.rebuildTree(state.cache);
      drainGtkEvents();

      CHECK(selectedNavigationListId(state.controller) == restoredListId);
    }

    SECTION("rebuildTree refreshes actions when the restored workspace list is already selected")
    {
      auto groupPtr = Gio::SimpleActionGroup::create();
      auto registration = state.controller.addActionsTo(*groupPtr);
      auto const newActionPtr = simpleAction(*groupPtr, "list-new-smart-list");
      REQUIRE(newActionPtr);
      REQUIRE(state.runtimeFixture.runtime().workspace().navigate({.target = rt::kAllTracksListId}));
      drainGtkEvents();

      state.controller.rebuildTree(state.cache);
      drainGtkEvents();

      CHECK(selectedNavigationListId(state.controller) == rt::kAllTracksListId);
      CHECK(newActionPtr->get_enabled());
    }
  }

  TEST_CASE("ListNavigationController - updates action availability from the selected list", "[gtk][unit][list]")
  {
    auto state = NavigationFixture{};

    auto groupPtr = Gio::SimpleActionGroup::create();
    auto registration = state.controller.addActionsTo(*groupPtr);

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

    auto const leafListId = createList(state.runtimeFixture.runtime(), "Leaf List");

    state.controller.rebuildTree(state.cache);
    drainGtkEvents();

    state.controller.select(leafListId);
    drainGtkEvents();
    CHECK(newActionPtr->get_enabled());
    CHECK(newPlaylistActionPtr->get_enabled());
    CHECK(editActionPtr->get_enabled());
    CHECK(deleteActionPtr->get_enabled());
    CHECK_FALSE(deleteSubtreeActionPtr->get_enabled());
  }

  TEST_CASE("ListNavigationController - opens the visible-tag playlist template", "[gtk][unit][list]")
  {
    auto state = NavigationFixture{};

    auto groupPtr = Gio::SimpleActionGroup::create();
    auto registration = state.controller.addActionsTo(*groupPtr);
    auto const newPlaylistActionPtr = simpleAction(*groupPtr, "list-new-playlist");
    REQUIRE(newPlaylistActionPtr);

    state.controller.rebuildTree(state.cache);
    drainGtkEvents();
    state.controller.select(rt::kAllTracksListId);
    drainGtkEvents();
    REQUIRE(newPlaylistActionPtr->get_enabled());

    newPlaylistActionPtr->activate();
    drainGtkEvents();

    auto* const dialog = dynamic_cast<SmartListDialog*>(findAppDialogByTitle("New Playlist"));
    REQUIRE(dialog != nullptr);
    CHECK(dialog->presentationId() == "list-order");
    dialog->close();
    drainGtkEvents();
  }

  TEST_CASE("ListNavigationController - ignores duplicate OK responses during submission",
            "[gtk][integration][list][concurrency]")
  {
    auto state = NavigationFixture{};

    auto* const dialog = openNewListDialog(state.controller, state.cache);
    REQUIRE(dialog != nullptr);
    auto* const nameEntry = listNameEntry(*dialog);
    REQUIRE(nameEntry != nullptr);
    auto* const okButton = findButtonByLabel(*dialog, "Create");
    REQUIRE(okButton != nullptr);
    nameEntry->set_text("Single submission");
    drainGtkEvents();
    REQUIRE(okButton->get_sensitive());

    dialog->response(Gtk::ResponseType::OK);
    CHECK_FALSE(okButton->get_sensitive());
    dialog->response(Gtk::ResponseType::OK);

    REQUIRE(tryPumpGtkEventsUntil([] { return findAppDialogByTitle("New List") == nullptr; }));
    CHECK(countListsNamed(state.runtimeFixture.runtime(), "Single submission") == 1);
  }

  TEST_CASE("ListNavigationController - presentation changes do not re-drive selection", "[gtk][unit][list]")
  {
    auto state = NavigationFixture{};

    auto const activeListId = createList(state.runtimeFixture.runtime(), "Active List");
    auto const browsedListId = createList(state.runtimeFixture.runtime(), "Browsed List");
    state.controller.rebuildTree(state.cache);
    REQUIRE(state.runtimeFixture.runtime().workspace().navigate({.target = activeListId}));
    drainGtkEvents();

    state.controller.select(browsedListId);
    drainGtkEvents();
    REQUIRE(state.selectedId == browsedListId);
    state.selectedId = kInvalidListId;
    auto const* const albums = rt::builtinTrackPresentationPreset("albums");
    REQUIRE(albums != nullptr);

    REQUIRE(state.runtimeFixture.runtime().workspace().setActivePresentation(albums->spec));
    drainGtkEvents();

    CHECK(state.selectedId == kInvalidListId);
  }

  TEST_CASE("ListNavigationController - selects submitted lists after synchronous publication rebuilds",
            "[gtk][unit][list]")
  {
    auto state = NavigationFixture{};

    auto* const dialog = openNewListDialog(state.controller, state.cache);
    REQUIRE(dialog != nullptr);
    auto* const nameEntry = listNameEntry(*dialog);
    REQUIRE(nameEntry != nullptr);
    nameEntry->set_text("Published selection");
    auto const presentationId = dialog->presentationId();
    state.selectedId = kInvalidListId;
    std::size_t rebuildCount = 0;
    auto changedSubscription = state.runtimeFixture.runtime().library().changes().onChanged(
      [&](rt::LibraryChangeSet const&)
      {
        ++rebuildCount;
        state.controller.rebuildTree(state.cache);
      });
    dialog->response(Gtk::ResponseType::OK);
    REQUIRE(tryPumpGtkEventsUntil([] { return findAppDialogByTitle("New List") == nullptr; }));
    auto const listId = state.savedPresentationListId;
    REQUIRE(listId != kInvalidListId);
    CHECK(rebuildCount == 1);
    CHECK(state.selectedId == listId);
    CHECK(selectedNavigationListId(state.controller) == listId);
    CHECK(state.savedPresentationId == presentationId);
  }

  TEST_CASE("ListNavigationController - reconciles rejected selections against newer authoritative state",
            "[gtk][unit][list][async]")
  {
    auto state = NavigationFixture{};

    SECTION("a failed pending selection is retried after the next complete rebuild")
    {
      auto* const dialog = openNewListDialog(state.controller, state.cache);
      REQUIRE(dialog != nullptr);
      auto* const nameEntry = listNameEntry(*dialog);
      REQUIRE(nameEntry != nullptr);
      nameEntry->set_text("Retry selection");
      state.rejectSelection = true;
      auto const attemptsBeforeSubmission = state.selectionAttemptCount;
      dialog->response(Gtk::ResponseType::OK);
      REQUIRE(tryPumpGtkEventsUntil([] { return findAppDialogByTitle("New List") == nullptr; }));
      auto const listId = state.savedPresentationListId;
      REQUIRE(listId != kInvalidListId);

      CHECK(state.selectedId != listId);
      CHECK(state.selectionAttemptCount == attemptsBeforeSubmission + 1);

      state.rejectSelection = false;
      state.controller.rebuildTree(state.cache);
      drainGtkEvents();

      CHECK(state.selectedId == listId);
      CHECK(state.selectionAttemptCount == attemptsBeforeSubmission + 2);
    }

    SECTION("a newer successful selection supersedes an earlier failed one")
    {
      auto const rejectedListId = createList(state.runtimeFixture.runtime(), "Rejected Target");
      auto const acceptedListId = createList(state.runtimeFixture.runtime(), "Accepted Target");

      state.controller.rebuildTree(state.cache);
      drainGtkEvents();

      state.rejectSelection = true;
      state.controller.select(rejectedListId);
      drainGtkEvents();
      REQUIRE(state.selectedId != rejectedListId);

      state.rejectSelection = false;
      state.controller.select(acceptedListId);
      drainGtkEvents();
      REQUIRE(state.selectedId == acceptedListId);

      auto const attemptsBeforeRebuild = state.selectionAttemptCount;

      state.controller.rebuildTree(state.cache);
      drainGtkEvents();

      CHECK(state.selectedId == acceptedListId);
      CHECK(state.selectionAttemptCount == attemptsBeforeRebuild);
    }

    SECTION("an authoritative workspace selection supersedes an earlier failed one")
    {
      auto const rejectedListId = createList(state.runtimeFixture.runtime(), "Rejected Target");
      auto const navigatedListId = createList(state.runtimeFixture.runtime(), "Navigated Target");

      state.controller.rebuildTree(state.cache);
      drainGtkEvents();

      state.rejectSelection = true;
      state.controller.select(rejectedListId);
      drainGtkEvents();
      REQUIRE(state.selectedId != rejectedListId);

      // External navigation is authoritative: the panel syncs to it silently,
      // so the stale pending selection must not survive into the next rebuild.
      state.rejectSelection = false;
      REQUIRE(state.runtimeFixture.runtime().workspace().navigate({.target = navigatedListId}));
      drainGtkEvents();
      CHECK(selectedNavigationListId(state.controller) == navigatedListId);

      auto const attemptsBeforeRebuild = state.selectionAttemptCount;

      state.controller.rebuildTree(state.cache);
      drainGtkEvents();

      CHECK(state.selectedId != rejectedListId);
      CHECK(state.selectionAttemptCount == attemptsBeforeRebuild);
      CHECK(selectedNavigationListId(state.controller) == navigatedListId);
    }

    SECTION("closing the last workspace view discards an earlier failed selection")
    {
      auto const rejectedListId = createList(state.runtimeFixture.runtime(), "Rejected Target");
      auto const activeViewId =
        ao::test::requireValue(state.runtimeFixture.runtime().workspace().navigate({.target = rt::kAllTracksListId}));
      state.controller.rebuildTree(state.cache);
      drainGtkEvents();

      state.rejectSelection = true;
      state.controller.select(rejectedListId);
      drainGtkEvents();
      REQUIRE(state.selectedId != rejectedListId);

      state.rejectSelection = false;
      REQUIRE(state.runtimeFixture.runtime().workspace().closeView(activeViewId));
      drainGtkEvents();
      REQUIRE(state.runtimeFixture.runtime().workspace().snapshot().activeViewId == rt::kInvalidViewId);
      auto const attemptsBeforeRebuild = state.selectionAttemptCount;

      state.controller.rebuildTree(state.cache);
      drainGtkEvents();

      CHECK(state.selectedId != rejectedListId);
      CHECK(state.selectionAttemptCount == attemptsBeforeRebuild);
    }
  }

  TEST_CASE("ListNavigationController - editing preserves the presentation callback", "[gtk][unit][list]")
  {
    auto state = NavigationFixture{};

    auto const listId = createList(state.runtimeFixture.runtime(), "Old Name");
    auto groupPtr = Gio::SimpleActionGroup::create();
    auto registration = state.controller.addActionsTo(*groupPtr);
    auto const editActionPtr = simpleAction(*groupPtr, "list-edit");
    REQUIRE(editActionPtr);
    state.controller.rebuildTree(state.cache);
    drainGtkEvents();
    state.controller.select(listId);
    drainGtkEvents();
    editActionPtr->activate();
    drainGtkEvents();
    auto* const dialog = dynamic_cast<SmartListDialog*>(findAppDialogByTitle("Edit List"));
    REQUIRE(dialog != nullptr);
    auto* const nameEntry = listNameEntry(*dialog);
    REQUIRE(nameEntry != nullptr);
    nameEntry->set_text("High Energy");
    auto const presentationId = dialog->presentationId();
    dialog->response(Gtk::ResponseType::OK);
    REQUIRE(tryPumpGtkEventsUntil([] { return findAppDialogByTitle("Edit List") == nullptr; }));

    auto const optList = findList(state.runtimeFixture.runtime(), listId);
    REQUIRE(optList);
    CHECK(optList->name == "High Energy");
    CHECK(state.savedPresentationListId == listId);
    CHECK(state.savedPresentationId == presentationId);

    state.controller.rebuildTree(state.cache);
    drainGtkEvents();

    CHECK(state.selectedId == listId);
  }

  TEST_CASE("ListNavigationController - stale edit responses preserve the visible draft", "[gtk][unit][list][async]")
  {
    auto state = NavigationFixture{};

    auto groupPtr = Gio::SimpleActionGroup::create();
    auto registration = state.controller.addActionsTo(*groupPtr);
    auto const editActionPtr = simpleAction(*groupPtr, "list-edit");
    REQUIRE(editActionPtr);
    auto const listId = createList(state.runtimeFixture.runtime(), "Draft to Preserve");
    state.controller.rebuildTree(state.cache);
    drainGtkEvents();
    state.controller.select(listId);
    drainGtkEvents();

    editActionPtr->activate();
    drainGtkEvents();
    auto* const dialog = dynamic_cast<SmartListDialog*>(findAppDialogByTitle("Edit List"));
    REQUIRE(dialog != nullptr);
    REQUIRE(dialog->get_visible());
    CHECK(dialog->draft().name == "Draft to Preserve");
    deleteList(state.runtimeFixture.runtime(), listId);

    dialog->response(Gtk::ResponseType::OK);

    CHECK(dialog->get_visible());
    CHECK(dialog->draft().name == "Draft to Preserve");
    bool visibleError = false;

    REQUIRE(tryPumpGtkEventsUntil(
      [&]
      {
        for (auto* const label : collectAll<Gtk::Label>(*dialog))
        {
          visibleError = visibleError || (label->get_visible() && label->has_css_class("ao-layout-error") &&
                                          !label->get_text().empty());
        }

        return visibleError;
      }));

    CHECK(visibleError);
    CHECK(state.savedPresentationListId == kInvalidListId);
    CHECK(state.savedPresentationId.empty());
    dialog->close();
    drainGtkEvents();
  }

  TEST_CASE("ListNavigationController - deletes the selected leaf and falls back to all tracks",
            "[gtk][integration][list]")
  {
    auto state = NavigationFixture{};

    auto groupPtr = Gio::SimpleActionGroup::create();
    auto registration = state.controller.addActionsTo(*groupPtr);

    auto const deleteActionPtr = simpleAction(*groupPtr, "list-delete");
    REQUIRE(deleteActionPtr);

    auto& runtime = state.runtimeFixture.runtime();
    auto const listId = createList(state.runtimeFixture.runtime(), "Delete Target");

    state.controller.rebuildTree(state.cache);
    drainGtkEvents();

    state.controller.select(listId);
    drainGtkEvents();
    REQUIRE(deleteActionPtr->get_enabled());

    deleteActionPtr->activate();

    CHECK(findList(runtime, listId));
    AppDialog* confirmation = nullptr;
    REQUIRE(tryPumpGtkEventsUntil(
      [&confirmation]
      {
        confirmation = findAppDialogByTitle("Delete List?");
        return confirmation != nullptr;
      }));
    REQUIRE(confirmation != nullptr);
    confirmation->response(Gtk::ResponseType::YES);
    REQUIRE(tryPumpGtkEventsUntil([&runtime, listId] { return !findList(runtime, listId); }));

    CHECK(!findList(runtime, listId));

    REQUIRE(tryPumpGtkEventsUntil(
      [&]
      {
        state.controller.rebuildTree(state.cache);
        return state.selectedId == rt::kAllTracksListId;
      }));

    CHECK(state.selectedId == rt::kAllTracksListId);
  }

  TEST_CASE("ListNavigationController - previews and deletes the selected derived subtree", "[gtk][integration][list]")
  {
    auto state = NavigationFixture{};

    auto groupPtr = Gio::SimpleActionGroup::create();
    auto registration = state.controller.addActionsTo(*groupPtr);
    auto const deleteActionPtr = simpleAction(*groupPtr, "list-delete");
    auto const deleteSubtreeActionPtr = simpleAction(*groupPtr, "list-delete-subtree");
    REQUIRE(deleteActionPtr);
    REQUIRE(deleteSubtreeActionPtr);

    auto& runtime = state.runtimeFixture.runtime();
    auto const parentId = createList(state.runtimeFixture.runtime(), "Delete Tree");
    auto const childId = createList(state.runtimeFixture.runtime(), "Delete Child", parentId);
    auto const grandchildId = createList(state.runtimeFixture.runtime(), "Delete Grandchild", childId);
    state.controller.rebuildTree(state.cache);
    drainGtkEvents();
    state.controller.select(parentId);
    drainGtkEvents();

    CHECK_FALSE(deleteActionPtr->get_enabled());
    REQUIRE(deleteSubtreeActionPtr->get_enabled());
    deleteSubtreeActionPtr->activate();

    AppDialog* confirmation = nullptr;
    REQUIRE(tryPumpGtkEventsUntil(
      [&confirmation]
      {
        confirmation = findAppDialogByTitle("Delete List and Descendants?");
        return confirmation != nullptr;
      }));
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
    REQUIRE(tryPumpGtkEventsUntil(
      [&runtime, parentId, childId, grandchildId]
      { return !findList(runtime, parentId) && !findList(runtime, childId) && !findList(runtime, grandchildId); }));

    CHECK_FALSE(findList(runtime, parentId));
    CHECK_FALSE(findList(runtime, childId));
    CHECK_FALSE(findList(runtime, grandchildId));
  }

  TEST_CASE("ListNavigationController - failed deletion preserves selection and a parent-bound error",
            "[gtk][unit][list]")
  {
    auto state = NavigationFixture{};

    auto groupPtr = Gio::SimpleActionGroup::create();
    auto registration = state.controller.addActionsTo(*groupPtr);
    auto const deleteActionPtr = simpleAction(*groupPtr, "list-delete");
    REQUIRE(deleteActionPtr);
    auto const listId = createList(state.runtimeFixture.runtime(), "Stale Delete Target");
    state.controller.rebuildTree(state.cache);
    drainGtkEvents();
    state.controller.select(listId);
    drainGtkEvents();
    REQUIRE(state.selectedId == listId);
    REQUIRE(selectedNavigationListId(state.controller) == listId);
    deleteList(state.runtimeFixture.runtime(), listId);

    deleteActionPtr->activate();

    AppDialog* dialog = nullptr;
    REQUIRE(tryPumpGtkEventsUntil(
      [&dialog]
      {
        dialog = findAppDialogByTitle("Unable to Delete List");
        return dialog != nullptr;
      }));
    REQUIRE(dialog != nullptr);
    CHECK(dialog->get_transient_for() == &state.window);
    CHECK(state.selectedId == listId);
    CHECK(selectedNavigationListId(state.controller) == listId);
    dialog->response(Gtk::ResponseType::CLOSE);
    drainGtkEvents();
  }

  TEST_CASE("ListNavigationController - registration retirement revokes retained actions", "[gtk][unit][list][async]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto cache = TrackRowCache{fixture.runtime().library(), ao::test::englishMessageCatalog()};
    auto actionGroupPtr = Gio::SimpleActionGroup::create();
    auto retainedActionPtr = Glib::RefPtr<Gio::SimpleAction>{};

    {
      auto themeCoordinator = ThemeCoordinator{};
      auto controller =
        ListNavigationController{window, fixture.runtime(), ao::test::englishMessageCatalog(), {}, themeCoordinator};
      auto widgetRetirement = retireNavigationWidgets(window);
      auto registration = controller.addActionsTo(*actionGroupPtr);
      retainedActionPtr = simpleAction(*actionGroupPtr, "list-new-smart-list");
      REQUIRE(retainedActionPtr);
      controller.rebuildTree(cache);
      controller.select(rt::kAllTracksListId);
      drainGtkEvents();
      REQUIRE(retainedActionPtr->get_enabled());
      REQUIRE(findAppDialogByTitle("New List") == nullptr);
      retainedActionPtr->activate();
      drainGtkEvents();
      auto* const dialog = findAppDialogByTitle("New List");
      REQUIRE(dialog != nullptr);
      dialog->close();
      drainGtkEvents();
      REQUIRE(findAppDialogByTitle("New List") == nullptr);
    }

    CHECK(actionGroupPtr->lookup_action("list-new-smart-list") == nullptr);
    REQUIRE(retainedActionPtr->get_enabled());
    retainedActionPtr->activate();
    drainGtkEvents();
    CHECK(findAppDialogByTitle("New List") == nullptr);
    CHECK(actionGroupPtr->lookup_action("list-new-smart-list") == nullptr);
  }

  TEST_CASE("ListNavigationController - replacement registration preserves current action availability",
            "[gtk][unit][list]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto cache = TrackRowCache{fixture.runtime().library(), ao::test::englishMessageCatalog()};
    auto themeCoordinator = ThemeCoordinator{};
    auto controller =
      ListNavigationController{window, fixture.runtime(), ao::test::englishMessageCatalog(), {}, themeCoordinator};
    window.set_child(controller.widget());
    auto widgetRetirement = retireNavigationWidgets(window);

    auto firstGroupPtr = Gio::SimpleActionGroup::create();
    auto firstRegistration = controller.addActionsTo(*firstGroupPtr);
    controller.rebuildTree(cache);
    drainGtkEvents();
    controller.select(rt::kAllTracksListId);
    drainGtkEvents();

    auto const firstNewListActionPtr = simpleAction(*firstGroupPtr, "list-new-smart-list");
    auto const firstDeleteActionPtr = simpleAction(*firstGroupPtr, "list-delete");
    REQUIRE(firstNewListActionPtr);
    REQUIRE(firstDeleteActionPtr);
    CHECK(firstNewListActionPtr->get_enabled());
    CHECK_FALSE(firstDeleteActionPtr->get_enabled());

    auto replacementGroupPtr = Gio::SimpleActionGroup::create();
    auto replacementRegistration = controller.addActionsTo(*replacementGroupPtr);
    auto const replacementNewListActionPtr = simpleAction(*replacementGroupPtr, "list-new-smart-list");
    auto const replacementNewPlaylistActionPtr = simpleAction(*replacementGroupPtr, "list-new-playlist");
    auto const replacementDeleteActionPtr = simpleAction(*replacementGroupPtr, "list-delete");
    auto const replacementDeleteSubtreeActionPtr = simpleAction(*replacementGroupPtr, "list-delete-subtree");
    auto const replacementEditActionPtr = simpleAction(*replacementGroupPtr, "list-edit");
    REQUIRE(replacementNewListActionPtr);
    REQUIRE(replacementNewPlaylistActionPtr);
    REQUIRE(replacementDeleteActionPtr);
    REQUIRE(replacementDeleteSubtreeActionPtr);
    REQUIRE(replacementEditActionPtr);
    CHECK(replacementNewListActionPtr->get_enabled());
    CHECK(replacementNewPlaylistActionPtr->get_enabled());
    CHECK_FALSE(replacementDeleteActionPtr->get_enabled());
    CHECK_FALSE(replacementDeleteSubtreeActionPtr->get_enabled());
    CHECK_FALSE(replacementEditActionPtr->get_enabled());
  }

  TEST_CASE("ListNavigationController - writable-tag delete offers optional tag cleanup",
            "[gtk][integration][list][list-delete]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{[&](library::MusicLibrary& library)
                                     {
                                       trackId = library::test::addTrackWithUniqueFixtureUri(
                                         library, {.title = "Tagged Track", .tags = {"road-trip"}});
                                     }};
    auto window = Gtk::Window{};
    auto cache = TrackRowCache{fixture.runtime().library(), ao::test::englishMessageCatalog()};
    auto themeCoordinator = ThemeCoordinator{};
    auto controller =
      ListNavigationController{window, fixture.runtime(), ao::test::englishMessageCatalog(), {}, themeCoordinator};
    window.set_child(controller.widget());
    auto widgetRetirement = retireNavigationWidgets(window);
    auto groupPtr = Gio::SimpleActionGroup::create();
    auto registration = controller.addActionsTo(*groupPtr);
    auto const deleteActionPtr = simpleAction(*groupPtr, "list-delete");
    REQUIRE(deleteActionPtr);
    auto const listId = createList(fixture.runtime(), "Road Trip", kInvalidListId, R"(#"road-trip")");

    controller.rebuildTree(cache);
    drainGtkEvents();
    controller.select(listId);
    drainGtkEvents();
    REQUIRE(deleteActionPtr->get_enabled());
    deleteActionPtr->activate();

    AppDialog* confirmation = nullptr;
    REQUIRE(tryPumpGtkEventsUntil(
      [&confirmation]
      {
        confirmation = findAppDialogByTitle("Delete List?");
        return confirmation != nullptr;
      }));
    REQUIRE(confirmation != nullptr);
    auto const checkButtons = collectAll<Gtk::CheckButton>(*confirmation);
    REQUIRE(checkButtons.size() == 1);
    CHECK_FALSE(checkButtons.front()->get_active());
    CHECK(checkButtons.front()->get_label().find("#") != Glib::ustring::npos);
    checkButtons.front()->set_active(true);
    confirmation->response(Gtk::ResponseType::YES);
    REQUIRE(tryPumpGtkEventsUntil(
      [&fixture, listId, trackId]
      { return !findList(fixture.runtime(), listId) && !hasTrackTag(fixture.runtime(), trackId, "road-trip"); }));

    CHECK_FALSE(findList(fixture.runtime(), listId));
    CHECK_FALSE(hasTrackTag(fixture.runtime(), trackId, "road-trip"));
  }

  TEST_CASE("ListNavigationPanel - retired selection model no longer drives callbacks", "[gtk][unit][list][async]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto const listId = createList(fixture.runtime(), "Retired Selection Source");
    auto selectedIds = std::vector<ListId>{};
    auto panel = ListNavigationPanel{
      ao::test::englishMessageCatalog(),
      {.onSelectionChanged = [&](ListId id) { selectedIds.push_back(id); }, .onContextMenuRequested = {}}};

    panel.rebuildTree(fixture.runtime().library());
    auto* const scrolledWindow = dynamic_cast<Gtk::ScrolledWindow*>(&panel.widget());
    REQUIRE(scrolledWindow != nullptr);
    auto* const listView = dynamic_cast<Gtk::ListView*>(scrolledWindow->get_child());
    REQUIRE(listView != nullptr);
    auto const retiredSelectionPtr = std::dynamic_pointer_cast<Gtk::SingleSelection>(listView->get_model());
    REQUIRE(retiredSelectionPtr);
    REQUIRE(retiredSelectionPtr->get_n_items() > 1);

    panel.selectList(rt::kAllTracksListId);
    selectedIds.clear();
    panel.selectList(listId);
    REQUIRE(selectedIds == std::vector{listId});

    panel.rebuildTree(fixture.runtime().library());
    auto const replacementSelectionPtr = std::dynamic_pointer_cast<Gtk::SingleSelection>(listView->get_model());
    REQUIRE(replacementSelectionPtr);
    CHECK(replacementSelectionPtr != retiredSelectionPtr);
    panel.selectList(rt::kAllTracksListId);
    selectedIds.clear();
    auto const replacementPosition = retiredSelectionPtr->get_selected() == 0 ? 1U : 0U;
    retiredSelectionPtr->set_selected(replacementPosition);

    CHECK(retiredSelectionPtr->get_selected() == replacementPosition);
    CHECK(selectedIds.empty());
    panel.selectList(listId);
    CHECK(selectedIds == std::vector{listId});
  }

  TEST_CASE("ListNavigationPanel - physical separator follows saved List section", "[gtk][unit][list]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto panel = ListNavigationPanel{ao::test::englishMessageCatalog(), {}};
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

    auto const listId = createList(fixture.runtime(), "Separated List");
    panel.rebuildTree(fixture.runtime().library());
    host.drain();
    CHECK(visibleSeparatorCount() == 1);

    deleteList(fixture.runtime(), listId);
    panel.rebuildTree(fixture.runtime().library());
    host.drain();
    CHECK(visibleSeparatorCount() == 0);
  }
} // namespace ao::gtk::test
