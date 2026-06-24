// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/ThemeCoordinator.h"
#include <ao/async/LifetimeScope.h>
#include <ao/async/Task.h>
#include <ao/library/LibraryScanner.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/library/LibraryYamlExporter.h>

#include <giomm/asyncresult.h>
#include <glibmm/refptr.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

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
  class LibraryTaskProgressDialog;

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
    ImportExportCoordinator(Gtk::Window& parent,
                            rt::AppRuntime& runtime,
                            ImportExportCallbacks callbacks,
                            ThemeCoordinator& themeController);
    ~ImportExportCoordinator();

    // Not copyable or movable due to GTK and runtime references/subscriptions
    ImportExportCoordinator(ImportExportCoordinator const&) = delete;
    ImportExportCoordinator& operator=(ImportExportCoordinator const&) = delete;
    ImportExportCoordinator(ImportExportCoordinator&&) = delete;
    ImportExportCoordinator& operator=(ImportExportCoordinator&&) = delete;

    ImportExportCallbacks& callbacks() { return _callbacks; }

    void openLibrary();
    void scanLibrary();
    void importLibrary(); // YAML import
    void exportLibrary(); // YAML export

    void openMusicLibrary(std::filesystem::path const& path) const;
    void importLibraryFrom(std::filesystem::path path);
    void exportLibraryTo(std::filesystem::path path, rt::ExportMode mode);

  private:
    void onImportFinished() const;

    void onLibraryImportSelected(Glib::RefPtr<Gio::AsyncResult>& result, Glib::RefPtr<Gtk::FileDialog> const& dialog);
    async::Task<void> importLibraryTask(std::filesystem::path importPath);

    // Scan-library pipeline split into coroutine + sync helpers so that
    // scanLibrary() itself stays a flat orchestrator.
    async::Task<std::optional<library::ScanPlan>> buildScanPlanOrReportFailure();
    async::Task<void> applyScanPlanWithProgress(library::ScanPlan plan);
    // Returns true when the plan has no New/Changed/Missing items; in that case
    // the appropriate notification is posted and the caller should return.
    bool reportIfNoActionableWork(library::ScanPlan const& plan);

    void onExportModeConfirmed(std::int32_t responseId, Gtk::DropDown* modeCombo, AppDialog* dialog);
    void onExportFileSelected(Glib::RefPtr<Gio::AsyncResult>& result,
                              rt::ExportMode mode,
                              Glib::RefPtr<Gtk::FileDialog> const& fileDialog);

    Gtk::Window& _parent;
    rt::AppRuntime& _runtime;
    ImportExportCallbacks _callbacks;
    ThemeCoordinator& _themeController;

    std::optional<ThemeRegistrationToken> _optLibraryTaskThemeToken;
    rt::Subscription _libraryTaskProgressSub;
    rt::Subscription _libraryTaskCompletedSub;
    async::LifetimeScope _tasks;
    std::unique_ptr<LibraryTaskProgressDialog> _libraryTaskDialogPtr;
  };
} // namespace ao::gtk::portal
