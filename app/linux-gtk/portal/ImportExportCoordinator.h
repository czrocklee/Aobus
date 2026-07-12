// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/ThemeCoordinator.h"
#include "portal/ImportExportActions.h"
#include "portal/ImportExportCallbacks.h"
#include "portal/LibraryImportExportWorkflow.h"
#include <ao/rt/library/LibraryYamlExporter.h>

#include <giomm/asyncresult.h>
#include <glibmm/refptr.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <filesystem>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::gtk
{
  class ThemeCoordinator;
}

namespace Gtk
{
  class DropDown;
}

namespace ao::gtk
{
  class AppDialog;
}

namespace ao::gtk::portal
{
  /**
   * ImportExportCoordinator owns the file/folder/mode chooser dialogs for library import/export and, once a
   * concrete path (and export mode) is resolved, delegates the background work to LibraryImportExportWorkflow.
   */
  class ImportExportCoordinator final : public ImportExportActions
  {
  public:
    ImportExportCoordinator(Gtk::Window& parent,
                            rt::AppRuntime& runtime,
                            ImportExportCallbacks callbacks,
                            ThemeCoordinator& themeCoordinator);

    // Not copyable or movable due to GTK and runtime references/subscriptions
    ImportExportCoordinator(ImportExportCoordinator const&) = delete;
    ImportExportCoordinator& operator=(ImportExportCoordinator const&) = delete;
    ImportExportCoordinator(ImportExportCoordinator&&) = delete;
    ImportExportCoordinator& operator=(ImportExportCoordinator&&) = delete;
    ~ImportExportCoordinator() override = default;

    ImportExportCallbacks& callbacks() { return _callbacks; }

    void openLibrary() override;
    void scanLibrary() override;
    void scanLibrary(ScanRequestMode mode);
    void importLibrary() override; // YAML import
    void exportLibrary() override; // YAML export

    void openMusicLibrary(std::filesystem::path const& path, bool scanAfterOpen = false) const;
    void importLibraryFrom(std::filesystem::path path);
    void exportLibraryTo(std::filesystem::path path, rt::ExportMode mode);

  private:
    void handleLibraryImportSelected(Glib::RefPtr<Gio::AsyncResult>& resultPtr,
                                     Glib::RefPtr<Gtk::FileDialog> const& dialogPtr);

    void handleExportModeConfirmed(std::int32_t responseId, Gtk::DropDown* modeCombo, AppDialog* dialog);
    void handleExportFileSelected(Glib::RefPtr<Gio::AsyncResult>& resultPtr,
                                  rt::ExportMode mode,
                                  Glib::RefPtr<Gtk::FileDialog> const& fileDialogPtr);

    Gtk::Window& _parent;
    ImportExportCallbacks _callbacks;
    ThemeCoordinator& _themeCoordinator;
    LibraryImportExportWorkflow _workflow;
  };
} // namespace ao::gtk::portal
