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
#include <ao/rt/ListMutation.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/uimodel/library/list/ListOrderSession.h>

#include <cstddef>
#include <exception>
#include <memory>
#include <source_location>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    std::size_t affectedTrackCount(rt::MoveListOrderReply const& reply)
    {
      return reply.selectedTrackIds.size();
    }

    std::size_t affectedTrackCount(rt::ResetListOrderReply const& reply)
    {
      return reply.forgottenPositionCount;
    }
  } // namespace

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
                            std::exception_ptr unexpectedPtr,
                            i18n::MessageId const appliedMessageId)
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

      switch (res->status)
      {
        case rt::AuthoringStatus::Applied:
          post(rt::NotificationSeverity::Info,
               i18n::requiredFormat(textCatalog, appliedMessageId, {{"count", affectedTrackCount(res->reply)}}));
          return;
        case rt::AuthoringStatus::NoOp:
          post(rt::NotificationSeverity::Info,
               std::string{i18n::requiredText(textCatalog, i18n::MessageId::ListOrderUnchanged)});
          return;
        // A busy library, a moved revision, and a view that cannot be ordered
        // are moments to retry, not faults, so they pass like the GTK status
        // line instead of pinning a warning the next successful move would
        // leave standing.
        case rt::AuthoringStatus::Busy:
          post(rt::NotificationSeverity::Info,
               std::string{i18n::requiredText(textCatalog, i18n::MessageId::ListOrderLibraryBusy)});
          return;
        case rt::AuthoringStatus::Stale:
          post(rt::NotificationSeverity::Info,
               std::string{i18n::requiredText(textCatalog, i18n::MessageId::ListOrderChanged)});
          return;
        case rt::AuthoringStatus::Unavailable:
          post(rt::NotificationSeverity::Info,
               std::string{i18n::requiredText(textCatalog, i18n::MessageId::ListOrderEditingUnavailable)});
          return;
      }
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
                                                async::Task<Result<rt::AuthoringResult<Reply>>> submission,
                                                i18n::MessageId const appliedMessageId)
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
      statePtr->completeSubmission(cancelled, std::move(res), std::move(unexpectedPtr), appliedMessageId);
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

    // Each submission task is constructed before the in-flight gate is armed
    // and its coroutine spawned, so a throwing construction leaves the gate
    // open for the next command instead of dropping every later key.
    switch (command)
    {
      case ListOrderCommand::MoveUp:
      {
        auto submission = session.moveUpAsync(std::move(selectedTrackIds));
        state.submissionPending = true;
        state.runtime.spawnLogged(
          State::runSubmissionAsync(_statePtr, std::move(submission), i18n::MessageId::ListOrderMoved),
          "TUI list order command");
        return;
      }
      case ListOrderCommand::MoveDown:
      {
        auto submission = session.moveDownAsync(std::move(selectedTrackIds));
        state.submissionPending = true;
        state.runtime.spawnLogged(
          State::runSubmissionAsync(_statePtr, std::move(submission), i18n::MessageId::ListOrderMoved),
          "TUI list order command");
        return;
      }
      case ListOrderCommand::MoveToTop:
      {
        auto submission = session.moveToTopAsync(std::move(selectedTrackIds));
        state.submissionPending = true;
        state.runtime.spawnLogged(
          State::runSubmissionAsync(_statePtr, std::move(submission), i18n::MessageId::ListOrderMoved),
          "TUI list order command");
        return;
      }
      case ListOrderCommand::MoveToBottom:
      {
        auto submission = session.moveToBottomAsync(std::move(selectedTrackIds));
        state.submissionPending = true;
        state.runtime.spawnLogged(
          State::runSubmissionAsync(_statePtr, std::move(submission), i18n::MessageId::ListOrderMoved),
          "TUI list order command");
        return;
      }
      case ListOrderCommand::Reset:
      {
        auto submission = session.resetOrderAsync();
        state.submissionPending = true;
        state.runtime.spawnLogged(
          State::runSubmissionAsync(_statePtr, std::move(submission), i18n::MessageId::ListOrderReset),
          "TUI list order command");
        return;
      }
    }
  }
} // namespace ao::tui
