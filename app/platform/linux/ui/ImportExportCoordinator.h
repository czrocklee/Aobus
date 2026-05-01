// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 RockStudio Contributors

#pragma once

#include "platform/linux/ui/ImportProgressDialog.h"
#include "platform/linux/ui/LibrarySession.h"
#include <rs/library/ImportWorker.h>
#include <rs/library/Exporter.h>

#include <gtkmm.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace app::ui
{
  /**
   * ImportExportCallbacks defines the notifications from coordinator to host.
   */
  struct ImportExportCallbacks final
  {
    std::function<LibrarySession*()> getCurrentSession;
    std::function<void(std::unique_ptr<LibrarySession>)> onLibrarySessionCreated;
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
    ImportExportCoordinator(Gtk::Window& parent, ImportExportCallbacks callbacks);
    ~ImportExportCoordinator();

    void openLibrary();
    void importFiles();
    void importLibrary(); // YAML import
    void exportLibrary(); // YAML export

    void scanDirectory(std::filesystem::path const& dir, std::vector<std::filesystem::path>& files) const;

    void openMusicLibrary(std::filesystem::path const& path) const;
    void importFilesFromPath(std::filesystem::path const& path);

  private:
    LibrarySession* currentSession() const;

    void onImportFolderSelected(Glib::RefPtr<Gio::AsyncResult>& result, Glib::RefPtr<Gtk::FileDialog> const& dialog);
    void executeImportTask(std::vector<std::filesystem::path> const& files,
                           std::shared_ptr<std::unique_ptr<LibrarySession>> pendingSession = nullptr);
    void onImportProgress(std::string const& filePath, int index);
    void onImportFinished() const;

    void onLibraryImportSelected(Glib::RefPtr<Gio::AsyncResult>& result, Glib::RefPtr<Gtk::FileDialog> const& dialog);
    void runLibraryImportTask(std::filesystem::path const& path, LibrarySession* session);
    void reportImportResult(bool success, std::string const& errorText);

    void onExportModeConfirmed(int responseId, Gtk::DropDown* modeCombo, Gtk::Dialog* dialog);
    void onExportFileSelected(Glib::RefPtr<Gio::AsyncResult>& result,
                              rs::library::ExportMode mode,
                              Glib::RefPtr<Gtk::FileDialog> const& fileDialog);
    void executeExportTask(std::filesystem::path const& path, rs::library::ExportMode mode);

    Gtk::Window& _parent;
    ImportExportCallbacks _callbacks;

    std::unique_ptr<rs::library::ImportWorker> _importWorker;
    std::jthread _importThread;
    std::unique_ptr<ImportProgressDialog> _importDialog;
  };
} // namespace app::ui
