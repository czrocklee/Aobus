// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/ThemeCoordinator.h"
#include "portal/ImportExportCallbacks.h"
#include <ao/Error.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Task.h>
#include <ao/library/LibraryScanner.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/library/LibraryYamlExporter.h>

#include <gtkmm/window.h>

#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::gtk::portal
{
  class LibraryTaskProgressDialog;

  /**
   * LibraryImportExportWorkflow owns the background scan/import/export operations and their UI presentation:
   * the progress dialog lifecycle, progress subscriptions, result notifications, and internal-error reporting.
   *
   * It does not own any file/folder chooser dialogs; callers resolve a concrete path (and export mode) first and
   * then drive the workflow. All public entry points start on the callback executor and are lifetime-bound, so the
   * workflow may be destroyed while operations are in flight.
   */
  class LibraryImportExportWorkflow final
  {
  public:
    LibraryImportExportWorkflow(Gtk::Window& parent,
                                rt::AppRuntime& runtime,
                                ImportExportCallbacks const& callbacks,
                                ThemeCoordinator& themeController);
    ~LibraryImportExportWorkflow();

    LibraryImportExportWorkflow(LibraryImportExportWorkflow const&) = delete;
    LibraryImportExportWorkflow& operator=(LibraryImportExportWorkflow const&) = delete;
    LibraryImportExportWorkflow(LibraryImportExportWorkflow&&) = delete;
    LibraryImportExportWorkflow& operator=(LibraryImportExportWorkflow&&) = delete;

    void scan();
    void importFrom(std::filesystem::path path);
    void exportTo(std::filesystem::path path, rt::ExportMode mode);

  private:
    async::Task<void> scanWorkflow();
    async::Task<void> importWorkflow(ImportExportCallbacks callbacks, std::filesystem::path importPath);
    async::Task<void> exportWorkflow(std::filesystem::path exportPath, rt::ExportMode mode);

    // Scan-library pipeline split into coroutine + sync helpers so that scanWorkflow() stays a flat orchestrator.
    async::Task<std::optional<library::ScanPlan>> buildScanPlanOrReportFailure();
    async::Task<void> applyScanPlanWithProgress(library::ScanPlan plan);
    // Returns true when the plan has no New/Changed/Missing items; in that case the appropriate
    // notification is posted and the caller should return.
    bool reportIfNoActionableWork(library::ScanPlan const& plan);

    // Presents a Result error: structured log of the error plus an error-severity notification.
    void presentFailure(std::string_view action, std::string const& notificationMessage, Error const& error);
    // Presents an internal (exception) failure as a critical log plus an error-severity notification.
    void reportInternalFailure(std::string_view action,
                               std::string_view notificationMessage,
                               std::exception_ptr exceptionPtr);

    Gtk::Window& _parent;
    rt::AppRuntime& _runtime;
    ImportExportCallbacks const& _callbacks;
    ThemeCoordinator& _themeController;

    std::optional<ThemeRegistrationToken> _optLibraryTaskThemeToken;
    rt::Subscription _libraryTaskProgressSub;
    async::LifetimeScope _tasks;
    std::unique_ptr<LibraryTaskProgressDialog> _libraryTaskDialogPtr;
  };
} // namespace ao::gtk::portal
