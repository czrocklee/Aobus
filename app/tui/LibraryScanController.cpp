// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LibraryScanController.h"

#include <ao/Contract.h>
#include <ao/async/Executor.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/uimodel/library/task/LibraryScanOutcome.h>
#include <ao/uimodel/status/activity/ActivityPresentationText.h>

#include <exception>
#include <memory>
#include <optional>
#include <source_location>
#include <stop_token>
#include <string>
#include <utility>

namespace ao::tui
{
  struct LibraryScanController::State final
  {
    State(async::Runtime& runtimeIn,
          rt::NotificationService& notificationsIn,
          i18n::MessageCatalog const& catalogIn,
          ScanTask scanIn)
      : runtime{runtimeIn}, notifications{notificationsIn}, catalog{catalogIn}, scan{std::move(scanIn)}
    {
    }

    void expectCallbackExecutor(std::source_location const location = std::source_location::current()) const
    {
      AO_EXPECTS_AT(location,
                    runtime.callbackExecutor().isCurrent(),
                    "TUI library scan bookkeeping must run on the callback executor");
    }

    void postTransient(i18n::MessageId const id)
    {
      notifications.post(rt::NotificationSeverity::Info,
                         std::string{i18n::requiredText(catalog, id)},
                         rt::NotificationLifetime::transient());
    }

    void presentOutcome(uimodel::LibraryScanOutcome const& outcome)
    {
      notifications.post(uimodel::libraryScanSeverity(outcome.verdict),
                         uimodel::formatLibraryScanMessage(catalog, outcome),
                         uimodel::libraryScanLifetime(outcome.verdict));
    }

    void complete(bool const cancelled,
                  std::optional<uimodel::LibraryScanOutcome> optOutcome,
                  std::exception_ptr unexpected)
    {
      expectCallbackExecutor();

      if (phase == Phase::Retired)
      {
        if (unexpected)
        {
          AO_FATAL_EXCEPTION(std::move(unexpected), "TUI library scan");
        }

        return;
      }

      if (unexpected)
      {
        AO_FATAL_EXCEPTION(std::move(unexpected), "TUI library scan");
      }

      if (cancelled)
      {
        phase = Phase::Idle;
        return;
      }

      if (optOutcome)
      {
        presentOutcome(*optOutcome);
      }

      phase = Phase::Idle;
    }

    async::Runtime& runtime;
    rt::NotificationService& notifications;
    i18n::MessageCatalog const& catalog;
    ScanTask scan;
    Phase phase = Phase::Idle;
    std::stop_source stopSource{};
  };

  LibraryScanController::LibraryScanController(async::Runtime& runtime,
                                               rt::NotificationService& notifications,
                                               i18n::MessageCatalog const& catalog,
                                               ScanTask scan)
    : _statePtr{std::make_shared<State>(runtime, notifications, catalog, std::move(scan))}
  {
  }

  LibraryScanController::LibraryScanController(async::Runtime& runtime,
                                               rt::LibraryJobs& jobs,
                                               rt::NotificationService& notifications,
                                               i18n::MessageCatalog const& catalog)
    : LibraryScanController{runtime,
                            notifications,
                            catalog,
                            [&jobs](std::stop_token const stopToken)
                            { return uimodel::runLibraryScan(&jobs, uimodel::LibraryScanMode::Eager, stopToken); }}
  {
  }

  LibraryScanController::~LibraryScanController()
  {
    retire();
  }

  void LibraryScanController::start()
  {
    _statePtr->expectCallbackExecutor();

    switch (_statePtr->phase)
    {
      case Phase::Idle:
        _statePtr->phase = Phase::Running;
        _statePtr->stopSource = std::stop_source{};
        _statePtr->runtime.spawnLogged(runScan(_statePtr, _statePtr->stopSource.get_token()), "TUI library scan");
        return;
      case Phase::Running: _statePtr->postTransient(i18n::MessageId::TuiLibraryScanAlreadyRunning); return;
      case Phase::Cancelling: _statePtr->postTransient(i18n::MessageId::TuiLibraryScanCancelling); return;
      case Phase::Retired: return;
    }
  }

  void LibraryScanController::cancel()
  {
    _statePtr->expectCallbackExecutor();

    if (_statePtr->phase != Phase::Running)
    {
      return;
    }

    _statePtr->phase = Phase::Cancelling;
    _statePtr->stopSource.request_stop();
  }

  void LibraryScanController::retire()
  {
    _statePtr->expectCallbackExecutor();

    _statePtr->phase = Phase::Retired;
    _statePtr->stopSource.request_stop();
  }

  LibraryScanController::Phase LibraryScanController::phase() const noexcept
  {
    return _statePtr->phase;
  }

  async::Task<void> LibraryScanController::runScan(std::shared_ptr<State> const statePtr,
                                                   std::stop_token const stopToken)
  {
    auto optOutcome = std::optional<uimodel::LibraryScanOutcome>{};
    auto unexpected = std::exception_ptr{};
    bool cancelled = false;

    try
    {
      optOutcome = co_await statePtr->scan(stopToken);
    }
    catch (std::exception const& error)
    {
      if (async::isOperationCancelled(error))
      {
        cancelled = true;
      }
      else
      {
        unexpected = std::current_exception();
      }
    }
    catch (...)
    {
      unexpected = std::current_exception();
    }

    co_await statePtr->runtime.resumeOnCallbackExecutor();
    statePtr->complete(cancelled, std::move(optOutcome), unexpected);
  }
} // namespace ao::tui
