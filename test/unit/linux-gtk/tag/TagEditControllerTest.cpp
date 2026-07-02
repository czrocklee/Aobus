// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TagEditController.h"

#include "app/ThemeCoordinator.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/simpleactiongroup.h>
#include <gtkmm/window.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace ao::gtk::test
{
  namespace
  {
    TrackId createTrack(library::MusicLibrary& library, std::string const& title)
    {
      return library::test::addTrack(library, {.title = title});
    }
  } // namespace

  TEST_CASE("TagEditController binds tag actions and routes submitted tag mutations", "[gtk][unit][tag]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};

    auto themeController = ThemeCoordinator{};
    std::int32_t mutationCallbacks = 0;
    auto callbacks = TagEditController::Callbacks{.onTagsMutated = [&mutationCallbacks] { ++mutationCallbacks; }};

    auto controller = TagEditController{window, fixture.runtime(), std::move(callbacks), themeController};

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
      auto& library = fixture.runtime().musicLibrary();
      auto const firstTrackId = createTrack(library, "Controller Target 1");
      auto const secondTrackId = createTrack(library, "Controller Target 2");
      auto const selection =
        TrackSelectionContext{.listId = rt::kAllTracksListId, .selectedIds = {firstTrackId, secondTrackId}};
      auto const tagsToAdd = std::array<std::string, 1>{"ControllerTag"};

      controller.submitTagChanges(selection, tagsToAdd, std::span<std::string const>{});

      CHECK(mutationCallbacks == 1);
    }
  }
} // namespace ao::gtk::test
