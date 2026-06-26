// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "portal/LibraryImportExportWorkflow.h"

#include "app/ThemeCoordinator.h"
#include "common/UiWorkflow.h"
#include "portal/ImportExportCallbacks.h"
#include "portal/LibraryTaskProgressDialog.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/library/LibraryScanner.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryTasks.h>

#include <gtkmm/window.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk::portal
{
  namespace
  {
    void logStructuredError(std::string_view action, Error const& error)
    {
      APP_LOG_ERROR("{}: code={}, message={}, location={}:{}",
                    action,
                    static_cast<int>(error.code),
                    error.message,
                    error.location.file_name(),
                    error.location.line());
    }
  } // namespace

  LibraryImportExportWorkflow::LibraryImportExportWorkflow(Gtk::Window& parent,
                                                           rt::AppRuntime& runtime,
                                                           ImportExportCallbacks const& callbacks,
                                                           ThemeCoordinator& themeController)
    : _parent{parent}, _runtime{runtime}, _callbacks{callbacks}, _themeController{themeController}
  {
  }

  LibraryImportExportWorkflow::~LibraryImportExportWorkflow()
  {
    // Stop in-flight operations first: their coroutine frames hold a raw pointer to the progress dialog.
    _tasks.cancelAll();

    // Tear down everything that references the progress dialog before the dialog itself is destroyed by member
    // teardown. The theme token's destructor calls back into the dialog window (remove_css_class), and the progress
    // subscription's callback captures the dialog by raw pointer; both must die first regardless of declaration order.
    _libraryTaskProgressSub.reset();
    _optLibraryTaskThemeToken.reset();
  }

  void LibraryImportExportWorkflow::scan()
  {
    APP_LOG_INFO("Starting library scan...");

    spawnUiWorkflow(
      _runtime.async(),
      _tasks,
      *this,
      [](LibraryImportExportWorkflow* self) { return self->scanWorkflow(); },
      [](LibraryImportExportWorkflow* self, std::exception_ptr exceptionPtr)
      { self->reportInternalFailure("Scan failed", "Scan failed: Internal error", exceptionPtr); });
  }

  void LibraryImportExportWorkflow::importFrom(std::filesystem::path path)
  {
    auto callbacks = _callbacks;
    spawnUiWorkflow(
      _runtime.async(),
      _tasks,
      *this,
      [callbacks = std::move(callbacks), importPath = std::move(path)](LibraryImportExportWorkflow* self) mutable
      { return self->importWorkflow(std::move(callbacks), std::move(importPath)); },
      [](LibraryImportExportWorkflow* self, std::exception_ptr exceptionPtr)
      { self->reportInternalFailure("Import failed", "Import failed: Internal error", exceptionPtr); });
  }

  void LibraryImportExportWorkflow::exportTo(std::filesystem::path path, rt::ExportMode mode)
  {
    spawnUiWorkflow(
      _runtime.async(),
      _tasks,
      *this,
      [exportPath = std::move(path), mode](LibraryImportExportWorkflow* self) mutable
      { return self->exportWorkflow(std::move(exportPath), mode); },
      [](LibraryImportExportWorkflow* self, std::exception_ptr exceptionPtr)
      { self->reportInternalFailure("Export failed", "Export failed: Internal error", exceptionPtr); });
  }

  async::Task<void> LibraryImportExportWorkflow::scanWorkflow()
  {
    auto optPlan = co_await buildScanPlanOrReportFailure();

    if (!optPlan)
    {
      co_return;
    }

    if (reportIfNoActionableWork(*optPlan))
    {
      co_return;
    }

    APP_LOG_INFO("Scan plan: {} new, {} changed, {} missing, {} errors",
                 optPlan->count(library::ScanClassification::New),
                 optPlan->count(library::ScanClassification::Changed),
                 optPlan->count(library::ScanClassification::Missing),
                 optPlan->count(library::ScanClassification::Error));

    co_await applyScanPlanWithProgress(std::move(*optPlan));
  }

  async::Task<void> LibraryImportExportWorkflow::exportWorkflow(std::filesystem::path exportPath, rt::ExportMode mode)
  {
    auto result = co_await _runtime.library().tasks().exportLibraryAsync(std::move(exportPath), mode);

    if (!result)
    {
      presentFailure("Export failed", std::format("Export failed: {}", result.error().message), result.error());
      co_return;
    }

    _runtime.notifications().post(rt::NotificationSeverity::Info, "Library exported successfully");
  }

  async::Task<void> LibraryImportExportWorkflow::importWorkflow(ImportExportCallbacks callbacks,
                                                                std::filesystem::path importPath)
  {
    auto result = co_await _runtime.library().tasks().importLibraryAsync(std::move(importPath));

    if (!result)
    {
      presentFailure("Import failed", std::format("Import failed: {}", result.error().message), result.error());
      co_return;
    }

    if (callbacks.onLibraryDataMutated)
    {
      callbacks.onLibraryDataMutated();
    }

    _runtime.notifications().post(rt::NotificationSeverity::Info, "Library imported successfully");
  }

  async::Task<std::optional<library::ScanPlan>> LibraryImportExportWorkflow::buildScanPlanOrReportFailure()
  {
    auto result = co_await _runtime.library().tasks().buildScanPlanAsync();

    if (!result)
    {
      presentFailure("Scan failed", "Scan failed", result.error());
      co_return std::nullopt;
    }

    co_return std::move(*result);
  }

  bool LibraryImportExportWorkflow::reportIfNoActionableWork(library::ScanPlan const& plan)
  {
    if (plan.count(library::ScanClassification::New) != 0 || plan.count(library::ScanClassification::Changed) != 0 ||
        plan.count(library::ScanClassification::Missing) != 0)
    {
      return false;
    }

    if (plan.count(library::ScanClassification::Error) == 0)
    {
      _runtime.notifications().post(rt::NotificationSeverity::Info, "Library is up to date");
      return true;
    }

    for (auto const& item : plan.items)
    {
      if (item.classification == library::ScanClassification::Error)
      {
        APP_LOG_ERROR("Failed to scan {}: {}", item.uri, item.errorMessage);
      }
    }

    _runtime.notifications().post(rt::NotificationSeverity::Error, "Scan failed");
    return true;
  }

  async::Task<void> LibraryImportExportWorkflow::applyScanPlanWithProgress(library::ScanPlan plan)
  {
    if (_libraryTaskDialogPtr == nullptr)
    {
      _libraryTaskDialogPtr =
        std::make_unique<LibraryTaskProgressDialog>(static_cast<std::int32_t>(plan.items.size()), _parent);
      _optLibraryTaskThemeToken = _themeController.registerToplevel(*_libraryTaskDialogPtr);

      auto* const dialog = _libraryTaskDialogPtr.get();
      _libraryTaskDialogPtr->signal_response().connect([dialog](std::int32_t /*responseId*/) { dialog->close(); });
    }

    auto* const dialog = _libraryTaskDialogPtr.get();

    _libraryTaskProgressSub = _runtime.library().changes().onLibraryTaskProgress(
      [dialog](auto const& ev) { dialog->updateProgress(ev.message, ev.fraction); });

    // Reset any terminal state left by a previous scan before re-showing the reused dialog.
    dialog->beginTask();
    _libraryTaskDialogPtr->show();

    try
    {
      auto result = co_await _runtime.library().tasks().applyScanPlanAsync(std::move(plan));

      if (!result)
      {
        presentFailure("Scan apply failed", "Scan failed", result.error());
        dialog->failed("Scan failed.");
      }
      else
      {
        dialog->ready();

        if (result->cancelled)
        {
          _runtime.notifications().post(rt::NotificationSeverity::Info, "Scan cancelled");
        }
        else if (result->failureCount > 0)
        {
          _runtime.notifications().post(rt::NotificationSeverity::Warning, "Scan completed with errors");
        }
        else
        {
          _runtime.notifications().post(rt::NotificationSeverity::Info, "Library scan complete");
        }
      }
    }
    catch (...)
    {
      async::rethrowIfOperationCancelled();

      dialog->failed("Scan failed: Internal error.");
      reportInternalFailure("Scan failed", "Scan failed: Internal error", std::current_exception());
    }

    _libraryTaskProgressSub.reset();
  }

  void LibraryImportExportWorkflow::presentFailure(std::string_view action,
                                                   std::string const& notificationMessage,
                                                   Error const& error)
  {
    logStructuredError(action, error);
    _runtime.notifications().post(rt::NotificationSeverity::Error, notificationMessage);
  }

  void LibraryImportExportWorkflow::reportInternalFailure(std::string_view action,
                                                          std::string_view notificationMessage,
                                                          std::exception_ptr exceptionPtr)
  {
    try
    {
      std::rethrow_exception(exceptionPtr);
    }
    catch (ao::Exception const& e)
    {
      APP_LOG_CRITICAL("{} (internal error): {} (at {}:{})", action, e.what(), e.file(), e.line());
      _runtime.notifications().post(rt::NotificationSeverity::Error, std::string{notificationMessage});
    }
    catch (std::exception const& e)
    {
      // Defensive: current callers filter cancellation before presentation, but this helper must never turn
      // cancellation into an internal-error notification if it is reused directly.
      async::rethrowIfOperationCancelled(e);

      APP_LOG_CRITICAL("{} (internal error): {}", action, e.what());
      _runtime.notifications().post(rt::NotificationSeverity::Error, std::string{notificationMessage});
    }
    catch (...)
    {
      // See the std::exception branch above: cancellation stays control-flow, not presentation.
      async::rethrowIfOperationCancelled();

      APP_LOG_CRITICAL("{} (internal error): unknown exception", action);
      _runtime.notifications().post(rt::NotificationSeverity::Error, std::string{notificationMessage});
    }
  }
} // namespace ao::gtk::portal
