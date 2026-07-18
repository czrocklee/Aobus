// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TagEditController.h"

#include "app/ThemeCoordinator.h"
#include "image/ImageCache.h"
#include "image/ThumbnailLoader.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include "track/TrackViewPage.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/simpleactiongroup.h>
#include <gtk/gtkwidget.h>
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/window.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace ao::gtk::test
{
  TEST_CASE("TagEditController - binds tag actions and routes submitted tag mutations", "[gtk][unit][tag]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto firstTrackId = kInvalidTrackId;
    auto secondTrackId = kInvalidTrackId;
    auto fixture =
      GtkRuntimeFixture{[&](library::MusicLibrary& library)
                        {
                          firstTrackId = library::test::addTrack(library, {.title = "Controller Target 1"});
                          secondTrackId = library::test::addTrack(library, {.title = "Controller Target 2"});
                        }};
    auto window = Gtk::Window{};

    auto themeCoordinator = ThemeCoordinator{};
    std::int32_t mutationCallbacks = 0;
    auto callbacks = TagEditController::Callbacks{.onTagsMutated = [&mutationCallbacks] { ++mutationCallbacks; }};

    auto controller = TagEditController{window, fixture.runtime(), std::move(callbacks), themeCoordinator};

    SECTION("registers tag actions")
    {
      auto groupPtr = Gio::SimpleActionGroup::create();
      controller.addActionsTo(*groupPtr);

      auto addActionPtr = std::dynamic_pointer_cast<Gio::SimpleAction>(groupPtr->lookup_action("track-tag-add"));
      REQUIRE(addActionPtr);

      addActionPtr->activate(Glib::Variant<Glib::ustring>::create("ActionTag"));
      drainGtkEvents();
      CHECK(mutationCallbacks == 0);
    }

    SECTION("submitTagChanges reports the mutation to the controller callback")
    {
      auto const selection =
        TrackSelection{.listId = rt::kAllTracksListId, .selectedIds = {firstTrackId, secondTrackId}};
      auto const tagsToAdd = std::array<std::string, 1>{"ControllerTag"};

      controller.submitTagChanges(selection, tagsToAdd, std::span<std::string const>{});

      CHECK(mutationCallbacks == 1);
    }
  }

  TEST_CASE("TagEditController - tag popover attachment follows the anchor lifetime", "[gtk][regression][tag]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{[&](library::MusicLibrary& library)
                                     { trackId = library::test::addTrack(library, {.title = "Popover Target"}); }};
    auto window = Gtk::Window{};
    auto anchor = Gtk::Box{};
    window.set_child(anchor);
    window.present();
    drainGtkEvents();

    auto themeCoordinator = ThemeCoordinator{};
    auto controller = TagEditController{window, fixture.runtime(), {}, themeCoordinator};
    auto const selection = TrackSelection{.listId = rt::kAllTracksListId, .selectedIds = {trackId}};

    controller.openTagEditor(selection, anchor);
    REQUIRE(collectAll<Gtk::Popover>(anchor).size() == 1);

    controller.openTagEditor(selection, anchor);
    CHECK(collectAll<Gtk::Popover>(anchor).size() == 1);

    window.unset_child();
    drainGtkEvents();
    CHECK(collectAll<Gtk::Popover>(anchor).empty());

    window.set_child(anchor);
    window.present();
    drainGtkEvents();
    controller.openTagEditor(selection, anchor);
    CHECK(collectAll<Gtk::Popover>(anchor).size() == 1);
  }

  TEST_CASE("TagEditController - Edit Tags survives context popover close-before-action ordering",
            "[gtk][regression][tag]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{[&](library::MusicLibrary& library)
                                     { trackId = library::test::addTrack(library, {.title = "Context Target"}); }};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library()};
    auto imageCache = ImageCache{200};
    auto thumbnailLoader = ThumbnailLoader{runtime.library(), imageCache, runtime.async()};
    auto modelPtr = TrackListModel::create(cache);
    auto layoutStore = uimodel::TrackColumnLayoutStore{};
    auto page = TrackViewPage{rt::kAllTracksListId, modelPtr, layoutStore, runtime, thumbnailLoader};
    auto window = Gtk::Window{};
    window.set_child(page);
    window.present();
    drainGtkEvents();

    auto themeCoordinator = ThemeCoordinator{};
    auto controller = TagEditController{window, runtime, {}, themeCoordinator};
    auto const selection = TrackSelection{.listId = rt::kAllTracksListId, .selectedIds = {trackId}};
    controller.openTrackContextMenu(page, selection, 20.0, 20.0);
    drainGtkEvents();

    auto const contextPopovers = collectAll<Gtk::PopoverMenu>(page);
    REQUIRE(contextPopovers.size() == 1);
    auto* const editTagsLabel = findLabelByText(*contextPopovers.front(), "Edit Tags");
    REQUIRE(editTagsLabel != nullptr);
    auto* const modelButton = editTagsLabel->get_parent();
    REQUIRE(modelButton != nullptr);

    CHECK(::gtk_widget_activate(modelButton->gobj()));
    drainGtkEvents();

    auto const popovers = collectAll<Gtk::Popover>(page);
    REQUIRE(popovers.size() == 1);
    CHECK(dynamic_cast<Gtk::PopoverMenu*>(popovers.front()) == nullptr);
  }
} // namespace ao::gtk::test
