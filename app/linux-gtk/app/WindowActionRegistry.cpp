// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/WindowActionRegistry.h"

#include "common/ActionMapRegistration.h"
#include "portal/ImportExportActions.h"

#include <giomm/simpleaction.h>
#include <glibmm/variant.h>
#include <gtkmm/applicationwindow.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    constexpr std::size_t kWindowActionCount = 7;
  }

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

  ActionMapRegistration WindowActionRegistry::install(Gtk::ApplicationWindow& window)
  {
    auto registration = ActionMapRegistration{window, kWindowActionCount};

    auto openActionPtr = Gio::SimpleAction::create(kOpenLibrary);
    registration.add(openActionPtr, [this](Glib::VariantBase const&) { _importExport.openLibrary(); });

    auto scanActionPtr = Gio::SimpleAction::create(kScanLibrary);
    registration.add(scanActionPtr, [this](Glib::VariantBase const&) { _importExport.scanLibrary(); });

    auto exportLibActionPtr = Gio::SimpleAction::create(kExportLibrary);
    registration.add(exportLibActionPtr, [this](Glib::VariantBase const&) { _importExport.exportLibrary(); });

    auto importLibActionPtr = Gio::SimpleAction::create(kImportLibrary);
    registration.add(importLibActionPtr, [this](Glib::VariantBase const&) { _importExport.importLibrary(); });

    auto editLayoutActionPtr = Gio::SimpleAction::create(kEditLayout);
    registration.add(editLayoutActionPtr,
                     [this](Glib::VariantBase const&)
                     {
                       if (_callbacks.onEditLayout)
                       {
                         _callbacks.onEditLayout();
                       }
                     });

    auto savePanelSizesActionPtr = Gio::SimpleAction::create(kSavePanelSizesAsLayoutDefaults);
    registration.add(savePanelSizesActionPtr,
                     [this](Glib::VariantBase const&)
                     {
                       if (_callbacks.onSaveCurrentPanelSizesAsLayoutDefaults)
                       {
                         _callbacks.onSaveCurrentPanelSizesAsLayoutDefaults();
                       }
                     });

    auto resetRuntimeLayoutStateActionPtr = Gio::SimpleAction::create(kResetRuntimeLayoutState);
    registration.add(resetRuntimeLayoutStateActionPtr,
                     [this](Glib::VariantBase const&)
                     {
                       if (_callbacks.onResetRuntimeLayoutState)
                       {
                         _callbacks.onResetRuntimeLayoutState();
                       }
                     });

    return registration;
  }
} // namespace ao::gtk
