// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "portal/ImportExportCoordinator.h"

#include "app/AppDialog.h"
#include "app/ThemeCoordinator.h"
#include "portal/ImportExportCallbacks.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include <ao/rt/library/LibraryTransfer.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/dialog.h>
#include <gtkmm/error.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>

namespace ao::gtk::test
{
  TEST_CASE("ImportExportCoordinator - maps every export dialog choice", "[gtk][unit][portal][import-export]")
  {
    CHECK(portal::detail::exportModeForSelection(0U) == rt::ExportMode::Delta);
    CHECK(portal::detail::exportModeForSelection(1U) == rt::ExportMode::Metadata);
    CHECK(portal::detail::exportModeForSelection(2U) == rt::ExportMode::Full);
    CHECK(portal::detail::exportModeForSelection(3U) == rt::ExportMode::ListOnly);
    CHECK(portal::detail::exportModeForSelection(99U) == rt::ExportMode::Metadata);
  }

  TEST_CASE("ImportExportCoordinator - suppresses native chooser cancellation only",
            "[gtk][unit][portal][import-export]")
  {
    CHECK_FALSE(portal::detail::isExpectedNativeChooserCancellation(Gtk::DialogError::FAILED));
    CHECK(portal::detail::isExpectedNativeChooserCancellation(Gtk::DialogError::CANCELLED));
    CHECK(portal::detail::isExpectedNativeChooserCancellation(Gtk::DialogError::DISMISSED));
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
    auto& runtime = fixture.runtime();
    auto coordinator = portal::ImportExportCoordinator{parent,
                                                       runtime.async(),
                                                       runtime.library(),
                                                       runtime.notifications(),
                                                       ao::test::englishMessageCatalog(),
                                                       callbacks,
                                                       theme};

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

  TEST_CASE("ImportExportCoordinator - installs its restore confirmation callback before import",
            "[gtk][regression][portal][import-export]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto parent = Gtk::Window{};
    auto theme = ThemeCoordinator{};
    auto& runtime = fixture.runtime();
    auto coordinator = portal::ImportExportCoordinator{parent,
                                                       runtime.async(),
                                                       runtime.library(),
                                                       runtime.notifications(),
                                                       ao::test::englishMessageCatalog(),
                                                       portal::ImportExportCallbacks{},
                                                       theme};
    auto const importPath = fixture.tempDir().path() / "restore.yaml";
    {
      auto yaml = std::ofstream{importPath};
      yaml << R"(version: 5
export_mode: full
library:
  resources: []
  tracks:
    - uri: restored.flac
      title: Restored
  lists: []
)";
    }

    coordinator.importLibraryFrom(importPath);

    AppDialog* confirmationDialog = nullptr;
    REQUIRE(pumpGtkEventsUntil(
      [&confirmationDialog]
      {
        for (auto* const window : Gtk::Window::list_toplevels())
        {
          if (auto* const dialog = dynamic_cast<AppDialog*>(window);
              dialog != nullptr && dialog->get_title() == "Confirm Library Restore")
          {
            confirmationDialog = dialog;
            return true;
          }
        }

        return false;
      }));

    REQUIRE(confirmationDialog != nullptr);
    REQUIRE(confirmationDialog->get_visible());
    confirmationDialog->response(Gtk::ResponseType::CANCEL);
    drainGtkEvents();
  }

  TEST_CASE("ImportExportCoordinator - export mode response is ignored after coordinator teardown",
            "[gtk][regression][import-export][concurrency]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto parent = Gtk::Window{};
    auto theme = ThemeCoordinator{};
    auto& runtime = fixture.runtime();
    auto coordinatorPtr = std::make_unique<portal::ImportExportCoordinator>(parent,
                                                                            runtime.async(),
                                                                            runtime.library(),
                                                                            runtime.notifications(),
                                                                            ao::test::englishMessageCatalog(),
                                                                            portal::ImportExportCallbacks{},
                                                                            theme);

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
