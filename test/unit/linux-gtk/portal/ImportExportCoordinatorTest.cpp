// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "portal/ImportExportCoordinator.h"

#include "app/AppDialog.h"
#include "app/ThemeCoordinator.h"
#include "portal/ImportExportCallbacks.h"
#include "portal/ImportExportCoordinatorPolicy.h"
#include "test/unit/TestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/library/LibraryYamlExporter.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/dialog.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>

namespace ao::gtk::test
{
  TEST_CASE("ImportExportCoordinator - policy maps dialog choices", "[gtk][unit][portal][import-export]")
  {
    SECTION("new library folders request an initial scan")
    {
      auto const tempDir = ao::test::TempDir{};
      auto const libraryPath = tempDir.path() / "library";
      std::filesystem::create_directories(libraryPath);

      CHECK(portal::shouldScanAfterOpen(libraryPath));

      auto const databasePath = portal::defaultLibraryDatabasePath(libraryPath) / "data.mdb";
      std::filesystem::create_directories(databasePath.parent_path());
      auto databaseFile = std::ofstream{databasePath};
      databaseFile << "existing database marker";
      databaseFile.close();

      CHECK_FALSE(portal::shouldScanAfterOpen(libraryPath));
    }

    SECTION("export dropdown indices select runtime export modes")
    {
      CHECK(portal::exportModeForSelection(0U) == rt::ExportMode::Delta);
      CHECK(portal::exportModeForSelection(1U) == rt::ExportMode::Metadata);
      CHECK(portal::exportModeForSelection(2U) == rt::ExportMode::Full);
      CHECK(portal::exportModeForSelection(3U) == rt::ExportMode::ListOnly);
      CHECK(portal::exportModeForSelection(99U) == rt::ExportMode::Metadata);
    }
  }

  TEST_CASE("ImportExportCoordinator - openMusicLibrary routes to the callback", "[gtk][unit][portal][import-export]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto parent = Gtk::Window{};
    auto theme = ThemeCoordinator{};

    auto receivedPath = std::filesystem::path{};
    bool receivedScanAfterOpen = false;
    std::int32_t openCallbackCount = 0;
    auto callbacks = portal::ImportExportCallbacks{
      .onOpenNewLibrary =
        [&receivedPath, &receivedScanAfterOpen, &openCallbackCount](
          std::filesystem::path const& path, bool const scanAfterOpen)
      {
        receivedPath = path;
        receivedScanAfterOpen = scanAfterOpen;
        ++openCallbackCount;
      },
    };
    auto coordinator = portal::ImportExportCoordinator{parent, fixture.runtime(), callbacks, theme};

    SECTION("default open does not request a scan")
    {
      auto const target = std::filesystem::path{fixture.tempDir().path() / "new_library"};

      coordinator.openMusicLibrary(target);

      CHECK(openCallbackCount == 1);
      CHECK(receivedPath == target);
      CHECK_FALSE(receivedScanAfterOpen);
    }

    SECTION("explicit open forwards the initial scan request")
    {
      auto const target = std::filesystem::path{fixture.tempDir().path() / "new_library"};

      coordinator.openMusicLibrary(target, true);

      CHECK(openCallbackCount == 1);
      CHECK(receivedPath == target);
      CHECK(receivedScanAfterOpen);
    }
  }

  TEST_CASE("ImportExportCoordinator - export mode response is ignored after coordinator teardown",
            "[gtk][regression][import-export][concurrency]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto parent = Gtk::Window{};
    auto theme = ThemeCoordinator{};
    auto coordinatorPtr = std::make_unique<portal::ImportExportCoordinator>(
      parent, fixture.runtime(), portal::ImportExportCallbacks{}, theme);

    coordinatorPtr->exportLibrary();

    AppDialog* exportModeDialog = nullptr;

    for (auto* const window : Gtk::Window::list_toplevels())
    {
      if (auto* const dialog = dynamic_cast<AppDialog*>(window);
          dialog != nullptr && dialog->get_title() == "Select Export Mode")
      {
        exportModeDialog = dialog;
        break;
      }
    }

    REQUIRE(exportModeDialog != nullptr);
    REQUIRE(exportModeDialog->get_visible());

    coordinatorPtr.reset();
    exportModeDialog->response(Gtk::ResponseType::CANCEL);
    drainGtkEvents();

    CHECK(exportModeDialog->get_visible());
    exportModeDialog->close();
    drainGtkEvents();
  }
} // namespace ao::gtk::test
