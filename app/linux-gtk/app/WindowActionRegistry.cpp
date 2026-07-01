// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/WindowActionRegistry.h"

#include "portal/ImportExportActions.h"

#include <giomm/simpleaction.h>
#include <glibmm/variant.h>
#include <gtkmm/applicationwindow.h>

#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  WindowActionRegistry::WindowActionRegistry(portal::ImportExportActions& importExport, Callbacks callbacks)
    : _importExport{importExport}, _callbacks{std::move(callbacks)}
  {
  }

  std::string WindowActionRegistry::detailedWindowAction(std::string_view const actionId)
  {
    auto detailed = std::string{"win."};
    detailed.append(actionId);
    return detailed;
  }

  void WindowActionRegistry::install(Gtk::ApplicationWindow& window)
  {
    auto openActionPtr = Gio::SimpleAction::create(kOpenLibrary);
    openActionPtr->signal_activate().connect([this](Glib::VariantBase const&) { _importExport.openLibrary(); });
    window.add_action(openActionPtr);

    auto scanActionPtr = Gio::SimpleAction::create(kScanLibrary);
    scanActionPtr->signal_activate().connect([this](Glib::VariantBase const&) { _importExport.scanLibrary(); });
    window.add_action(scanActionPtr);

    auto exportLibActionPtr = Gio::SimpleAction::create(kExportLibrary);
    exportLibActionPtr->signal_activate().connect([this](Glib::VariantBase const&) { _importExport.exportLibrary(); });
    window.add_action(exportLibActionPtr);

    auto importLibActionPtr = Gio::SimpleAction::create(kImportLibrary);
    importLibActionPtr->signal_activate().connect([this](Glib::VariantBase const&) { _importExport.importLibrary(); });
    window.add_action(importLibActionPtr);

    auto editLayoutActionPtr = Gio::SimpleAction::create(kEditLayout);
    editLayoutActionPtr->signal_activate().connect(
      [this](Glib::VariantBase const&)
      {
        if (_callbacks.onEditLayout)
        {
          _callbacks.onEditLayout();
        }
      });
    window.add_action(editLayoutActionPtr);

    auto savePanelSizesActionPtr = Gio::SimpleAction::create(kSavePanelSizesAsLayoutDefaults);
    savePanelSizesActionPtr->signal_activate().connect(
      [this](Glib::VariantBase const&)
      {
        if (_callbacks.onSaveCurrentPanelSizesAsLayoutDefaults)
        {
          _callbacks.onSaveCurrentPanelSizesAsLayoutDefaults();
        }
      });
    window.add_action(savePanelSizesActionPtr);

    auto resetRuntimeLayoutStateActionPtr = Gio::SimpleAction::create(kResetRuntimeLayoutState);
    resetRuntimeLayoutStateActionPtr->signal_activate().connect(
      [this](Glib::VariantBase const&)
      {
        if (_callbacks.onResetRuntimeLayoutState)
        {
          _callbacks.onResetRuntimeLayoutState();
        }
      });
    window.add_action(resetRuntimeLayoutStateActionPtr);
  }
} // namespace ao::gtk
