// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "portal/ImportExportCoordinator.h"

#include "app/AppDialog.h"
#include "app/ThemeCoordinator.h"
#include "i18n/GtkText.h"
#include "portal/ImportExportCallbacks.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkLayoutTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/library/LibraryTransfer.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <glib-object.h>
#include <gsl-lite/gsl-lite.hpp>
#include <gtkmm/dialog.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/enums.h>
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

  TEST_CASE("ImportExportCoordinator - localized export dialog preserves layout and actions",
            "[gtk][unit][portal][import-export][geometry]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto parent = Gtk::Window{};
    auto theme = ThemeCoordinator{};
    auto& runtime = fixture.runtime();

    for (auto const* const locale : {"en", "de", "es", "fr", "ja", "zh-Hans", "zh-Hant"})
    {
      INFO("locale " << locale);
      auto const textCatalog = ao::test::messageCatalog(locale);
      auto const title = gtkText(textCatalog, i18n::MessageId::LibrarySelectExportMode);
      auto const include = gtkText(textCatalog, i18n::MessageId::LibraryInclude);
      auto const full = gtkText(textCatalog, i18n::MessageId::LibraryExportModeFull);
      auto coordinator = portal::ImportExportCoordinator{parent,
                                                         runtime.async(),
                                                         runtime.library(),
                                                         runtime.notifications(),
                                                         textCatalog,
                                                         portal::ImportExportCallbacks{},
                                                         theme};

      coordinator.exportLibrary();

      auto* const exportModeDialog = findAppDialogByTitle(title);
      REQUIRE(exportModeDialog != nullptr);
      auto* const modeCombo = findWidget<Gtk::DropDown>(*exportModeDialog);
      REQUIRE(modeCombo != nullptr);
      CHECK(findLabelByText(*modeCombo, full) != nullptr);
      CHECK(findLabelByText(*exportModeDialog, include) == nullptr);
      CHECK(hasAccessibleLabel(*modeCombo, include));

      drainGtkEvents();
      auto const initialDialogHorizontal = measureWidget(*exportModeDialog, Gtk::Orientation::HORIZONTAL);

      for (auto const selectedIndex : {0U, 1U, 2U, 3U})
      {
        INFO("selection " << selectedIndex);
        modeCombo->set_selected(selectedIndex);
        drainGtkEvents();

        auto const dialogHorizontal = measureWidget(*exportModeDialog, Gtk::Orientation::HORIZONTAL);
        auto const modeHorizontal = measureWidget(*modeCombo, Gtk::Orientation::HORIZONTAL);
        auto const modeVertical = measureWidget(*modeCombo, Gtk::Orientation::VERTICAL);
        CHECK(modeHorizontal.minimum == modeHorizontal.natural);
        CHECK(modeVertical.minimum == modeVertical.natural);
        CHECK(dialogHorizontal.minimum >= modeHorizontal.natural);
        CHECK(dialogHorizontal.minimum == initialDialogHorizontal.minimum);
        CHECK(dialogHorizontal.natural == initialDialogHorizontal.natural);
      }

      auto* const nextButton = findButtonByLabel(*exportModeDialog, gtkText(textCatalog, i18n::MessageId::LibraryNext));
      REQUIRE(nextButton != nullptr);
      CHECK(exportModeDialog->get_default_widget() == nextButton);

      std::int32_t closeResponse = Gtk::ResponseType::NONE;
      exportModeDialog->signal_response().connect([&closeResponse](std::int32_t response)
                                                  { closeResponse = response; });
      exportModeDialog->close();
      CHECK(closeResponse == Gtk::ResponseType::CANCEL);
      drainGtkEvents();
    }
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
            "[gtk][unit][portal][import-export]")
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
    REQUIRE(tryPumpGtkEventsUntil(
      [&confirmationDialog]
      {
        confirmationDialog = findAppDialogByTitle("Confirm Restore");
        return confirmationDialog != nullptr;
      }));

    REQUIRE(confirmationDialog != nullptr);
    REQUIRE(confirmationDialog->get_visible());
    confirmationDialog->response(Gtk::ResponseType::CANCEL);
    drainGtkEvents();
  }

  TEST_CASE("ImportExportCoordinator - export mode response is ignored after coordinator teardown",
            "[gtk][unit][portal][import-export][async]")
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

    auto* const exportModeDialog = findAppDialogByTitle("Select Export Mode");

    REQUIRE(exportModeDialog != nullptr);
    REQUIRE(exportModeDialog->get_visible());

    auto const retireBeforeResponse = GENERATE(false, true);
    bool finalized = false;
    auto* const nativeDialog = G_OBJECT(exportModeDialog->gobj());
    auto const markFinalized = +[](void* data, GObject*) { *static_cast<bool*>(data) = true; };
    auto const detachWeakWatch = gsl_lite::finally(
      [&finalized, nativeDialog, markFinalized]
      {
        if (!finalized)
        {
          ::g_object_weak_unref(nativeDialog, markFinalized, &finalized);
        }
      });
    ::g_object_weak_ref(nativeDialog, markFinalized, &finalized);

    if (retireBeforeResponse)
    {
      coordinatorPtr.reset();
    }

    exportModeDialog->response(Gtk::ResponseType::CANCEL);
    drainGtkEvents();

    if (retireBeforeResponse)
    {
      REQUIRE_FALSE(finalized);
      CHECK(exportModeDialog->get_visible());
      exportModeDialog->close();
      drainGtkEvents();
    }
    else
    {
      CHECK(finalized);
    }
  }
} // namespace ao::gtk::test
