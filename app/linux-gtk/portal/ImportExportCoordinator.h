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
                            ThemeCoordinator& themeController);

    // Not copyable or movable due to GTK and runtime references/subscriptions
    ImportExportCoordinator(ImportExportCoordinator const&) = delete;
    ImportExportCoordinator& operator=(ImportExportCoordinator const&) = delete;
    ImportExportCoordinator(ImportExportCoordinator&&) = delete;
    ImportExportCoordinator& operator=(ImportExportCoordinator&&) = delete;
    ~ImportExportCoordinator() override = default;

    ImportExportCallbacks& callbacks() { return _callbacks; }

    void openLibrary() override;
    void scanLibrary() override;
    void importLibrary() override; // YAML import
    void exportLibrary() override; // YAML export

    void openMusicLibrary(std::filesystem::path const& path, bool scanAfterOpen = false) const;
    void importLibraryFrom(std::filesystem::path path);
    void exportLibraryTo(std::filesystem::path path, rt::ExportMode mode);

  private:
    void onLibraryImportSelected(Glib::RefPtr<Gio::AsyncResult>& result, Glib::RefPtr<Gtk::FileDialog> const& dialog);

    void onExportModeConfirmed(std::int32_t responseId, Gtk::DropDown* modeCombo, AppDialog* dialog);
    void onExportFileSelected(Glib::RefPtr<Gio::AsyncResult>& result,
                              rt::ExportMode mode,
                              Glib::RefPtr<Gtk::FileDialog> const& fileDialog);

    Gtk::Window& _parent;
    ImportExportCallbacks _callbacks;
    ThemeCoordinator& _themeController;
    LibraryImportExportWorkflow _workflow;
  };
} // namespace ao::gtk::portal
