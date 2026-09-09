// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TrackEditController.h"

#include "TrackPropertiesEditor.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/MetadataValueCompleter.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/ordering/TextOrderingPolicy.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>
#include <ao/utility/Path.h>

#include <ftxui/component/event.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    using i18n::MessageId;

    /// Why one preparation attempt installed nothing, in the terms it is reported.
    struct PreparationError final
    {
      MessageId messageId = MessageId::TuiEditorOpenUnavailable;
      /// The runtime's own wording, when it had any; appended to the message above.
      std::string detail{};
    };

    /// One coherent read: what the editor shows, and the binding it writes through.
    struct PreparedEditor final
    {
      TrackEditorPreparation preparation;
      uimodel::TrackAuthoringSession session;
    };

    std::string identityTitle(rt::TrackRow const& row)
    {
      // A file with no title tag is still worth naming, and its own file name
      // is the only identity the library has left for it.
      if (!row.title.empty())
      {
        return row.title;
      }

      return row.optUriPath ? utility::pathToUtf8(row.optUriPath->filename()) : std::string{};
    }

    std::string identityPath(rt::TrackRow const& row)
    {
      return row.optUriPath ? utility::pathToUtf8(*row.optUriPath) : std::string{};
    }

    void loadFormField(uimodel::TrackPropertiesFormModel& form,
                       rt::LibrarySnapshot const& snapshot,
                       TrackId const trackId,
                       rt::TrackField const field,
                       bool const first)
    {
      auto rawValue = snapshot.trackField(trackId, field);

      if (first)
      {
        form.loadFirstTrackField(field, std::move(rawValue));
        return;
      }

      std::ignore = form.tryMergeTrackField(field, rawValue);
    }

    /**
     * @brief Re-sorts @p tags so equal-frequency names follow the locale's collation.
     *
     * LibrarySnapshot already returns descending frequency then ascending byte
     * order, so this only replaces the tiebreak, and only where a collation
     * policy exists to replace it with.
     */
    void sortTagsByFrequency(std::vector<std::pair<std::string, std::size_t>>& tags,
                             rt::TextOrderingPolicy const& textOrderingPolicy)
    {
      struct TagWithSortKey final
      {
        std::pair<std::string, std::size_t> tag;
        std::string sortKey;
      };

      auto ordered = std::vector<TagWithSortKey>{};
      ordered.reserve(tags.size());

      for (auto& tag : tags)
      {
        auto sortKey = std::string{};
        std::ignore = textOrderingPolicy.makeSortKeyInto(sortKey, tag.first);
        ordered.push_back(TagWithSortKey{.tag = std::move(tag), .sortKey = std::move(sortKey)});
      }

      std::ranges::sort(ordered,
                        [](TagWithSortKey const& lhs, TagWithSortKey const& rhs)
                        {
                          if (lhs.tag.second != rhs.tag.second)
                          {
                            return lhs.tag.second > rhs.tag.second;
                          }

                          if (auto const cmp = lhs.sortKey.compare(rhs.sortKey); cmp != 0)
                          {
                            return cmp < 0;
                          }

                          return lhs.tag.first < rhs.tag.first;
                        });

      tags.clear();

      for (auto& item : ordered)
      {
        tags.push_back(std::move(item.tag));
      }
    }

    /// The library tags no captured track carries, most frequent first.
    std::vector<std::string> selectTagSuggestions(std::vector<std::pair<std::string, std::size_t>> allTags,
                                                  std::span<std::pair<std::string, std::size_t> const> const tagCounts,
                                                  rt::TextOrderingPolicy const* const textOrderingPolicy)
    {
      if (textOrderingPolicy != nullptr)
      {
        sortTagsByFrequency(allTags, *textOrderingPolicy);
      }

      // The selection's own tags are their own section in the editor, so the
      // suggestion list is the library vocabulary minus them.
      auto onSelection = std::unordered_set<std::string_view>{};
      onSelection.reserve(tagCounts.size());

      for (auto const& [tag, count] : tagCounts)
      {
        onSelection.insert(tag);
      }

      auto suggestions = std::vector<std::string>{};

      for (auto& [tag, count] : allTags)
      {
        if (!onSelection.contains(tag))
        {
          suggestions.push_back(std::move(tag));
        }
      }

      return suggestions;
    }

    /**
     * @brief Performs the single coherent preparation an open or a reload gets.
     *
     * The session binds first so its revision can be compared with the one
     * snapshot every value is read through. A mismatch means the library moved
     * between the two, so the values read would not describe what a write
     * would land on.
     */
    std::expected<PreparedEditor, PreparationError> prepareEditor(
      rt::Library& library,
      i18n::MessageCatalog const& textCatalog,
      std::vector<TrackId> const& targetIds,
      rt::TextOrderingPolicy const* const textOrderingPolicy)
    {
      auto sessionRes = uimodel::TrackAuthoringSession::begin(library, targetIds);

      if (!sessionRes)
      {
        // The bind is where a vanished target is actually found, so its own
        // NotFound is the incomplete-selection report rather than a generic
        // availability refusal.
        auto const missing = sessionRes.error().code == Error::Code::NotFound;
        return std::unexpected{PreparationError{
          .messageId = missing ? MessageId::TuiEditorOpenIncomplete : MessageId::TuiEditorOpenUnavailable,
          .detail = sessionRes.error().message}};
      }

      auto const snapshot = library.snapshot();

      if (snapshot.revision() != sessionRes->boundRevision())
      {
        return std::unexpected{PreparationError{.messageId = MessageId::TuiEditorOpenUnavailable}};
      }

      auto const spec = uimodel::buildTrackPropertiesFormSpec(textCatalog);
      auto baseline = uimodel::TrackPropertiesFormModel{textCatalog};

      for (auto const& row : spec.metadataRows)
      {
        baseline.addField(row.field, true);
      }

      for (auto const& row : spec.propertyRows)
      {
        baseline.addField(row.field, false);
      }

      auto targets = std::vector<TrackEditorTarget>{};
      targets.reserve(targetIds.size());

      for (auto const trackId : targetIds)
      {
        auto const optRow = snapshot.trackRow(trackId);

        if (!optRow)
        {
          // Editing whatever survived would be a different operation than the
          // one the user asked for, so nothing opens instead.
          return std::unexpected{PreparationError{.messageId = MessageId::TuiEditorOpenIncomplete}};
        }

        auto const first = targets.empty();
        targets.push_back(
          TrackEditorTarget{.id = trackId, .title = identityTitle(*optRow), .path = identityPath(*optRow)});

        for (auto const& row : spec.metadataRows)
        {
          loadFormField(baseline, snapshot, trackId, row.field, first);
        }

        for (auto const& row : spec.propertyRows)
        {
          loadFormField(baseline, snapshot, trackId, row.field, first);
        }
      }

      auto tagCounts = snapshot.selectionTagCounts(targetIds);
      auto tagSuggestions = selectTagSuggestions(snapshot.allTagsByFrequency(), tagCounts, textOrderingPolicy);

      return PreparedEditor{.preparation =
                              TrackEditorPreparation{
                                .targets = std::move(targets),
                                .baseline = std::move(baseline),
                                .tagCounts = std::move(tagCounts),
                                .tagSuggestions = std::move(tagSuggestions),
                              },
                            .session = std::move(*sessionRes)};
    }

    std::size_t countDistinctChangedTracks(rt::UpdateTrackPropertiesReply const& reply)
    {
      auto changed = std::unordered_set<TrackId>{};

      for (auto const& change : reply.metadata.changes)
      {
        changed.insert(change.trackId);
      }

      for (auto const& change : reply.tags.changes)
      {
        changed.insert(change.trackId);
      }

      return changed.size();
    }
  } // namespace

  struct TrackEditController::State final
  {
    State(async::Runtime& runtimeIn,
          rt::Library& libraryIn,
          rt::NotificationService& notificationsIn,
          i18n::MessageCatalog const& textCatalogIn,
          Outputs outputsIn,
          rt::CompletionService& completionServiceIn,
          rt::TextOrderingPolicy const* const textOrderingPolicyIn)
      : runtime{runtimeIn}
      , library{libraryIn}
      , notifications{notificationsIn}
      , textCatalog{textCatalogIn}
      , outputs{std::move(outputsIn)}
      , completionService{completionServiceIn}
      , textOrderingPolicy{textOrderingPolicyIn}
    {
    }

    void expectCallbackExecutor(std::source_location const location = std::source_location::current()) const
    {
      AO_EXPECTS_AT(location,
                    runtime.callbackExecutor().isCurrent(),
                    "TUI track editor bookkeeping must run on the callback executor");
    }

    void post(rt::NotificationSeverity const severity, std::string message)
    {
      notifications.post(severity, std::move(message), rt::NotificationLifetime::transient());
    }

    void postText(rt::NotificationSeverity const severity, MessageId const id)
    {
      post(severity, std::string{i18n::requiredText(textCatalog, id)});
    }

    std::string failureText(PreparationError const& failure) const
    {
      auto message = std::string{i18n::requiredText(textCatalog, failure.messageId)};
      return failure.detail.empty() ? message : std::format("{}: {}", message, failure.detail);
    }

    void requestRefresh() const
    {
      if (outputs.requestRefresh)
      {
        outputs.requestRefresh();
      }
    }

    /// Installs a prepared editor, reporting false when its session was already gone.
    bool tryInstall(PreparedEditor prepared)
    {
      auto session = std::move(prepared.session);
      // The observer is attached before the currency recheck, so a session
      // invalidated between preparation and here cannot slip through unseen.
      auto subscription = session.onInvalidated([this] { handleInvalidated(); });

      if (!session.isCurrent())
      {
        return false;
      }

      auto completionProvider = TrackPropertiesEditor::CompletionProvider{
        [completionService = &completionService](
          rt::TrackField const field, std::string_view const text, std::size_t const cursor)
        { return rt::MetadataValueCompleter{*completionService, field}.asProvider()(text, cursor); }};

      // The previous observer goes before the session it watches does.
      invalidatedSubscription.reset();
      optSession.emplace(std::move(session));
      optEditor.emplace(textCatalog, std::move(prepared.preparation), std::move(completionProvider));
      invalidatedSubscription = std::move(subscription);
      return true;
    }

    void handleInvalidated()
    {
      expectCallbackExecutor();

      // A write already in flight reconciles its own binding; calling this a
      // refusal would throw away a result that is still coming.
      if (optEditor && optEditor->status() != TrackEditorStatus::Submitting)
      {
        optEditor->setStatus(TrackEditorStatus::Stale);
        requestRefresh();
      }
    }

    /// Takes the editor away, keeping a submitted write and its session alive.
    void closeEditor()
    {
      optEditor.reset();
      invalidatedSubscription.reset();

      if (!submissionPending)
      {
        optSession.reset();
      }

      requestRefresh();
    }

    void settleSubmission()
    {
      submissionPending = false;

      if (!optEditor)
      {
        optSession.reset();
      }

      if (outputs.notifySubmittedWriteSettled)
      {
        outputs.notifySubmittedWriteSettled();
      }
    }

    /// Reports the terminal result of one submitted write, on the callback executor.
    void completeSubmission(bool const cancelled,
                            Result<uimodel::TrackPropertiesSubmitResult> submitRes,
                            std::exception_ptr unexpected)
    {
      expectCallbackExecutor();

      // Bookkeeping clears before anything that can throw, so a failing
      // presentation cannot leave exit waiting for a write that already landed.
      settleSubmission();

      if (unexpected)
      {
        AO_FATAL_EXCEPTION(std::move(unexpected), "TUI track properties write");
      }

      if (retired)
      {
        return;
      }

      if (cancelled)
      {
        presentStale();
        return;
      }

      if (!submitRes)
      {
        presentFailure(submitRes.error().message);
        return;
      }

      presentResult(*submitRes);
    }

    void presentFailure(std::string diagnostic)
    {
      if (optEditor)
      {
        optEditor->setStatus(TrackEditorStatus::Failed, std::move(diagnostic));
        requestRefresh();
        return;
      }

      post(rt::NotificationSeverity::Error, std::move(diagnostic));
    }

    void presentApplied(std::size_t const changedCount)
    {
      closeEditor();

      // Fewer change records than targets only means some targets already
      // carried the value that was written.
      if (changedCount == 0)
      {
        postText(rt::NotificationSeverity::Info, MessageId::TuiEditorNoChange);
        return;
      }

      post(rt::NotificationSeverity::Info,
           i18n::requiredFormat(textCatalog, MessageId::TuiEditorApplied, {{"count", changedCount}}));
    }

    void presentBusy()
    {
      if (!optEditor)
      {
        postText(rt::NotificationSeverity::Warning, MessageId::TuiEditorBusy);
        return;
      }

      // A Busy refusal leaves the binding intact, so the draft goes back to
      // being submittable instead of demanding a reload first.
      auto const stillCurrent = optSession && optSession->isCurrent();
      optEditor->setStatus(stillCurrent ? TrackEditorStatus::Ready : TrackEditorStatus::Stale,
                           std::string{i18n::requiredText(textCatalog, MessageId::TuiEditorBusy)});
      requestRefresh();
    }

    void presentStale()
    {
      if (!optEditor)
      {
        postText(rt::NotificationSeverity::Warning, MessageId::TuiEditorOpenUnavailable);
        return;
      }

      optEditor->setStatus(TrackEditorStatus::Stale);
      requestRefresh();
    }

    void presentResult(uimodel::TrackPropertiesSubmitResult const& result)
    {
      switch (result.status)
      {
        case rt::AuthoringStatus::Applied:
        case rt::AuthoringStatus::NoOp: presentApplied(countDistinctChangedTracks(result.reply)); return;
        case rt::AuthoringStatus::Busy: presentBusy(); return;
        case rt::AuthoringStatus::Stale:
        case rt::AuthoringStatus::Unavailable: presentStale(); return;
      }
    }

    async::Runtime& runtime;
    rt::Library& library;
    rt::NotificationService& notifications;
    i18n::MessageCatalog const& textCatalog;
    Outputs outputs;
    rt::CompletionService& completionService;
    rt::TextOrderingPolicy const* textOrderingPolicy = nullptr;
    std::optional<TrackPropertiesEditor> optEditor{};
    std::optional<uimodel::TrackAuthoringSession> optSession{};
    async::Subscription invalidatedSubscription{};
    bool submissionPending = false;
    bool retired = false;
  };

  TrackEditController::TrackEditController(async::Runtime& runtime,
                                           rt::Library& library,
                                           rt::NotificationService& notifications,
                                           i18n::MessageCatalog const& textCatalog,
                                           Outputs outputs,
                                           rt::CompletionService& completionService,
                                           rt::TextOrderingPolicy const* const textOrderingPolicy)
    : _statePtr{std::make_shared<State>(runtime,
                                        library,
                                        notifications,
                                        textCatalog,
                                        std::move(outputs),
                                        completionService,
                                        textOrderingPolicy)}
  {
  }

  TrackEditController::~TrackEditController()
  {
    // A submitted write keeps the shared State alive past this owner, so the
    // destructor only drops what this owner lent it: the editor, the observer,
    // and the callbacks whose targets are about to disappear. It dispatches
    // nothing and calls no presentation code.
    _statePtr->retired = true;
    _statePtr->optEditor.reset();
    _statePtr->invalidatedSubscription.reset();
    _statePtr->outputs = Outputs{};
  }

  bool TrackEditController::tryOpen(std::vector<TrackId> targetIds)
  {
    auto& state = *_statePtr;
    state.expectCallbackExecutor();

    // A write from the previous editor is still in flight, and its settle path
    // reports on whichever editor is tryOpen when it lands; a second editor opened
    // now would be handed the first one's result.
    if (state.retired || state.optEditor || state.submissionPending)
    {
      return false;
    }

    if (targetIds.empty())
    {
      state.postText(rt::NotificationSeverity::Warning, MessageId::TuiEditorOpenNoTargets);
      return false;
    }

    auto preparedRes = prepareEditor(state.library, state.textCatalog, targetIds, state.textOrderingPolicy);

    if (!preparedRes)
    {
      state.post(rt::NotificationSeverity::Warning, state.failureText(preparedRes.error()));
      return false;
    }

    if (!state.tryInstall(std::move(*preparedRes)))
    {
      state.postText(rt::NotificationSeverity::Warning, MessageId::TuiEditorOpenUnavailable);
      return false;
    }

    state.requestRefresh();
    return true;
  }

  TrackPropertiesEditor const* TrackEditController::activeEditor() const noexcept
  {
    return _statePtr->optEditor ? &*_statePtr->optEditor : nullptr;
  }

  bool TrackEditController::hasPendingSubmission() const noexcept
  {
    return _statePtr->submissionPending;
  }

  bool TrackEditController::tryHandleEvent(ftxui::Event const& event)
  {
    auto& state = *_statePtr;
    state.expectCallbackExecutor();

    if (!state.optEditor)
    {
      return false;
    }

    std::ignore = state.optEditor->tryHandleEvent(event);
    serviceRequest();
    // An open editor answers for everything the terminal delivers, so the
    // workspace behind it never sees a key it would act on.
    return true;
  }

  void TrackEditController::retire()
  {
    auto& state = *_statePtr;
    state.expectCallbackExecutor();

    state.retired = true;

    if (state.optEditor)
    {
      state.closeEditor();
    }
  }

  void TrackEditController::serviceRequest()
  {
    auto& state = *_statePtr;

    if (!state.optEditor)
    {
      return;
    }

    switch (state.optEditor->takeRequest())
    {
      case TrackEditorRequest::None: return;
      case TrackEditorRequest::Close: state.closeEditor(); return;
      case TrackEditorRequest::Apply: submit(); return;
      case TrackEditorRequest::Reload: reload(); return;
    }
  }

  void TrackEditController::submit()
  {
    auto& state = *_statePtr;

    if (!state.optEditor || state.submissionPending || !state.optSession)
    {
      return;
    }

    // Nothing below installs, so one reference serves the whole submission.
    auto& editor = *state.optEditor;

    if (!state.optSession->isCurrent())
    {
      editor.setStatus(TrackEditorStatus::Stale);
      state.requestRefresh();
      return;
    }

    // The task is built here, from the session just found current, so the
    // coroutine that awaits it never has to reach for a session that the
    // editor's own closure may already have released.
    auto submission = state.optSession->submitPropertiesAsync(editor.buildPatch());
    editor.setStatus(TrackEditorStatus::Submitting);
    state.submissionPending = true;
    state.requestRefresh();
    state.runtime.spawnLogged(runSubmitAsync(_statePtr, std::move(submission)), "TUI track properties write");
  }

  void TrackEditController::reload()
  {
    auto& state = *_statePtr;

    if (state.submissionPending || !state.optEditor)
    {
      return;
    }

    auto targetIds = std::vector<TrackId>{};
    targetIds.reserve(state.optEditor->targets().size());

    for (auto const& target : state.optEditor->targets())
    {
      targetIds.push_back(target.id);
    }

    // A successful install replaces the editor in place, so no reference is
    // held across one: every report below reads back whichever editor is open
    // at the moment it is made.
    auto preparedRes = prepareEditor(state.library, state.textCatalog, targetIds, state.textOrderingPolicy);

    if (!preparedRes)
    {
      // The draft outlives a refused reload; only the reason changes.
      if (state.optEditor)
      {
        state.optEditor->setStatus(TrackEditorStatus::Failed, state.failureText(preparedRes.error()));
      }

      state.requestRefresh();
      return;
    }

    // A refused install replaced nothing, so the editor still open is the one
    // that lost its binding.
    if (!state.tryInstall(std::move(*preparedRes)) && state.optEditor)
    {
      state.optEditor->setStatus(TrackEditorStatus::Stale);
    }

    state.requestRefresh();
  }

  async::Task<void> TrackEditController::runSubmitAsync(
    std::shared_ptr<State> const statePtr,
    async::Task<Result<uimodel::TrackPropertiesSubmitResult>> submission)
  {
    auto submitRes = Result<uimodel::TrackPropertiesSubmitResult>{};
    auto unexpected = std::exception_ptr{};
    bool cancelled = false;

    try
    {
      // Session state shares the event thread with invalidation callbacks.
      co_await statePtr->runtime.resumeOnCallbackExecutorAsync();
      submitRes = co_await std::move(submission);
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

    co_await statePtr->runtime.resumeOnCallbackExecutorAsync();
    statePtr->completeSubmission(cancelled, std::move(submitRes), unexpected);
  }
} // namespace ao::tui
