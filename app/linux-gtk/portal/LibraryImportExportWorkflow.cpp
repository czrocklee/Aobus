// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "portal/LibraryImportExportWorkflow.h"

#include "common/UiWorkflow.h"
#include "portal/ImportExportCallbacks.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryTasks.h>
#include <ao/rt/library/ScanPlan.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
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

    std::string relinkedScanMessage(std::int32_t relinkedCount)
    {
      return std::format("Relinked {} moved file{}", relinkedCount, relinkedCount == 1 ? "" : "s");
    }

    std::string missingScanMessage(std::int32_t missingCount)
    {
      return std::format(
        "{} missing file{} need{} review", missingCount, missingCount == 1 ? "" : "s", missingCount == 1 ? "s" : "");
    }

    std::string scanCompletionSummary(rt::ScanApplyResult const& result)
    {
      if (result.relinkedCount > 0 && result.missingCount > 0)
      {
        return std::format(
          "{}; {}", relinkedScanMessage(result.relinkedCount), missingScanMessage(result.missingCount));
      }

      if (result.relinkedCount > 0)
      {
        return relinkedScanMessage(result.relinkedCount);
      }

      if (result.missingCount > 0)
      {
        return missingScanMessage(result.missingCount);
      }

      return "Library scan complete";
    }
  } // namespace

  LibraryImportExportWorkflow::LibraryImportExportWorkflow(rt::AppRuntime& runtime,
                                                           ImportExportCallbacks const& callbacks)
    : _runtime{runtime}, _callbacks{callbacks}
  {
  }

  LibraryImportExportWorkflow::~LibraryImportExportWorkflow()
  {
    _tasks.cancelAll();
  }

  void LibraryImportExportWorkflow::scan(ScanRequestMode mode)
  {
    APP_LOG_INFO("Starting library scan...");

    spawnUiWorkflow(
      _runtime.async(),
      _tasks,
      *this,
      [mode](LibraryImportExportWorkflow* self) { return self->scanWorkflow(mode); },
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

  async::Task<void> LibraryImportExportWorkflow::scanWorkflow(ScanRequestMode mode)
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

    APP_LOG_INFO("Scan plan: {} new, {} changed, {} moved, {} missing, {} errors",
                 optPlan->count(rt::ScanClassification::New),
                 optPlan->count(rt::ScanClassification::Changed),
                 optPlan->count(rt::ScanClassification::Moved),
                 optPlan->count(rt::ScanClassification::Missing),
                 optPlan->count(rt::ScanClassification::Error));

    co_await applyScanPlanWithProgress(std::move(*optPlan), mode);
  }

  async::Task<void> LibraryImportExportWorkflow::backfillAudioIdentityWorkflow()
  {
    try
    {
      auto result = co_await _runtime.library().tasks().backfillAudioIdentityAsync();

      if (!result)
      {
        logStructuredError("Audio identity indexing failed", result.error());
        _runtime.notifications().post(rt::NotificationSeverity::Warning, "Audio identity indexing failed");
        co_return;
      }

      if (result->cancelled)
      {
        co_return;
      }

      if (result->failureCount > 0)
      {
        _runtime.notifications().post(
          rt::NotificationSeverity::Warning, "Audio identity indexing completed with errors");
      }
      else if (result->completedCount > 0)
      {
        _runtime.notifications().post(rt::NotificationSeverity::Info, "Audio identity indexing complete");
      }
    }
    catch (...)
    {
      async::rethrowIfOperationCancelled();

      reportInternalFailure(
        "Audio identity indexing failed", "Audio identity indexing failed: Internal error", std::current_exception());
    }
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

  async::Task<std::optional<rt::ScanPlan>> LibraryImportExportWorkflow::buildScanPlanOrReportFailure()
  {
    auto result = co_await _runtime.library().tasks().buildScanPlanAsync();

    if (!result)
    {
      presentFailure("Scan failed", "Scan failed", result.error());
      co_return std::nullopt;
    }

    co_return std::move(*result);
  }

  bool LibraryImportExportWorkflow::reportIfNoActionableWork(rt::ScanPlan const& plan)
  {
    if (plan.count(rt::ScanClassification::New) != 0 || plan.count(rt::ScanClassification::Changed) != 0 ||
        plan.count(rt::ScanClassification::Moved) != 0 || plan.count(rt::ScanClassification::Missing) != 0)
    {
      return false;
    }

    if (plan.count(rt::ScanClassification::Error) == 0)
    {
      _runtime.notifications().post(rt::NotificationSeverity::Info, "Library is up to date");
      return true;
    }

    for (auto const& item : plan.items)
    {
      if (item.classification == rt::ScanClassification::Error)
      {
        APP_LOG_ERROR("Failed to scan {}: {}", item.uri, item.errorMessage);
      }
    }

    _runtime.notifications().post(rt::NotificationSeverity::Error, "Scan failed");
    return true;
  }

  async::Task<void> LibraryImportExportWorkflow::applyScanPlanWithProgress(rt::ScanPlan plan, ScanRequestMode mode)
  {
    try
    {
      auto options = rt::ScanApplyOptions{};

      if (mode == ScanRequestMode::FastBootstrap)
      {
        options.audioIdentityPolicy = rt::AudioIdentityPolicy::DeferNew;
      }

      auto result = co_await _runtime.library().tasks().applyScanPlanAsync(std::move(plan), options);

      if (!result)
      {
        presentFailure("Scan apply failed", "Scan failed", result.error());
      }
      else
      {
        if (!result->cancelled && !result->processedIds.empty() && _callbacks.onLibraryDataMutated)
        {
          _callbacks.onLibraryDataMutated();
        }

        if (result->cancelled)
        {
          _runtime.notifications().post(rt::NotificationSeverity::Info, "Scan cancelled");
        }
        else if (result->failureCount > 0)
        {
          auto message = std::string{"Scan completed with errors"};

          if (result->missingCount > 0 || result->relinkedCount > 0)
          {
            message += std::format("; {}", scanCompletionSummary(*result));
          }

          _runtime.notifications().post(rt::NotificationSeverity::Warning, std::move(message));
        }
        else if (result->missingCount > 0)
        {
          _runtime.notifications().post(rt::NotificationSeverity::Warning, scanCompletionSummary(*result));
        }
        else
        {
          _runtime.notifications().post(rt::NotificationSeverity::Info, scanCompletionSummary(*result));
        }

        if (mode == ScanRequestMode::FastBootstrap && !result->cancelled)
        {
          startAudioIdentityIndexing();
        }
      }
    }
    catch (...)
    {
      async::rethrowIfOperationCancelled();

      reportInternalFailure("Scan failed", "Scan failed: Internal error", std::current_exception());
    }
  }

  void LibraryImportExportWorkflow::startAudioIdentityIndexing()
  {
    _runtime.notifications().post(
      rt::NotificationSeverity::Info, "Library ready; indexing audio identity in background");

    spawnUiWorkflow(
      _runtime.async(),
      _tasks,
      *this,
      [](LibraryImportExportWorkflow* self) { return self->backfillAudioIdentityWorkflow(); },
      [](LibraryImportExportWorkflow* self, std::exception_ptr exceptionPtr)
      {
        self->reportInternalFailure(
          "Audio identity indexing failed", "Audio identity indexing failed: Internal error", exceptionPtr);
      });
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
