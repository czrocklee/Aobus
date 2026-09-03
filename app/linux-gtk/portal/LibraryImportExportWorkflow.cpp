// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "portal/LibraryImportExportWorkflow.h"

#include "common/UiWorkflow.h"
#include "i18n/GtkText.h"
#include "portal/ImportExportCallbacks.h"
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryImportPlan.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibraryTransfer.h>
#include <ao/uimodel/library/task/LibraryScanOutcome.h>
#include <ao/uimodel/status/activity/ActivityPresentationText.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
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
  } // namespace

  LibraryImportExportWorkflow::LibraryImportExportWorkflow(async::Runtime& asyncRuntime,
                                                           rt::Library& library,
                                                           rt::NotificationService& notifications,
                                                           ImportExportCallbacks const& callbacks,
                                                           i18n::MessageCatalog textCatalog)
    : _asyncRuntime{asyncRuntime}
    , _library{library}
    , _notifications{notifications}
    , _callbacks{callbacks}
    , _textCatalog{std::move(textCatalog)}
  {
  }

  LibraryImportExportWorkflow::~LibraryImportExportWorkflow()
  {
    _presentationCallbacks.close();
    _tasks.cancelAll();
  }

  void LibraryImportExportWorkflow::scan(ScanRequestMode mode)
  {
    APP_LOG_INFO("Starting library scan...");

    spawnUiWorkflow(_asyncRuntime,
                    _tasks,
                    *this,
                    kScanExceptionContext,
                    [mode](LibraryImportExportWorkflow* self, std::stop_token const stopToken)
                    { return self->scanWorkflow(mode, stopToken); });
  }

  void LibraryImportExportWorkflow::importFrom(std::filesystem::path path)
  {
    auto callbacks = _callbacks;
    spawnUiWorkflow(_asyncRuntime,
                    _tasks,
                    *this,
                    kImportExceptionContext,
                    [callbacks = std::move(callbacks), importPath = std::move(path)](
                      LibraryImportExportWorkflow* self, std::stop_token const stopToken) mutable
                    { return self->prepareImportWorkflow(std::move(callbacks), std::move(importPath), stopToken); });
  }

  void LibraryImportExportWorkflow::exportTo(std::filesystem::path path, rt::ExportMode mode)
  {
    spawnUiWorkflow(
      _asyncRuntime,
      _tasks,
      *this,
      kExportExceptionContext,
      [exportPath = std::move(path), mode](LibraryImportExportWorkflow* self, std::stop_token const stopToken) mutable
      { return self->exportWorkflow(std::move(exportPath), mode, stopToken); });
  }

  async::Task<void> LibraryImportExportWorkflow::scanWorkflow(ScanRequestMode mode, std::stop_token const stopToken)
  {
    auto presentResult = _presentationCallbacks.guard([this](uimodel::LibraryScanOutcome outcome) mutable
                                                      { presentScanOutcome(outcome); });
    auto* const jobs = &_library.jobs();
    auto outcome = co_await uimodel::runLibraryScan(jobs, mode, stopToken);
    presentResult(std::move(outcome));
  }

  async::Task<void> LibraryImportExportWorkflow::backfillAudioIdentityWorkflow(std::stop_token const stopToken)
  {
    auto presentResult = _presentationCallbacks.guard(
      [this](std::optional<Error> optError, std::int32_t completedCount, std::int32_t failureCount)
      {
        if (optError)
        {
          logStructuredError("Audio identity indexing failed", *optError);
          _notifications.post(rt::NotificationSeverity::Warning,
                              gtkText(_textCatalog, i18n::MessageId::LibraryAudioIdentityIndexingFailed),
                              rt::NotificationLifetime::history());
          return;
        }

        if (failureCount > 0)
        {
          _notifications.post(rt::NotificationSeverity::Warning,
                              gtkText(_textCatalog, i18n::MessageId::LibraryAudioIdentityIndexingCompletedWithErrors),
                              rt::NotificationLifetime::history());
        }
        else if (completedCount > 0)
        {
          _notifications.post(rt::NotificationSeverity::Info,
                              gtkText(_textCatalog, i18n::MessageId::LibraryAudioIdentityIndexingComplete),
                              rt::NotificationLifetime::transient());
        }
      });
    auto* const jobs = &_library.jobs();
    auto result = co_await jobs->backfillAudioIdentityAsync(stopToken);

    if (!result)
    {
      presentResult(std::optional{result.error()}, 0, 0);
      co_return;
    }

    presentResult(std::nullopt, result->completedCount, result->failureCount);
  }

  async::Task<void> LibraryImportExportWorkflow::exportWorkflow(std::filesystem::path exportPath,
                                                                rt::ExportMode mode,
                                                                std::stop_token const stopToken)
  {
    auto presentResult = _presentationCallbacks.guard(
      [this](Result<> result)
      {
        if (!result)
        {
          presentFailure("Export failed",
                         i18n::requiredFormat(
                           _textCatalog, i18n::MessageId::LibraryExportFailed, {{"error", result.error().message}}),
                         result.error());
          return;
        }

        _notifications.post(rt::NotificationSeverity::Info,
                            gtkText(_textCatalog, i18n::MessageId::LibraryExported),
                            rt::NotificationLifetime::transient());
      });
    auto* const jobs = &_library.jobs();
    auto result = co_await jobs->exportLibraryAsync(std::move(exportPath), mode, stopToken);
    presentResult(std::move(result));
  }

  async::Task<void> LibraryImportExportWorkflow::prepareImportWorkflow(ImportExportCallbacks callbacks,
                                                                       std::filesystem::path importPath,
                                                                       std::stop_token const stopToken)
  {
    auto presentResult = _presentationCallbacks.guard(
      [this, callbacks = std::move(callbacks)](Result<rt::LibraryImportPlan> result) mutable
      {
        if (!result)
        {
          presentFailure("Import failed",
                         i18n::requiredFormat(
                           _textCatalog, i18n::MessageId::LibraryImportFailed, {{"error", result.error().message}}),
                         result.error());
          return;
        }

        if (!callbacks.requestLibraryRestoreConfirmation)
        {
          auto const error =
            Error{.code = Error::Code::InvalidState, .message = "Library restore confirmation is unavailable"};
          presentFailure(
            "Import failed", gtkText(_textCatalog, i18n::MessageId::LibraryImportConfirmationUnavailable), error);
          return;
        }

        auto requestConfirmation = std::move(callbacks.requestLibraryRestoreConfirmation);
        auto const report = result->report();
        auto pendingPlanPtr = std::make_shared<std::optional<rt::LibraryImportPlan>>(std::move(*result));
        requestConfirmation(report,
                            _presentationCallbacks.guard(
                              [this, pendingPlanPtr](bool const confirmed) mutable
                              {
                                if (!pendingPlanPtr->has_value())
                                {
                                  return;
                                }

                                if (!confirmed)
                                {
                                  *pendingPlanPtr = std::nullopt;
                                  return;
                                }

                                auto plan = std::move(**pendingPlanPtr);
                                *pendingPlanPtr = std::nullopt;
                                applyPreparedImport(std::move(plan));
                              }));
      });
    auto* const jobs = &_library.jobs();
    auto result = co_await jobs->prepareLibraryImportAsync(std::move(importPath), rt::ImportMode::Restore, stopToken);
    presentResult(std::move(result));
  }

  void LibraryImportExportWorkflow::applyPreparedImport(rt::LibraryImportPlan plan)
  {
    spawnUiWorkflow(_asyncRuntime,
                    _tasks,
                    *this,
                    kImportExceptionContext,
                    [plan = std::move(plan)](LibraryImportExportWorkflow* self, std::stop_token const stopToken) mutable
                    { return self->applyImportWorkflow(std::move(plan), stopToken); });
  }

  async::Task<void> LibraryImportExportWorkflow::applyImportWorkflow(rt::LibraryImportPlan plan,
                                                                     std::stop_token const stopToken)
  {
    auto presentResult = _presentationCallbacks.guard(
      [this](Result<rt::ImportReport> result)
      {
        if (!result)
        {
          presentFailure("Import failed",
                         i18n::requiredFormat(
                           _textCatalog, i18n::MessageId::LibraryImportFailed, {{"error", result.error().message}}),
                         result.error());
          return;
        }

        _notifications.post(rt::NotificationSeverity::Info,
                            gtkText(_textCatalog, i18n::MessageId::LibraryImported),
                            rt::NotificationLifetime::transient());
      });
    auto* const jobs = &_library.jobs();
    auto result = co_await jobs->applyLibraryImportPlanAsync(std::move(plan), stopToken);
    presentResult(std::move(result));
  }

  void LibraryImportExportWorkflow::presentScanOutcome(uimodel::LibraryScanOutcome const& outcome)
  {
    // The verdict, the sentence, and how long it stays reachable are all
    // decided in uimodel, which is what keeps this window and the Windows one
    // reporting the same scan the same way. Posting it is all that is left.
    _notifications.post(uimodel::libraryScanSeverity(outcome.verdict),
                        uimodel::formatLibraryScanMessage(_textCatalog, outcome),
                        uimodel::libraryScanLifetime(outcome.verdict));

    if (outcome.shouldBackfillAudioIdentity)
    {
      startAudioIdentityIndexing();
    }
  }

  void LibraryImportExportWorkflow::startAudioIdentityIndexing()
  {
    _notifications.post(rt::NotificationSeverity::Info,
                        gtkText(_textCatalog, i18n::MessageId::LibraryReadyIndexingAudioIdentity),
                        rt::NotificationLifetime::transient());

    spawnUiWorkflow(_asyncRuntime,
                    _tasks,
                    *this,
                    kAudioIdentityExceptionContext,
                    [](LibraryImportExportWorkflow* self, std::stop_token const stopToken)
                    { return self->backfillAudioIdentityWorkflow(stopToken); });
  }

  void LibraryImportExportWorkflow::presentFailure(std::string_view action,
                                                   std::string const& notificationMessage,
                                                   Error const& error)
  {
    logStructuredError(action, error);
    _notifications.post(rt::NotificationSeverity::Error, notificationMessage, rt::NotificationLifetime::history());
  }
} // namespace ao::gtk::portal
