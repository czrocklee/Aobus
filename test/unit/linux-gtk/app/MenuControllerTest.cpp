// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MenuController.h"

#include "app/WindowActionRegistry.h"
#include "portal/ImportExportActions.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <giomm/actiongroup.h>
#include <gtkmm/applicationwindow.h>

#include <cstdint>

namespace ao::gtk::test
{
  namespace
  {
    class FakeImportExportActions final : public portal::ImportExportActions
    {
    public:
      void openLibrary() override { ++_openLibraryCount; }
      void scanLibrary() override { ++_scanLibraryCount; }
      void importLibrary() override { ++_importLibraryCount; }
      void exportLibrary() override { ++_exportLibraryCount; }

      std::int32_t openLibraryCount() const { return _openLibraryCount; }
      std::int32_t scanLibraryCount() const { return _scanLibraryCount; }
      std::int32_t importLibraryCount() const { return _importLibraryCount; }
      std::int32_t exportLibraryCount() const { return _exportLibraryCount; }

    private:
      std::int32_t _openLibraryCount = 0;
      std::int32_t _scanLibraryCount = 0;
      std::int32_t _importLibraryCount = 0;
      std::int32_t _exportLibraryCount = 0;
    };
  } // namespace

  TEST_CASE("WindowActionRegistry - actions dispatch to injected collaborators", "[gtk][unit][menu]")
  {
    auto const appPtr = ensureGtkApplication();
    auto importExport = FakeImportExportActions{};

    bool editLayoutCalled = false;
    bool resetCalled = false;
    bool savePanelsCalled = false;

    auto registry = WindowActionRegistry{
      importExport,
      WindowActionRegistry::Callbacks{
        .onEditLayout = [&editLayoutCalled] { editLayoutCalled = true; },
        .onResetRuntimeLayoutState = [&resetCalled] { resetCalled = true; },
        .onSaveCurrentPanelSizesAsLayoutDefaults = [&savePanelsCalled] { savePanelsCalled = true; },
      }};

    SECTION("file actions invoke import/export collaborators")
    {
      auto window = Gtk::ApplicationWindow{};
      window.set_application(appPtr);
      registry.install(window);

      auto* const actions = dynamic_cast<Gio::ActionGroup*>(&window);
      REQUIRE(actions != nullptr);

      actions->activate_action("open-library");
      CHECK(importExport.openLibraryCount() == 1);
      CHECK(importExport.scanLibraryCount() == 0);
      CHECK(importExport.importLibraryCount() == 0);
      CHECK(importExport.exportLibraryCount() == 0);

      actions->activate_action("scan-library");
      CHECK(importExport.scanLibraryCount() == 1);

      actions->activate_action("import-library");
      CHECK(importExport.importLibraryCount() == 1);

      actions->activate_action("export-library");
      CHECK(importExport.exportLibraryCount() == 1);
    }

    SECTION("view actions invoke callbacks")
    {
      auto window = Gtk::ApplicationWindow{};
      window.set_application(appPtr);
      registry.install(window);

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

  TEST_CASE("MenuController - builds menu model around window and app actions", "[gtk][unit][menu]")
  {
    auto controller = MenuController{};

    CHECK(controller.menuModel() == nullptr);

    controller.setup();

    CHECK(controller.menuModel() != nullptr);
  }
} // namespace ao::gtk::test
