// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "ListAuthoringController.h"

#include "MouseBindings.h"
#include "Render.h"
#include "SmartListEditor.h"
#include "Style.h"
#include "TextCell.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/QueryExpressionCompleter.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/library/list/ListActions.h>
#include <ao/uimodel/library/list/ListAuthoring.h>
#include <ao/uimodel/library/list/SmartListEditing.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <source_location>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    using i18n::MessageId;

    constexpr auto kPreviewDebounceInterval = std::chrono::milliseconds{200};
    /// Removed Lists a delete confirmation names before counting the rest.
    constexpr std::size_t kDeleteEntryRowLimit = 6;

    /// Everything one preview recomputation produced for the open editor.
    struct ListPreview final
    {
      uimodel::SmartListEditorViewState viewState{};
      std::vector<std::string> tracks{};
    };

    bool hasListChildren(rt::LibrarySnapshot const& snapshot, ListId const listId)
    {
      return std::ranges::any_of(
        snapshot.lists(), [listId](rt::ListNode const& node) { return node.parentId == listId; });
    }

    /// The visible spelling of a membership tag, the same `#name` form the
    /// expression language itself uses.
    std::string displayedTag(std::string_view const tag)
    {
      return query::serialize(query::VariableExpression{.type = query::VariableType::Tag, .name = std::string{tag}});
    }

    /// The shared question embeds its own line structure, so each line wraps
    /// as its own paragraph instead of feeding the breaks to one block.
    void appendQuestionLines(std::vector<ftxui::Element>& body, std::string_view const question)
    {
      std::size_t lineBreak = question.find('\n');
      std::size_t lineStart = 0;

      while (lineBreak != std::string::npos)
      {
        body.push_back(ftxui::paragraph(std::string{question.substr(lineStart, lineBreak - lineStart)}));
        lineStart = lineBreak + 1;
        lineBreak = question.find('\n', lineStart);
      }

      body.push_back(ftxui::paragraph(std::string{question.substr(lineStart)}));
    }

    /**
     * @brief Recomputes one editor preview from a live source.
     *
     * The source lease is acquired for the parent's source with the draft's
     * local expression, exactly what saving would evaluate, so the status,
     * diagnostic, and sample rows all describe the definition being drafted.
     */
    ListPreview computeListPreview(rt::Library& library,
                                   rt::ViewService& views,
                                   rt::TrackSourceCache& sources,
                                   i18n::MessageCatalog const& textCatalog,
                                   ListId const parentListId,
                                   std::string_view const name,
                                   std::string_view const localExpression)
    {
      auto const sourceListId = rt::resolveParentSourceId(parentListId);
      auto sourceRes =
        sources.acquire(rt::SourceSpec{.baseListId = sourceListId, .filterExpression = std::string{localExpression}});

      if (!sourceRes)
      {
        // The parent source itself is gone, so nothing about the draft can be
        // previewed or safely written; the runtime's own wording says why.
        auto state = uimodel::SmartListEditorViewState{};
        state.name = std::string{name};
        state.localExpression = std::string{localExpression};
        state.expressionValid = false;
        state.errorVisible = true;
        state.errorText =
          i18n::requiredFormat(textCatalog, MessageId::TrackFilterError, {{"diagnostic", sourceRes.error().message}});
        return {.viewState = std::move(state), .tracks = {}};
      }

      auto const& lease = *sourceRes;
      auto const optError = sources.sourceError(lease);
      auto viewState = uimodel::makeSmartListEditorViewState(
        textCatalog,
        uimodel::SmartListPreviewState{.name = name,
                                       .localExpression = localExpression,
                                       .hasPreviewSource = true,
                                       .hasError = optError.has_value(),
                                       .errorMessage = optError ? optError->message : std::string{},
                                       .matchCount = lease.source().size(),
                                       .isAllTracks = rt::isVirtualListId(parentListId)});

      auto tracks = std::vector<std::string>{};

      if (viewState.previewVisible)
      {
        // A transient projection supplies the same ordered membership the
        // saved List will show, and only its bounded leading rows are read.
        auto projectionPtr = views.createTransientTrackListProjection(lease, rt::TrackOrderSpec{});
        auto const snapshot = library.snapshot();
        auto const visibleCount = std::min(projectionPtr->size(), uimodel::kSmartListPreviewLimit);
        tracks.reserve(visibleCount);

        for (std::size_t index = 0; index < visibleCount; ++index)
        {
          if (auto const optRow = snapshot.trackRow(projectionPtr->trackIdAt(index)); optRow)
          {
            tracks.push_back(
              uimodel::formatSmartListPreviewTrackLabel(textCatalog, optRow->title, optRow->artist, optRow->album));
          }
        }
      }

      return {.viewState = std::move(viewState), .tracks = std::move(tracks)};
    }
  } // namespace

  struct ListAuthoringController::State final
  {
    State(async::Runtime& runtimeIn,
          rt::Library& libraryIn,
          rt::ViewService& viewsIn,
          rt::TrackSourceCache& sourcesIn,
          rt::WorkspaceService& workspaceIn,
          rt::CompletionService& completionIn,
          rt::NotificationService& notificationsIn,
          uimodel::ListPresentations& listPresentationsIn,
          i18n::MessageCatalog const& textCatalogIn,
          Outputs outputsIn)
      : runtime{runtimeIn}
      , library{libraryIn}
      , views{viewsIn}
      , sources{sourcesIn}
      , workspace{workspaceIn}
      , completion{completionIn}
      , notifications{notificationsIn}
      , listPresentations{listPresentationsIn}
      , textCatalog{textCatalogIn}
      , outputs{std::move(outputsIn)}
    {
    }

    void expectCallbackExecutor(std::source_location const location = std::source_location::current()) const
    {
      AO_EXPECTS_AT(location,
                    runtime.callbackExecutor().isCurrent(),
                    "TUI List authoring bookkeeping must run on the callback executor");
    }

    void post(rt::NotificationSeverity const severity, std::string message)
    {
      notifications.post(severity, std::move(message), rt::NotificationLifetime::transient());
    }

    void postText(rt::NotificationSeverity const severity, MessageId const id)
    {
      post(severity, std::string{i18n::requiredText(textCatalog, id)});
    }

    void requestRefresh() const
    {
      if (outputs.requestRefresh)
      {
        outputs.requestRefresh();
      }
    }

    /// Ends any workspace gesture or transient input an asynchronously arrived surface would interrupt.
    void cancelShellInteractions() const
    {
      if (outputs.cancelTransientInteractions)
      {
        outputs.cancelTransientInteractions();
      }
    }

    /// Recomputes and installs everything the editor shows about its draft.
    void refreshPreviewFor(SmartListEditor& editor)
    {
      auto const draft = editor.draft();
      auto preview =
        computeListPreview(library, views, sources, textCatalog, editor.parentListId(), draft.name, draft.expression);
      editor.applyPreview(std::move(preview.viewState), std::move(preview.tracks));
    }

    /// Takes the editor away, keeping a submitted write's settlement path intact.
    void closeEditor()
    {
      previewTask.reset();
      ++previewGeneration;
      optEditor.reset();
      requestRefresh();
    }

    void settleSubmission()
    {
      submissionPending = false;

      if (outputs.notifySubmittedWriteSettled)
      {
        outputs.notifySubmittedWriteSettled();
      }
    }

    /// Installs a prepared editor over its baseline, preview included.
    void installEditor(ListEditorMode const mode, rt::ListDraft baseline)
    {
      auto completionProvider = SmartListEditor::CompletionProvider{
        [completion = &completion](std::string_view const text, std::size_t const cursor)
        { return rt::QueryExpressionCompleter{*completion}.complete(text, cursor); }};

      // The inherited expression is read through the same coherent snapshot
      // the preview will use, so both halves of the effective filter agree.
      auto parentExpression = std::string{};

      if (auto const optParent = library.snapshot().listNode(baseline.parentId); optParent)
      {
        parentExpression = optParent->expression;
      }

      auto editor = SmartListEditor{
        textCatalog, mode, std::move(baseline), std::move(parentExpression), std::move(completionProvider)};
      refreshPreviewFor(editor);
      optEditor.emplace(std::move(editor));
      requestRefresh();
    }

    /// Reports the terminal result of one submitted save, on the callback executor.
    void completeSave(bool const cancelled, Result<ListId> saveRes, std::exception_ptr unexpected)
    {
      expectCallbackExecutor();

      // Bookkeeping clears before anything that can throw, so a failing
      // presentation cannot leave exit waiting for a write that already landed.
      settleSubmission();

      if (unexpected)
      {
        AO_FATAL_EXCEPTION(std::move(unexpected), "TUI List authoring save");
      }

      if (retired)
      {
        return;
      }

      if (cancelled)
      {
        presentSaveCancelled();
        return;
      }

      if (!saveRes)
      {
        presentFailure(saveRes.error().message);
        return;
      }

      presentSaved(*saveRes);
    }

    void presentSaveCancelled()
    {
      if (optEditor)
      {
        // The write never landed, so the draft stays exactly as it was and a
        // retry needs nothing more than the ordinary Save key.
        optEditor->setStatus(
          ListEditorStatus::Ready, std::string{i18n::requiredText(textCatalog, MessageId::TuiEditorOpenUnavailable)});
        requestRefresh();
        return;
      }

      postText(rt::NotificationSeverity::Warning, MessageId::TuiEditorOpenUnavailable);
    }

    void presentFailure(std::string diagnostic)
    {
      if (optEditor)
      {
        // A failed save leaves the draft intact and a retry available, unlike
        // a track session, whose binding must be reloaded first.
        optEditor->setStatus(ListEditorStatus::Ready, std::move(diagnostic));
        requestRefresh();
        return;
      }

      post(rt::NotificationSeverity::Error, std::move(diagnostic));
    }

    void presentSaved(ListId const listId)
    {
      // An edit keeps the List's stored presentation choice, exactly as the
      // other frontends reselect it in their editor; every other save resolves
      // the Auto presentation from the expression that was just written.
      auto const& localExpression = optEditor ? optEditor->draft().expression : std::string{};
      auto const isEdit = optEditor && optEditor->mode() == ListEditorMode::Edit;
      auto const optStored =
        isEdit ? listPresentations.presentationIdForList(listId) : std::optional<std::string_view>{};
      auto presentationId = std::string{};

      if (optStored)
      {
        // The borrowed id is copied before the map entry it names can be
        // replaced below.
        presentationId = std::string{*optStored};
      }
      else
      {
        presentationId = uimodel::resolveSmartListTrackPresentationId(uimodel::kSmartListAutoTrackPresentationIndex,
                                                                      true,
                                                                      localExpression,
                                                                      rt::builtinTrackPresentationPresets(),
                                                                      workspace.customPresets());
      }

      closeEditor();
      listPresentations.setPresentationIdForList(listId, presentationId);
      requestRefresh();
    }

    /// Installs the confirmation for one settled deletion preview.
    void presentDeletePreview(bool const includeDescendants, Result<rt::DeleteListSubtreeReply> previewRes)
    {
      AO_INVARIANT(!previewRes->deletedLists.empty(), "A successful List deletion preview must contain its root List");

      auto const& preview = previewRes->deletedLists.front();
      auto confirmation = ListDeleteConfirmation{};
      confirmation.listId = previewRes->rootListId;
      confirmation.includeDescendants = includeDescendants;
      confirmation.title = std::string{i18n::requiredText(
        textCatalog, includeDescendants ? MessageId::ListDeleteSubtreeTitle : MessageId::ListDeleteTitle)};

      if (includeDescendants)
      {
        confirmation.deletedListNames.reserve(previewRes->deletedLists.size());

        for (auto const& list : previewRes->deletedLists)
        {
          confirmation.deletedListNames.push_back(list.name);
        }
      }
      else
      {
        confirmation.question =
          i18n::requiredFormat(textCatalog, MessageId::ListDeleteQuestion, {{"name", preview.name}});
      }

      if (preview.optTagImpact)
      {
        confirmation.tagImpactQuestion = i18n::requiredFormat(
          textCatalog,
          MessageId::ListRemoveTag,
          {{"tag", displayedTag(preview.optTagImpact->tag)}, {"count", preview.optTagImpact->taggedTrackCount}});

        if (!preview.optTagImpact->otherListReferences.empty())
        {
          auto references = std::string{};

          for (auto const& reference : preview.optTagImpact->otherListReferences)
          {
            if (!references.empty())
            {
              references.append(", ");
            }

            references.append(reference.name);
          }

          confirmation.tagReferencesWarning =
            i18n::requiredFormat(textCatalog,
                                 MessageId::ListTagReferences,
                                 {{"tag", displayedTag(preview.optTagImpact->tag)}, {"references", references}});
        }
      }

      optDeleteConfirmation.emplace(std::move(confirmation));

      // The confirmation is the one surface that appears without a keypress,
      // so the workspace gestures it interrupts are retired here rather than
      // by the event that opened it.
      cancelShellInteractions();
      requestRefresh();
    }

    /// Reports the terminal result of one submitted deletion, on the callback executor.
    void completeDeletion(bool const cancelled,
                          Result<rt::DeleteListSubtreeReply> deleteRes,
                          std::exception_ptr unexpected)
    {
      expectCallbackExecutor();

      settleSubmission();

      if (unexpected)
      {
        AO_FATAL_EXCEPTION(std::move(unexpected), "TUI List deletion");
      }

      if (retired)
      {
        return;
      }

      if (cancelled)
      {
        if (optDeleteConfirmation)
        {
          // The deletion never landed, so the question returns to its
          // answerable state rather than claiming anything was removed.
          optDeleteConfirmation->deleting = false;
          postText(rt::NotificationSeverity::Warning, MessageId::TuiListDeleteCancelled);
          requestRefresh();
        }

        return;
      }

      optDeleteConfirmation.reset();

      if (!deleteRes)
      {
        post(rt::NotificationSeverity::Error, deleteRes.error().message);
        requestRefresh();
        return;
      }

      // The published change set retires the deleted List's presentation
      // entry and rebuilds navigation; nothing else is claimed here.
      requestRefresh();
    }

    async::Runtime& runtime;
    rt::Library& library;
    rt::ViewService& views;
    rt::TrackSourceCache& sources;
    rt::WorkspaceService& workspace;
    rt::CompletionService& completion;
    rt::NotificationService& notifications;
    uimodel::ListPresentations& listPresentations;
    i18n::MessageCatalog const& textCatalog;
    Outputs outputs;
    std::optional<SmartListEditor> optEditor{};
    std::optional<ListDeleteConfirmation> optDeleteConfirmation{};
    bool deletePreviewPending = false;
    bool submissionPending = false;
    std::uint64_t previewGeneration = 0;
    async::TaskHandle previewTask{};
    mutable MouseBindings confirmationMouseBindings;
    mutable bool confirmationMouseReady = false;
    bool retired = false;
  };

  ListAuthoringController::ListAuthoringController(async::Runtime& runtime,
                                                   rt::Library& library,
                                                   rt::ViewService& views,
                                                   rt::TrackSourceCache& sources,
                                                   rt::WorkspaceService& workspace,
                                                   rt::CompletionService& completion,
                                                   rt::NotificationService& notifications,
                                                   uimodel::ListPresentations& listPresentations,
                                                   i18n::MessageCatalog const& textCatalog,
                                                   Outputs outputs)
    : _statePtr{std::make_shared<State>(runtime,
                                        library,
                                        views,
                                        sources,
                                        workspace,
                                        completion,
                                        notifications,
                                        listPresentations,
                                        textCatalog,
                                        std::move(outputs))}
  {
  }

  ListAuthoringController::~ListAuthoringController()
  {
    // A submitted write keeps the shared State alive past this owner, so the
    // destructor only drops what this owner lent it: the surfaces, the
    // debounce task, and the callbacks whose targets are about to disappear.
    // It dispatches nothing and calls no presentation code.
    _statePtr->retired = true;
    _statePtr->optEditor.reset();
    _statePtr->optDeleteConfirmation.reset();
    _statePtr->previewTask.reset();
    ++_statePtr->previewGeneration;
    _statePtr->outputs = Outputs{};
  }

  bool ListAuthoringController::tryOpenNew(ListId const targetListId)
  {
    auto& state = *_statePtr;
    state.expectCallbackExecutor();

    if (state.retired || state.optEditor || state.optDeleteConfirmation || state.deletePreviewPending ||
        state.submissionPending)
    {
      return false;
    }

    // A virtual target parents the new List at the library root.
    auto baseline = rt::ListDraft{};
    baseline.parentId = uimodel::parentForNewSmartList(targetListId);
    state.installEditor(ListEditorMode::New, std::move(baseline));
    return true;
  }

  bool ListAuthoringController::tryOpenEdit(ListId const targetListId)
  {
    auto& state = *_statePtr;
    state.expectCallbackExecutor();

    if (state.retired || state.optEditor || state.optDeleteConfirmation || state.deletePreviewPending ||
        state.submissionPending)
    {
      return false;
    }

    auto const actions = uimodel::describeListActions(targetListId, false);

    if (!actions.canEdit)
    {
      state.postText(rt::NotificationSeverity::Warning, MessageId::TuiListVirtualTarget);
      return false;
    }

    auto const snapshot = state.library.snapshot();

    if (auto const optNode = snapshot.listNode(targetListId); optNode)
    {
      auto baseline = rt::ListDraft{};
      baseline.parentId = optNode->parentId;
      baseline.listId = targetListId;
      baseline.name = optNode->name;
      baseline.description = optNode->description;
      baseline.expression = optNode->expression;
      state.installEditor(ListEditorMode::Edit, std::move(baseline));
      return true;
    }

    state.postText(rt::NotificationSeverity::Warning, MessageId::TuiListOpenUnavailable);
    return false;
  }

  bool ListAuthoringController::tryDelete(ListId const targetListId)
  {
    auto& state = *_statePtr;
    state.expectCallbackExecutor();

    if (state.retired || state.optEditor || state.optDeleteConfirmation || state.deletePreviewPending ||
        state.submissionPending)
    {
      return false;
    }

    auto const snapshot = state.library.snapshot();
    auto const actions = uimodel::describeListActions(targetListId, hasListChildren(snapshot, targetListId));

    if (!actions.canDelete && !actions.canDeleteSubtree)
    {
      state.postText(rt::NotificationSeverity::Warning, MessageId::TuiListVirtualTarget);
      return false;
    }

    // A List with descendants is deleted as its subtree; the preview and the
    // confirmation both describe exactly that scope.
    auto const includeDescendants = actions.canDeleteSubtree;
    auto previewTask = uimodel::previewListDeletionAsync(&state.library, targetListId, includeDescendants);
    state.deletePreviewPending = true;
    state.runtime.spawnLogged(
      runDeletePreviewAsync(_statePtr, includeDescendants, std::move(previewTask)), "TUI List deletion preview");
    return true;
  }

  SmartListEditor const* ListAuthoringController::activeEditor() const noexcept
  {
    return _statePtr->optEditor ? &*_statePtr->optEditor : nullptr;
  }

  ListDeleteConfirmation const* ListAuthoringController::activeDeleteConfirmation() const noexcept
  {
    return _statePtr->optDeleteConfirmation ? &*_statePtr->optDeleteConfirmation : nullptr;
  }

  bool ListAuthoringController::isActive() const noexcept
  {
    return _statePtr->optEditor || _statePtr->optDeleteConfirmation;
  }

  bool ListAuthoringController::hasPendingSubmission() const noexcept
  {
    return _statePtr->submissionPending;
  }

  bool ListAuthoringController::isBusy() const noexcept
  {
    return _statePtr->deletePreviewPending || _statePtr->submissionPending;
  }

  bool ListAuthoringController::tryHandleEvent(ftxui::Event const& event)
  {
    auto& state = *_statePtr;
    state.expectCallbackExecutor();

    if (state.optEditor)
    {
      std::ignore = state.optEditor->tryHandleEvent(event);
      serviceRequest();
      // An open editor answers for everything the terminal delivers, so the
      // workspace behind it never sees a key it would act on.
      return true;
    }

    if (state.optDeleteConfirmation)
    {
      handleDeleteConfirmationEvent(event);
      return true;
    }

    return false;
  }

  void ListAuthoringController::retire()
  {
    auto& state = *_statePtr;
    state.expectCallbackExecutor();

    state.retired = true;

    if (state.optEditor)
    {
      state.closeEditor();
    }

    state.optDeleteConfirmation.reset();
    state.previewTask.reset();
    ++state.previewGeneration;
  }

  void ListAuthoringController::serviceRequest()
  {
    auto& state = *_statePtr;

    if (!state.optEditor)
    {
      return;
    }

    switch (state.optEditor->takeRequest())
    {
      case ListEditorRequest::None: return;
      case ListEditorRequest::Close: state.closeEditor(); return;
      case ListEditorRequest::Submit: submit(); return;
      case ListEditorRequest::PreviewChanged: schedulePreview(); return;
    }
  }

  void ListAuthoringController::submit()
  {
    auto& state = *_statePtr;

    if (!state.optEditor || state.submissionPending)
    {
      return;
    }

    auto& editor = *state.optEditor;

    if (!editor.canSubmit())
    {
      return;
    }

    cancelPreviewDebounce();

    // The task is built here, from the draft the editor just reported, so
    // the coroutine that awaits it never has to reach for an editor that the
    // editor's own closure may already have released.
    auto submission = uimodel::saveListAsync(&state.library, editor.draft());
    editor.setStatus(ListEditorStatus::Submitting);
    state.submissionPending = true;
    state.requestRefresh();
    state.runtime.spawnLogged(runSaveAsync(_statePtr, std::move(submission)), "TUI List authoring save");
  }

  void ListAuthoringController::schedulePreview()
  {
    auto& state = *_statePtr;

    cancelPreviewDebounce();
    auto const generation = state.previewGeneration;
    state.previewTask = state.runtime.spawnCancellable(
      [runtime = &state.runtime, statePtr = _statePtr, generation](std::stop_token const stopToken)
      { return runPreviewDebounceAsync(runtime, statePtr, generation, stopToken); },
      "TUI List authoring preview");
  }

  void ListAuthoringController::cancelPreviewDebounce()
  {
    auto& state = *_statePtr;
    state.previewTask.reset();
    ++state.previewGeneration;
  }

  void ListAuthoringController::handleDeleteConfirmationEvent(ftxui::Event const& event)
  {
    auto& state = *_statePtr;

    // The caller routes here only while a confirmation exists, but the flow
    // owns its own guard so a raced reset cannot dereference absence.
    if (!state.optDeleteConfirmation)
    {
      return;
    }

    auto& confirmation = *state.optDeleteConfirmation;

    // A deletion in flight cannot be confirmed twice or cancelled, so the
    // question stays visible and inert until its result arrives.
    if (confirmation.deleting)
    {
      return;
    }

    if (event.is_mouse())
    {
      auto mouseEvent = event;
      auto const& mouse = mouseEvent.mouse();

      if (!isLeftPress(mouse) || !std::exchange(state.confirmationMouseReady, false))
      {
        return;
      }

      if (auto const optEvent = state.confirmationMouseBindings.eventAt(mouse); optEvent)
      {
        state.confirmationMouseBindings.clear();
        handleDeleteConfirmationEvent(*optEvent);
      }

      return;
    }

    state.confirmationMouseReady = false;

    if (event == ftxui::Event::Escape)
    {
      state.optDeleteConfirmation.reset();
      state.requestRefresh();
      return;
    }

    if (event == ftxui::Event::Return)
    {
      auto const listId = confirmation.listId;
      auto const includeDescendants = confirmation.includeDescendants;
      auto options = rt::DeleteListOptions{};
      options.removeWritableTagFromTracks = confirmation.removeWritableTag;

      // The task is built before the in-flight flag is armed, so a refused
      // spawn can never leave a pending write nobody settles.
      auto deletion = uimodel::deleteListAsync(&state.library, listId, includeDescendants, options);
      confirmation.deleting = true;
      state.submissionPending = true;
      state.requestRefresh();
      state.runtime.spawnLogged(runDeleteAsync(_statePtr, std::move(deletion)), "TUI List deletion");
      return;
    }

    if (event == ftxui::Event::Character(" "))
    {
      // Only a writable membership tag offers the extra cleanup step.
      if (!confirmation.tagImpactQuestion.empty())
      {
        confirmation.removeWritableTag = !confirmation.removeWritableTag;
        state.requestRefresh();
      }

      return;
    }
  }

  async::Task<void> ListAuthoringController::runSaveAsync(std::shared_ptr<State> const statePtr,
                                                          async::Task<Result<ListId>> submission)
  {
    auto saveRes = Result<ListId>{};
    auto unexpected = std::exception_ptr{};
    bool cancelled = false;

    try
    {
      // State shares the event thread with TUI dispatch.
      co_await statePtr->runtime.resumeOnCallbackExecutorAsync();
      saveRes = co_await std::move(submission);
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
    statePtr->completeSave(cancelled, std::move(saveRes), unexpected);
  }

  async::Task<void> ListAuthoringController::runDeletePreviewAsync(
    std::shared_ptr<State> const statePtr,
    bool const includeDescendants,
    async::Task<Result<rt::DeleteListSubtreeReply>> preview)
  {
    auto previewRes = Result<rt::DeleteListSubtreeReply>{};
    auto unexpected = std::exception_ptr{};
    bool cancelled = false;

    try
    {
      co_await statePtr->runtime.resumeOnCallbackExecutorAsync();
      previewRes = co_await std::move(preview);
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
    statePtr->deletePreviewPending = false;

    if (unexpected)
    {
      AO_FATAL_EXCEPTION(std::move(unexpected), "TUI List deletion preview");
    }

    if (statePtr->retired || cancelled)
    {
      co_return;
    }

    if (!previewRes)
    {
      statePtr->post(rt::NotificationSeverity::Error, previewRes.error().message);
      co_return;
    }

    statePtr->presentDeletePreview(includeDescendants, std::move(previewRes));
  }

  async::Task<void> ListAuthoringController::runDeleteAsync(std::shared_ptr<State> const statePtr,
                                                            async::Task<Result<rt::DeleteListSubtreeReply>> deletion)
  {
    auto deleteRes = Result<rt::DeleteListSubtreeReply>{};
    auto unexpected = std::exception_ptr{};
    bool cancelled = false;

    try
    {
      co_await statePtr->runtime.resumeOnCallbackExecutorAsync();
      deleteRes = co_await std::move(deletion);
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
    statePtr->completeDeletion(cancelled, std::move(deleteRes), unexpected);
  }

  async::Task<void> ListAuthoringController::runPreviewDebounceAsync(async::Runtime* const runtime,
                                                                     std::shared_ptr<State> const statePtr,
                                                                     std::uint64_t const generation,
                                                                     std::stop_token const stopToken)
  {
    co_await runtime->sleepForAsync(kPreviewDebounceInterval, stopToken);
    co_await runtime->resumeOnCallbackExecutorAsync(stopToken);

    // A newer edit, a submission, or a closure already replaced this
    // generation, so its preview describes a draft nobody is looking at.
    if (generation != statePtr->previewGeneration || !statePtr->optEditor || statePtr->retired)
    {
      co_return;
    }

    statePtr->refreshPreviewFor(*statePtr->optEditor);
    statePtr->requestRefresh();
  }

  ftxui::Element ListAuthoringController::activeModal(std::int32_t const terminalColumns,
                                                      std::int32_t const terminalRows) const
  {
    if (auto const* const editor = activeEditor(); editor != nullptr)
    {
      return editor->renderModal(terminalColumns, terminalRows);
    }

    if (auto const* const confirmation = activeDeleteConfirmation(); confirmation != nullptr)
    {
      return renderDeleteConfirmation(*confirmation, terminalColumns, terminalRows);
    }

    return nullptr;
  }

  std::string ListAuthoringController::buildSubtreeEntries(ListDeleteConfirmation const& confirmation,
                                                           std::ptrdiff_t const entryBudget,
                                                           std::int32_t const entryColumns) const
  {
    using i18n::MessageId;

    auto& state = *_statePtr;

    // The subtree question names its entries inside the shared pattern, so
    // the modal owns how many fit: a bounded leading window, one localized
    // count line for the rest, and never more rows than the terminal can
    // hold together with the question and its footer.
    auto const budget = static_cast<std::size_t>(std::max<std::ptrdiff_t>(1, entryBudget));
    auto visible = std::min(confirmation.deletedListNames.size(), kDeleteEntryRowLimit);
    auto hidden = confirmation.deletedListNames.size() - visible;

    if (auto const entryRows = visible + (hidden > 0 ? 1 : 0); entryRows > budget)
    {
      auto const excess = entryRows - budget;
      visible = visible > excess ? visible - excess : 0;
      hidden = confirmation.deletedListNames.size() - visible;
    }

    auto entries = std::string{};

    for (std::size_t index = 0; index < visible; ++index)
    {
      if (!entries.empty())
      {
        entries.append("\n");
      }

      entries.append("• ").append(fitCellText(confirmation.deletedListNames[index], entryColumns));
    }

    if (hidden > 0)
    {
      if (!entries.empty())
      {
        entries.append("\n");
      }

      entries.append(i18n::requiredFormat(state.textCatalog, MessageId::TuiListDeleteMore, {{"count", hidden}}));
    }

    return entries;
  }

  ftxui::Element ListAuthoringController::renderDeleteConfirmation(ListDeleteConfirmation const& confirmation,
                                                                   std::int32_t const terminalColumns,
                                                                   std::int32_t const terminalRows) const
  {
    using namespace ftxui;

    auto& state = *_statePtr;
    state.confirmationMouseBindings.clear();
    state.confirmationMouseReady = true;

    auto const modalCols = std::min(terminalColumns, std::clamp(terminalColumns - 4, 60, 80));
    auto const availableRows = std::max(1, terminalRows - 2);

    // Rows the modal needs around the entry lines: border, title, the
    // question's own fixed lines, the tag offer, and the footer that must
    // stay on screen. The warning wraps at constrained widths, so it counts
    // double to keep the budget honest.
    auto const tagRows =
      (confirmation.tagImpactQuestion.empty() ? 0 : 2) + (confirmation.tagReferencesWarning.empty() ? 0 : 2);
    auto const fixedRows = 2 + 1 + 1 + 2 + 2 + tagRows + 1 + 1;

    auto body = Elements{text(confirmation.title) | bold, text("")};

    if (confirmation.includeDescendants)
    {
      appendQuestionLines(
        body,
        i18n::requiredFormat(
          state.textCatalog,
          MessageId::ListDeleteSubtreeQuestion,
          {{"count", confirmation.deletedListNames.size()},
           {"entries", buildSubtreeEntries(confirmation, availableRows - fixedRows, modalCols - 2 - 2)}}));
    }
    else
    {
      appendQuestionLines(body, confirmation.question);
    }

    if (!confirmation.tagImpactQuestion.empty())
    {
      body.push_back(text(""));
      body.push_back(hbox({
        text(confirmation.removeWritableTag ? "[x] " : "[ ] ") | bold,
        text(confirmation.tagImpactQuestion) | flex,
      }));
    }

    if (!confirmation.tagReferencesWarning.empty())
    {
      body.push_back(text(""));
      body.push_back(paragraph(confirmation.tagReferencesWarning) | style::warning());
    }

    body.push_back(filler());

    auto footer = Elements{};

    if (confirmation.deleting)
    {
      footer.push_back(text(std::string{i18n::requiredText(state.textCatalog, MessageId::TuiListStatusDeleting)}) |
                       bold);
    }
    else
    {
      footer.push_back(state.confirmationMouseBindings.bind(
        style::shortcutChip(
          "Enter",
          std::string{i18n::requiredText(
            state.textCatalog,
            confirmation.includeDescendants ? MessageId::ListDeleteAllAction : MessageId::ListDeleteAction)}),
        Event::Return));
      footer.push_back(style::mutedSeparator());

      if (!confirmation.tagImpactQuestion.empty())
      {
        footer.push_back(state.confirmationMouseBindings.bind(
          style::shortcutChip("Space", i18n::requiredText(state.textCatalog, MessageId::TuiEditorHintToggle)),
          Event::Character(" ")));
        footer.push_back(style::mutedSeparator());
      }

      footer.push_back(state.confirmationMouseBindings.bind(
        style::shortcutChip("Esc", i18n::requiredText(state.textCatalog, MessageId::TuiEditorHintClose)),
        Event::Escape));
    }

    body.push_back(hbox(std::move(footer)));

    auto const contentRows = static_cast<std::int32_t>(body.size()) + 2;
    auto const modalRows = std::min(availableRows, std::max(12, contentRows));

    // EQUAL sizing keeps the bounded body the whole modal: the footer can
    // never be pushed past the rows the terminal actually has.
    auto boxPtr =
      vbox(std::move(body)) | border | size(WIDTH, EQUAL, modalCols) | size(HEIGHT, EQUAL, modalRows) | clear_under;

    return vbox({
      filler(),
      hbox({
        filler(),
        std::move(boxPtr),
        filler(),
      }),
      filler(),
    });
  }
} // namespace ao::tui
