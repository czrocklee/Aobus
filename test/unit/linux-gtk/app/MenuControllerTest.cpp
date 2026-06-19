// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MenuController.h"

#include "app/ThemeCoordinator.h"
#include "portal/ImportExportCoordinator.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <giomm/actiongroup.h>
#include <gtkmm/applicationwindow.h>
#include <gtkmm/window.h>

namespace ao::gtk::test
{
  using ao::gtk::portal::ImportExportCallbacks;
  using ao::gtk::portal::ImportExportCoordinator;

  // MenuController wires the window's "View" menu actions to injected callbacks. The contract worth
  // pinning is that dispatch: activating each window action invokes the matching std::function.
  // Menu *shape* (labels, ordering) is gtkmm glue and intentionally not asserted.
  TEST_CASE("MenuController - view actions dispatch to injected callbacks", "[gtk][app][menu]")
  {
    auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto parent = Gtk::Window{};
    auto theme = ThemeCoordinator{};
    auto callbacks = ImportExportCallbacks{.onOpenNewLibrary = {}, .onLibraryDataMutated = {}, .onTitleChanged = {}};
    auto coordinator = ImportExportCoordinator{parent, fixture.runtime(), callbacks, theme};

    bool editLayoutCalled = false;
    bool resetCalled = false;
    bool savePanelsCalled = false;

    auto controller = MenuController{coordinator,
                                     [&editLayoutCalled] { editLayoutCalled = true; },
                                     [&resetCalled] { resetCalled = true; },
                                     [&savePanelsCalled] { savePanelsCalled = true; }};

    SECTION("menu model is only built once setup runs")
    {
      CHECK(controller.menuModel() == nullptr);

      auto window = Gtk::ApplicationWindow{};
      window.set_application(appPtr);
      controller.setup(window);

      CHECK(controller.menuModel() != nullptr);
    }

    SECTION("each view action invokes its callback")
    {
      auto window = Gtk::ApplicationWindow{};
      window.set_application(appPtr);
      controller.setup(window);

      auto* const actions = dynamic_cast<Gio::ActionGroup*>(&window);
      REQUIRE(actions != nullptr);

      actions->activate_action("edit-layout");
      CHECK(editLayoutCalled);
      CHECK_FALSE(resetCalled);
      CHECK_FALSE(savePanelsCalled);

      actions->activate_action("reset-runtime-layout-state");
      CHECK(resetCalled);

      actions->activate_action("save-panel-sizes-as-layout-defaults");
      CHECK(savePanelsCalled);
    }
  }
} // namespace ao::gtk::test
