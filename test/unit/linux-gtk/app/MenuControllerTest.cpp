// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MenuController.h"

#include "app/WindowActionRegistry.h"
#include "portal/ImportExportActions.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gio/gio.h>
#include <giomm/action.h>
#include <giomm/simpleaction.h>
#include <gtk/gtk.h>
#include <gtkmm/applicationwindow.h>
#include <gtkmm/widget.h>
#include <sigc++/scoped_connection.h>

#include <cstdint>
#include <memory>
#include <string>

namespace ao::gtk::test
{
  namespace
  {
    auto menuLink(GMenuModel* menu, std::int32_t index, char const* link)
    {
      REQUIRE(menu != nullptr);
      REQUIRE(index >= 0);
      REQUIRE(index < ::g_menu_model_get_n_items(menu));
      auto resultPtr = std::unique_ptr<GMenuModel, decltype(&::g_object_unref)>{
        ::g_menu_model_get_item_link(menu, index, link), &::g_object_unref};
      REQUIRE(resultPtr);
      return resultPtr;
    }

    std::string menuStringAttribute(GMenuModel* menu, std::int32_t index, char const* attribute)
    {
      REQUIRE(menu != nullptr);
      REQUIRE(index >= 0);
      REQUIRE(index < ::g_menu_model_get_n_items(menu));
      auto valuePtr = std::unique_ptr<GVariant, decltype(&::g_variant_unref)>{
        ::g_menu_model_get_item_attribute_value(menu, index, attribute, G_VARIANT_TYPE_STRING), &::g_variant_unref};
      REQUIRE(valuePtr);
      return ::g_variant_get_string(valuePtr.get(), nullptr);
    }

    void activateMenuItem(Gtk::Widget& window, GMenuModel* menu, std::int32_t index)
    {
      auto const action = menuStringAttribute(menu, index, G_MENU_ATTRIBUTE_ACTION);
      REQUIRE_FALSE(action.empty());
      REQUIRE(::gtk_widget_activate_action(GTK_WIDGET(window.gobj()), action.c_str(), nullptr) != 0);
    }

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

  TEST_CASE("MenuController - window menu actions dispatch through WindowActionRegistry", "[gtk][unit][menu]")
  {
    auto const appPtr = ensureRegisteredGtkApplication();
    auto controller = MenuController{ao::test::englishMessageCatalog()};
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
      auto registration = registry.install(window);

      auto fileMenuPtr = menuLink(controller.menuModel()->gobj(), 0, G_MENU_LINK_SUBMENU);
      auto transferMenuPtr = menuLink(fileMenuPtr.get(), 2, G_MENU_LINK_SECTION);

      activateMenuItem(window, fileMenuPtr.get(), 0);
      CHECK(importExport.openLibraryCount() == 1);
      CHECK(importExport.scanLibraryCount() == 0);
      CHECK(importExport.importLibraryCount() == 0);
      CHECK(importExport.exportLibraryCount() == 0);

      activateMenuItem(window, fileMenuPtr.get(), 1);
      CHECK(importExport.scanLibraryCount() == 1);

      activateMenuItem(window, transferMenuPtr.get(), 0);
      CHECK(importExport.importLibraryCount() == 1);

      activateMenuItem(window, transferMenuPtr.get(), 1);
      CHECK(importExport.exportLibraryCount() == 1);
    }

    SECTION("view actions invoke callbacks")
    {
      auto window = Gtk::ApplicationWindow{};
      window.set_application(appPtr);
      auto registration = registry.install(window);

      auto viewMenuPtr = menuLink(controller.menuModel()->gobj(), 2, G_MENU_LINK_SUBMENU);

      activateMenuItem(window, viewMenuPtr.get(), 0);
      CHECK(editLayoutCalled);
      CHECK_FALSE(resetCalled);
      CHECK_FALSE(savePanelsCalled);

      activateMenuItem(window, viewMenuPtr.get(), 2);
      CHECK(resetCalled);

      activateMenuItem(window, viewMenuPtr.get(), 1);
      CHECK(savePanelsCalled);
    }
  }

  TEST_CASE("WindowActionRegistry - ending registration revokes old actions without removing replacements",
            "[gtk][unit][menu][async]")
  {
    auto const appPtr = ensureRegisteredGtkApplication();
    auto window = Gtk::ApplicationWindow{};
    window.set_application(appPtr);

    std::int32_t oldActivationCount = 0;
    std::int32_t replacementActivationCount = 0;
    auto retainedOldActionPtr = Glib::RefPtr<Gio::Action>{};
    auto replacementActionPtr = Gio::SimpleAction::create(WindowActionRegistry::kEditLayout);
    auto replacementConnection = sigc::scoped_connection{replacementActionPtr->signal_activate().connect(
      [&replacementActivationCount](Glib::VariantBase const&) { ++replacementActivationCount; })};

    {
      auto importExport = FakeImportExportActions{};
      auto registry = WindowActionRegistry{importExport,
                                           WindowActionRegistry::Callbacks{
                                             .onEditLayout = [&oldActivationCount] { ++oldActivationCount; },
                                             .onResetRuntimeLayoutState = {},
                                             .onSaveCurrentPanelSizesAsLayoutDefaults = {},
                                           }};
      auto registration = registry.install(window);
      retainedOldActionPtr = window.lookup_action(WindowActionRegistry::kEditLayout);
      REQUIRE(retainedOldActionPtr);

      window.add_action(replacementActionPtr);
      registration.reset();

      auto const currentActionPtr = window.lookup_action(WindowActionRegistry::kEditLayout);
      REQUIRE(currentActionPtr);
      CHECK(currentActionPtr.get() == replacementActionPtr.get());
    }

    retainedOldActionPtr->activate();
    CHECK(oldActivationCount == 0);

    replacementActionPtr->activate();
    CHECK(replacementActivationCount == 1);
  }

  TEST_CASE("MenuController - builds menu model around window and app actions", "[gtk][unit][menu]")
  {
    auto const appPtr = ensureGtkApplication();
    auto const controller = MenuController{ao::test::englishMessageCatalog()};

    REQUIRE(controller.menuModel() != nullptr);

    CHECK(menuStringAttribute(controller.menuModel()->gobj(), 0, G_MENU_ATTRIBUTE_LABEL) == "File");
  }
} // namespace ao::gtk::test
