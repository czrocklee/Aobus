// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "portal/LibraryImportExportWorkflow.h"

#include "common/UiWorkflow.h"
#include "portal/ImportExportCallbacks.h"
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryTaskService.h>
#include <ao/rt/library/LibraryYamlImporter.h>
#include <ao/rt/library/ScanPlan.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk::portal
{
  namespace
  {
    constexpr auto kScanExceptionContext = std::string_view{"library scan workflow"};
    constexpr auto kImportExceptionContext = std::string_view{"library import workflow"};
    constexpr auto kExportExceptionContext = std::string_view{"library export workflow"};
    constexpr auto kAudioIdentityExceptionContext = std::string_view{"audio identity workflow"};

    void logStructuredError(std::string_view action, Error const& error)
    {
      APP_LOG_ERROR("{}: code={}, message={}, location={}:{}",
                    action,
                    static_cast<int>(error.code),
                    error.message,
                    error.location.file_name(),
                    error.location.line());
    }

    std::string relinkedScanMessage(std::size_t relinkedCount)
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
      if (!result.relinkedIds.empty() && result.missingCount > 0)
      {
        return std::format(
          "{}; {}", relinkedScanMessage(result.relinkedIds.size()), missingScanMessage(result.missingCount));
      }

      if (!result.relinkedIds.empty())
      {
        return relinkedScanMessage(result.relinkedIds.size());
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
      kScanExceptionContext,
      [mode](LibraryImportExportWorkflow* self, std::stop_token const stopToken)
      { return self->scanWorkflow(mode, stopToken); },
      [](LibraryImportExportWorkflow* self) { self->presentInternalFailure("Scan failed: Internal error"); });
  }

  void LibraryImportExportWorkflow::importFrom(std::filesystem::path path)
  {
    auto callbacks = _callbacks;
    spawnUiWorkflow(
      _runtime.async(),
      _tasks,
      *this,
      kImportExceptionContext,
      [callbacks = std::move(callbacks), importPath = std::move(path)](
        LibraryImportExportWorkflow* self, std::stop_token const stopToken) mutable
      { return self->importWorkflow(std::move(callbacks), std::move(importPath), stopToken); },
      [](LibraryImportExportWorkflow* self) { self->presentInternalFailure("Import failed: Internal error"); });
  }

  void LibraryImportExportWorkflow::exportTo(std::filesystem::path path, rt::ExportMode mode)
  {
    spawnUiWorkflow(
      _runtime.async(),
      _tasks,
      *this,
      kExportExceptionContext,
      [exportPath = std::move(path), mode](LibraryImportExportWorkflow* self, std::stop_token const stopToken) mutable
      { return self->exportWorkflow(std::move(exportPath), mode, stopToken); },
      [](LibraryImportExportWorkflow* self) { self->presentInternalFailure("Export failed: Internal error"); });
  }

  async::Task<void> LibraryImportExportWorkflow::scanWorkflow(ScanRequestMode mode, std::stop_token const stopToken)
  {
    auto optPlan = co_await buildScanPlanOrReportFailure(stopToken);

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

    co_await applyScanPlanWithProgress(std::move(*optPlan), mode, stopToken);
  }

  async::Task<void> LibraryImportExportWorkflow::backfillAudioIdentityWorkflow(std::stop_token const stopToken)
  {
    auto result = co_await _runtime.library().taskService().backfillAudioIdentityAsync(stopToken);

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
      _runtime.notifications().post(rt::NotificationSeverity::Warning, "Audio identity indexing completed with errors");
    }
    else if (result->completedCount > 0)
    {
      _runtime.notifications().post(rt::NotificationSeverity::Info, "Audio identity indexing complete");
    }
  }

  async::Task<void> LibraryImportExportWorkflow::exportWorkflow(std::filesystem::path exportPath,
                                                                rt::ExportMode mode,
                                                                std::stop_token const stopToken)
  {
    auto result = co_await _runtime.library().taskService().exportLibraryAsync(std::move(exportPath), mode, stopToken);

    if (!result)
    {
      presentFailure("Export failed", std::format("Export failed: {}", result.error().message), result.error());
      co_return;
    }

    _runtime.notifications().post(rt::NotificationSeverity::Info, "Library exported successfully");
  }

  async::Task<void> LibraryImportExportWorkflow::importWorkflow(ImportExportCallbacks callbacks,
                                                                std::filesystem::path importPath,
                                                                std::stop_token const stopToken)
  {
    auto result = co_await _runtime.library().taskService().importLibraryAsync(
      std::move(importPath), rt::ImportMode::Restore, stopToken);

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

  async::Task<std::optional<rt::ScanPlan>> LibraryImportExportWorkflow::buildScanPlanOrReportFailure(
    std::stop_token const stopToken)
  {
    auto result = co_await _runtime.library().taskService().buildScanPlanAsync(stopToken);

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

    for (auto const& item : plan.items())
    {
      if (item.classification == rt::ScanClassification::Error)
      {
        APP_LOG_ERROR("Failed to scan {}: {}", item.uri, item.errorMessage);
      }
    }

    _runtime.notifications().post(rt::NotificationSeverity::Error, "Scan failed");
    return true;
  }

  async::Task<void> LibraryImportExportWorkflow::applyScanPlanWithProgress(rt::ScanPlan plan,
                                                                           ScanRequestMode mode,
                                                                           std::stop_token const stopToken)
  {
    auto options = rt::ScanApplyOptions{};

    if (mode == ScanRequestMode::FastBootstrap)
    {
      options.audioIdentityPolicy = rt::AudioIdentityPolicy::DeferNew;
    }

    auto result = co_await _runtime.library().taskService().applyScanPlanAsync(std::move(plan), options, stopToken);

    if (!result)
    {
      presentFailure("Scan apply failed", "Scan failed", result.error());
    }
    else
    {
      if ((!result->insertedIds.empty() || !result->mutatedIds.empty() || !result->relinkedIds.empty()) &&
          _callbacks.onLibraryDataMutated)
      {
        _callbacks.onLibraryDataMutated();
      }

      if (result->failureCount > 0)
      {
        auto message = std::string{"Scan completed with errors"};

        if (result->missingCount > 0 || !result->relinkedIds.empty())
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

      if (mode == ScanRequestMode::FastBootstrap)
      {
        startAudioIdentityIndexing();
      }
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
      kAudioIdentityExceptionContext,
      [](LibraryImportExportWorkflow* self, std::stop_token const stopToken)
      { return self->backfillAudioIdentityWorkflow(stopToken); },
      [](LibraryImportExportWorkflow* self)
      { self->presentInternalFailure("Audio identity indexing failed: Internal error"); });
  }

  void LibraryImportExportWorkflow::presentFailure(std::string_view action,
                                                   std::string const& notificationMessage,
                                                   Error const& error)
  {
    logStructuredError(action, error);
    _runtime.notifications().post(rt::NotificationSeverity::Error, notificationMessage);
  }

  void LibraryImportExportWorkflow::presentInternalFailure(std::string_view notificationMessage)
  {
    _runtime.notifications().post(rt::NotificationSeverity::Error, std::string{notificationMessage});
  }
} // namespace ao::gtk::portal
