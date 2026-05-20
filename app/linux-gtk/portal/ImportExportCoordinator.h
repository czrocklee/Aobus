// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "portal/ImportProgressDialog.h"
#include "runtime/CorePrimitives.h"
#include "runtime/LibraryExporter.h"
#include "runtime/async/LifetimeScope.h"

#include <giomm/asyncresult.h>
#include <glibmm/refptr.h>
#include <gtkmm/dialog.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/window.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::gtk::portal
{
  /**
   * ImportExportCallbacks defines the notifications from coordinator to host.
   */
  struct ImportExportCallbacks final
  {
    std::function<void(std::filesystem::path const&)> onOpenNewLibrary;
    std::function<void()> onLibraryDataMutated;
    std::function<void(std::string const&)> onTitleChanged;
  };

  /**
   * ImportExportCoordinator manages file dialogs and background import/export tasks.
   */
  class ImportExportCoordinator final
  {
  public:
    ImportExportCoordinator(Gtk::Window& parent, rt::AppRuntime& runtime, ImportExportCallbacks callbacks);
    ~ImportExportCoordinator();

    // Not copyable or movable due to GTK and runtime references/subscriptions
    ImportExportCoordinator(ImportExportCoordinator const&) = delete;
    ImportExportCoordinator& operator=(ImportExportCoordinator const&) = delete;
    ImportExportCoordinator(ImportExportCoordinator&&) = delete;
    ImportExportCoordinator& operator=(ImportExportCoordinator&&) = delete;

    ImportExportCallbacks& callbacks() { return _callbacks; }

    void openLibrary();
    void importFiles();
    void importLibrary(); // YAML import
    void exportLibrary(); // YAML export

    void openMusicLibrary(std::filesystem::path const& path) const;
    void importFilesFromPath(std::filesystem::path const& path);

  private:
    void onImportFolderSelected(Glib::RefPtr<Gio::AsyncResult>& result, Glib::RefPtr<Gtk::FileDialog> const& dialog);
    void executeImportTask(std::vector<std::filesystem::path> const& files, bool isNewLibrary);
    void onImportFinished() const;

    void onLibraryImportSelected(Glib::RefPtr<Gio::AsyncResult>& result, Glib::RefPtr<Gtk::FileDialog> const& dialog);

    void onExportModeConfirmed(int responseId, Gtk::DropDown* modeCombo, Gtk::Dialog* dialog);
    void onExportFileSelected(Glib::RefPtr<Gio::AsyncResult>& result,
                              rt::ExportMode mode,
                              Glib::RefPtr<Gtk::FileDialog> const& fileDialog);
    void executeExportTask(std::filesystem::path const& path, rt::ExportMode mode);

    Gtk::Window& _parent;
    rt::AppRuntime& _runtime;
    ImportExportCallbacks _callbacks;

    rt::Subscription _importProgressSub;
    rt::Subscription _importCompleteSub;
    rt::async::LifetimeScope _tasks;
    std::unique_ptr<ImportProgressDialog> _importDialog;
  };
} // namespace ao::gtk::portal
