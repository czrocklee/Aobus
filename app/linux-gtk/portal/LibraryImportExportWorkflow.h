// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "common/MainContextCallbackScope.h"
#include "portal/ImportExportCallbacks.h"
#include <ao/Error.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/library/LibraryImportPlan.h>
#include <ao/rt/library/LibraryTransfer.h>
#include <ao/uimodel/library/task/LibraryScanOutcome.h>

#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>

namespace ao::rt
{
  class Library;
  class NotificationService;
}

namespace ao::async
{
  class Runtime;
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
    LibraryImportExportWorkflow(async::Runtime& asyncRuntime,
                                rt::Library& library,
                                rt::NotificationService& notifications,
                                ImportExportCallbacks const& callbacks,
                                i18n::MessageCatalog textCatalog);
    // The callbacks are borrowed for the workflow's whole life, so a temporary
    // would leave _callbacks dangling before the first operation starts.
    LibraryImportExportWorkflow(async::Runtime&,
                                rt::Library&,
                                rt::NotificationService&,
                                ImportExportCallbacks&&,
                                i18n::MessageCatalog) = delete;
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

    async::Runtime& _asyncRuntime;
    rt::Library& _library;
    rt::NotificationService& _notifications;
    // Borrowed so callback updates made by the owning coordinator remain visible
    // to operations started after construction.
    ImportExportCallbacks const& _callbacks;
    i18n::MessageCatalog _textCatalog;

    async::LifetimeScope _tasks;
    MainContextCallbackScope _presentationCallbacks;
  };
} // namespace ao::gtk::portal
