// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "common/MainContextCallbackScope.h"
#include "portal/ImportExportCallbacks.h"
#include <ao/Error.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Task.h>
#include <ao/rt/library/LibraryImportPlan.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/uimodel/library/task/LibraryScanOutcome.h>
#include <ao/uimodel/library/task/LibraryScanWorkflow.h>

#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::gtk::portal
{
  using ScanRequestMode = uimodel::LibraryScanMode;

  /**
   * LibraryImportExportWorkflow owns the background scan/import/export operations and their UI presentation:
   * progress events, result notifications, and internal-error reporting.
   *
   * It does not own any file/folder chooser dialogs; callers resolve a concrete path (and export mode) first and
   * then drive the workflow. All public entry points start on the callback executor and are lifetime-bound, so the
   * workflow may be destroyed while operations are in flight.
   */
  class LibraryImportExportWorkflow final
  {
  public:
    LibraryImportExportWorkflow(rt::AppRuntime& runtime, ImportExportCallbacks const& callbacks);
    ~LibraryImportExportWorkflow();

    LibraryImportExportWorkflow(LibraryImportExportWorkflow const&) = delete;
    LibraryImportExportWorkflow& operator=(LibraryImportExportWorkflow const&) = delete;
    LibraryImportExportWorkflow(LibraryImportExportWorkflow&&) = delete;
    LibraryImportExportWorkflow& operator=(LibraryImportExportWorkflow&&) = delete;

    void scan(ScanRequestMode mode = ScanRequestMode::Eager);
    void importFrom(std::filesystem::path path);
    void exportTo(std::filesystem::path path, rt::ExportMode mode);

  private:
    async::Task<void> scanWorkflow(ScanRequestMode mode, std::stop_token stopToken);
    async::Task<void> backfillAudioIdentityWorkflow(std::stop_token stopToken);
    async::Task<void> prepareImportWorkflow(ImportExportCallbacks callbacks,
                                            std::filesystem::path importPath,
                                            std::stop_token stopToken);
    async::Task<void> applyImportWorkflow(rt::LibraryImportPlan plan, std::stop_token stopToken);
    void applyPreparedImport(rt::LibraryImportPlan plan);
    async::Task<void> exportWorkflow(std::filesystem::path exportPath, rt::ExportMode mode, std::stop_token stopToken);

    void startAudioIdentityIndexing();
    void presentScanOutcome(uimodel::LibraryScanOutcome const& outcome);

    // Presents a Result error: structured log of the error plus an error-severity notification.
    void presentFailure(std::string_view action, std::string const& notificationMessage, Error const& error);

    rt::AppRuntime& _runtime;
    ImportExportCallbacks const& _callbacks;

    async::LifetimeScope _tasks;
    MainContextCallbackScope _presentationCallbacks;
  };
} // namespace ao::gtk::portal
