// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "library_io/ImportProgressDialog.h"
#include <ao/library/Exporter.h>
#include <ao/library/ImportWorker.h>

#include <gtkmm.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ao::rt
{
  class AppSession;
}

namespace ao::gtk
{
  /**
   * ImportExportCallbacks defines the notifications from coordinator to host.
   */
  struct ImportExportCallbacks final
  {
    std::function<void(std::filesystem::path const&)> onOpenNewLibrary;
    std::function<void()> onLibraryDataMutated;
    std::function<void(double, std::string const&)> onProgressUpdated;
    std::function<void(std::string const&)> onStatusMessage;
    std::function<void(std::string const&)> onTitleChanged;
  };

  /**
   * ImportExportCoordinator manages file dialogs and background import/export tasks.
   */
  class ImportExportCoordinator final
  {
  public:
    ImportExportCoordinator(Gtk::Window& parent, ao::rt::AppSession& session, ImportExportCallbacks callbacks);
    ~ImportExportCoordinator();

    ImportExportCallbacks& callbacks() { return _callbacks; }

    void openLibrary();
    void importFiles();
    void importLibrary(); // YAML import
    void exportLibrary(); // YAML export

    void scanDirectory(std::filesystem::path const& dir, std::vector<std::filesystem::path>& files) const;

    void openMusicLibrary(std::filesystem::path const& path) const;
    void importFilesFromPath(std::filesystem::path const& path);

  private:
    void onImportFolderSelected(Glib::RefPtr<Gio::AsyncResult>& result, Glib::RefPtr<Gtk::FileDialog> const& dialog);
    void executeImportTask(std::vector<std::filesystem::path> const& files, bool isNewLibrary);
    void onImportProgress(std::string const& filePath, int index);
    void onImportFinished() const;

    void onLibraryImportSelected(Glib::RefPtr<Gio::AsyncResult>& result, Glib::RefPtr<Gtk::FileDialog> const& dialog);
    void runLibraryImportTask(std::filesystem::path const& path);
    void reportImportResult(bool success, std::string const& errorText);

    void onExportModeConfirmed(int responseId, Gtk::DropDown* modeCombo, Gtk::Dialog* dialog);
    void onExportFileSelected(Glib::RefPtr<Gio::AsyncResult>& result,
                              ao::library::ExportMode mode,
                              Glib::RefPtr<Gtk::FileDialog> const& fileDialog);
    void executeExportTask(std::filesystem::path const& path, ao::library::ExportMode mode);

    Gtk::Window& _parent;
    ao::rt::AppSession& _session;
    ImportExportCallbacks _callbacks;

    std::unique_ptr<ao::library::ImportWorker> _importWorker;
    std::jthread _importThread;
    std::jthread _exportThread;
    std::jthread _importTaskThread;
    std::unique_ptr<ImportProgressDialog> _importDialog;
  };
} // namespace ao::gtk
