// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "ListOrderController.h"

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/uimodel/library/list/ListOrderOutcome.h>
#include <ao/uimodel/library/list/ListOrderSession.h>

#include <exception>
#include <memory>
#include <source_location>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui
{
  struct ListOrderController::State final
  {
    async::Runtime& runtime;
    rt::Library& library;
    rt::ViewService& views;
    rt::NotificationService& notifications;
    i18n::MessageCatalog const& textCatalog;
    bool submissionPending = false;

    void expectCallbackExecutor(std::source_location const location = std::source_location::current()) const
    {
      AO_EXPECTS_AT(location,
                    runtime.callbackExecutor().isCurrent(),
                    "TUI list order bookkeeping must run on the callback executor");
    }

    void post(rt::NotificationSeverity const severity, std::string message)
    {
      notifications.post(rt::NotificationRequest{
        .severity = severity,
        .message = std::move(message),
        .lifetime = severity == rt::NotificationSeverity::Info ? rt::NotificationLifetime::transient()
                                                               : rt::NotificationLifetime::history(),
      });
    }

    /// Reports the terminal outcome of one submitted command, on the callback executor.
    template<typename Reply>
    void completeSubmission(bool const cancelled,
                            Result<rt::AuthoringResult<Reply>> res,
                            std::exception_ptr unexpectedPtr)
    {
      expectCallbackExecutor();

      // The gate clears before anything that can report, so a failing
      // presentation cannot leave later commands dropped against a
      // submission that already settled.
      submissionPending = false;

      if (unexpectedPtr)
      {
        AO_FATAL_EXCEPTION(std::move(unexpectedPtr), "TUI list order command");
      }

      // A cancelled command settles silently: the runtime is closing, so no
      // notification could be presented, and the next command would start a
      // fresh binding anyway.
      if (cancelled)
      {
        return;
      }

      if (!res)
      {
        post(rt::NotificationSeverity::Warning, res.error().message);
        return;
      }

      // Writer statuses, including refusals, are transient feedback rather than operational faults.
      post(rt::NotificationSeverity::Info, uimodel::formatListOrderOutcome(textCatalog, *res));
    }

    /**
     * @brief Awaits @p submission and settles it on the callback executor.
     *
     * The submission task was created from a session that was just found
     * current, so this coroutine never reads the session again and can
     * outlive both it and the controller that started the command.
     */
    template<typename Reply>
    static async::Task<void> runSubmissionAsync(std::shared_ptr<State> statePtr,
                                                async::Task<Result<rt::AuthoringResult<Reply>>> submission)
    {
      auto res = Result<rt::AuthoringResult<Reply>>{};
      auto unexpectedPtr = std::exception_ptr{};
      bool cancelled = false;

      try
      {
        // Session state shares the event thread with invalidation callbacks.
        co_await statePtr->runtime.resumeOnCallbackExecutorAsync();
        res = co_await std::move(submission);
      }
      catch (std::exception const& error)
      {
        if (async::isOperationCancelled(error))
        {
          cancelled = true;
        }
        else
        {
          unexpectedPtr = std::current_exception();
        }
      }
      catch (...)
      {
        unexpectedPtr = std::current_exception();
      }

      co_await statePtr->runtime.resumeOnCallbackExecutorAsync();
      statePtr->completeSubmission(cancelled, std::move(res), std::move(unexpectedPtr));
    }
  };

  ListOrderController::ListOrderController(async::Runtime& runtime,
                                           rt::Library& library,
                                           rt::ViewService& views,
                                           rt::NotificationService& notifications,
                                           i18n::MessageCatalog const& textCatalog)
    : _statePtr{std::make_shared<State>(State{
        .runtime = runtime,
        .library = library,
        .views = views,
        .notifications = notifications,
        .textCatalog = textCatalog,
      })}
  {
  }

  void ListOrderController::apply(ListOrderCommand const command,
                                  rt::ViewId const viewId,
                                  std::vector<TrackId> selectedTrackIds)
  {
    auto& state = *_statePtr;

    state.expectCallbackExecutor();

    // One command commits a complete List order, so an extra key or command
    // arriving while a submission is still in flight is dropped rather than
    // queued or replayed: the pending binding belongs to a revision the
    // in-flight commit is already changing, and the next command can be
    // issued again once its outcome is reported.
    if (state.submissionPending)
    {
      return;
    }

    auto sessionRes = uimodel::ListOrderAuthoringSession::begin(state.library, state.views, viewId, state.textCatalog);

    if (!sessionRes)
    {
      // The refusal names what to change, such as choosing Manual Order; it
      // is guidance for this keypress, not a fault to keep on screen.
      state.post(rt::NotificationSeverity::Info, sessionRes.error().message);
      return;
    }

    auto& session = *sessionRes;

    // The task is constructed before arming the in-flight gate, so a throwing
    // construction leaves the gate open for the next command.
    auto submit = [this, &state](auto submission)
    {
      state.submissionPending = true;
      state.runtime.spawnLogged(State::runSubmissionAsync(_statePtr, std::move(submission)), "TUI list order command");
    };

    switch (command)
    {
      case ListOrderCommand::MoveUp: submit(session.moveUpAsync(std::move(selectedTrackIds))); return;
      case ListOrderCommand::MoveDown: submit(session.moveDownAsync(std::move(selectedTrackIds))); return;
      case ListOrderCommand::MoveToTop: submit(session.moveToTopAsync(std::move(selectedTrackIds))); return;
      case ListOrderCommand::MoveToBottom: submit(session.moveToBottomAsync(std::move(selectedTrackIds))); return;
      case ListOrderCommand::Reset: submit(session.resetOrderAsync()); return;
    }
  }
} // namespace ao::tui
