// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "library_io/ImportProgressDialog.h"
#include <ao/library/Exporter.h>
#include <ao/library/ImportWorker.h>
#include <runtime/CorePrimitives.h>

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
    std::function<void(std::string const&)> onTitleChanged;
  };

  /**
   * ImportExportCoordinator manages file dialogs and background import/export tasks.
   */
  class ImportExportCoordinator final
  {
  public:
    ImportExportCoordinator(Gtk::Window& parent, rt::AppSession& session, ImportExportCallbacks callbacks);
    ~ImportExportCoordinator();

    // Not copyable or movable due to GTK and session references/subscriptions
    ImportExportCoordinator(ImportExportCoordinator const&) = delete;
    ImportExportCoordinator& operator=(ImportExportCoordinator const&) = delete;
    ImportExportCoordinator(ImportExportCoordinator&&) = delete;
    ImportExportCoordinator& operator=(ImportExportCoordinator&&) = delete;

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
    void onImportFinished() const;

    void onLibraryImportSelected(Glib::RefPtr<Gio::AsyncResult>& result, Glib::RefPtr<Gtk::FileDialog> const& dialog);
    void runLibraryImportTask(std::filesystem::path const& path);
    void reportImportResult(bool success, std::string const& errorText);

    void onExportModeConfirmed(int responseId, Gtk::DropDown* modeCombo, Gtk::Dialog* dialog);
    void onExportFileSelected(Glib::RefPtr<Gio::AsyncResult>& result,
                              library::ExportMode mode,
                              Glib::RefPtr<Gtk::FileDialog> const& fileDialog);
    void executeExportTask(std::filesystem::path const& path, library::ExportMode mode);

    Gtk::Window& _parent;
    rt::AppSession& _session;
    ImportExportCallbacks _callbacks;

    rt::Subscription _importProgressSub;
    rt::Subscription _importCompleteSub;
    std::jthread _exportThread;
    std::jthread _importTaskThread;
    std::unique_ptr<ImportProgressDialog> _importDialog;
  };
} // namespace ao::gtk
