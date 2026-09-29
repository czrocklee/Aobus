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
    /// Referencing Lists the tag warning names before counting the rest.
    constexpr std::size_t kReferenceNameLimit = 2;
    /// Column budget for one referenced List name inside the tag warning.
    constexpr std::int32_t kReferenceNameColumns = 24;

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

    /// Appends @p value wrapped to @p columns, one element per row, keeping
    /// the shared text's own line breaks and blank lines, so the modal can
    /// count exactly the rows it lays out.
    void appendWrappedRows(ftxui::Elements& body,
                           std::string_view const value,
                           std::int32_t const columns,
                           ftxui::Decorator const& decorator = ftxui::nothing)
    {
      for (auto& row : wrapCellText(value, std::max(1, columns)))
      {
        body.push_back(ftxui::text(std::move(row)) | decorator);
      }
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
        // A transient projection samples the draft's membership in source
        // order, as the GTK preview does; the saved List then shows it in its
        // resolved presentation. Only the bounded leading rows are read.
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
      auto const isNew = optEditor && optEditor->mode() == ListEditorMode::New;
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

      // A new List is shown at once, as the GTK sidebar selects it: the
      // created List on screen is the save's confirmation. An edit keeps the
      // workspace where the user was maintaining it.
      if (isNew && outputs.openCreatedList)
      {
        outputs.openCreatedList(listId);
      }

      requestRefresh();
    }

    /// Fills the waiting confirmation from one settled deletion preview.
    void presentDeletePreview(Result<rt::DeleteListSubtreeReply> previewRes)
    {
      expectCallbackExecutor();
      AO_INVARIANT(optDeleteConfirmation, "A landing List deletion preview must have its confirmation");
      AO_INVARIANT(!previewRes->deletedLists.empty(), "A successful List deletion preview must contain its root List");

      auto const& preview = previewRes->deletedLists.front();
      auto& confirmation = *optDeleteConfirmation;

      if (confirmation.includeDescendants)
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
          // The warning names a bounded window of references and counts the
          // rest; the renderer then wraps it and counts the rows it takes.
          auto references = std::string{};
          auto const referenceCount = preview.optTagImpact->otherListReferences.size();

          for (std::size_t index = 0; index < referenceCount; ++index)
          {
            if (!references.empty())
            {
              references.append(", ");
            }

            if (index >= kReferenceNameLimit)
            {
              references.append(i18n::requiredFormat(
                textCatalog, MessageId::TuiListDeleteMore, {{"count", referenceCount - kReferenceNameLimit}}));
              break;
            }

            references.append(
              ellipsizeToCellWidth(preview.optTagImpact->otherListReferences[index].name, kReferenceNameColumns));
          }

          confirmation.tagReferencesWarning =
            i18n::requiredFormat(textCatalog,
                                 MessageId::ListTagReferences,
                                 {{"tag", displayedTag(preview.optTagImpact->tag)}, {"references", references}});
        }
      }

      // The question can be answered only once this filled state has been
      // drawn; the renderer arms the keys.
      confirmation.previewReady = true;
      confirmationKeysReady = false;
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
    bool submissionPending = false;
    std::uint64_t previewGeneration = 0;
    /// Names the deletion preview the open confirmation waits for; a
    /// cancelled or replaced confirmation leaves its late preview unmatched.
    std::uint64_t deleteGeneration = 0;
    async::TaskHandle previewTask{};
    mutable MouseBindings confirmationMouseBindings;
    mutable bool confirmationMouseReady = false;
    /// Set once the filled question has been rendered, so a key typed before it was visible cannot confirm it.
    mutable bool confirmationKeysReady = false;
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
    ++_statePtr->deleteGeneration;
    _statePtr->previewTask.reset();
    ++_statePtr->previewGeneration;
    _statePtr->outputs = Outputs{};
  }

  bool ListAuthoringController::tryOpenNew(ListId const targetListId)
  {
    auto& state = *_statePtr;
    state.expectCallbackExecutor();

    if (state.retired || state.optEditor || state.optDeleteConfirmation || state.submissionPending)
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

    if (state.retired || state.optEditor || state.optDeleteConfirmation || state.submissionPending)
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

    if (state.retired || state.optEditor || state.optDeleteConfirmation || state.submissionPending)
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

    // The confirmation owns input from this moment, not from the preview's
    // arrival: a key typed while the runtime is still answering can neither
    // reach the workspace behind it nor confirm a question nobody has seen.
    auto confirmation = ListDeleteConfirmation{};
    confirmation.listId = targetListId;
    confirmation.includeDescendants = includeDescendants;
    confirmation.title = std::string{i18n::requiredText(
      state.textCatalog, includeDescendants ? MessageId::ListDeleteSubtreeTitle : MessageId::ListDeleteTitle)};
    state.optDeleteConfirmation.emplace(std::move(confirmation));
    // A new question starts unarmed for keys and pointer alike: nothing
    // drawn for an earlier question can answer this one.
    state.confirmationKeysReady = false;
    state.confirmationMouseReady = false;
    state.confirmationMouseBindings.clear();
    auto const generation = ++state.deleteGeneration;
    state.runtime.spawnLogged(
      runDeletePreviewAsync(_statePtr, generation, std::move(previewTask)), "TUI List deletion preview");
    state.requestRefresh();
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
    ++state.deleteGeneration;
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
    cancelPreviewDebounce();

    // A fast Ctrl-S can beat the 200 ms debounce, so the last preview that
    // landed may judge older text in either direction. Recompute it from the
    // draft being saved before the gate decides, so the gate and the visible
    // state describe the same text; a refused save leaves the draft's own
    // diagnostic on screen.
    state.refreshPreviewFor(editor);
    state.requestRefresh();

    if (!editor.canSubmit())
    {
      return;
    }

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
      // Cancelling while the preview runs leaves its late answer unmatched.
      state.optDeleteConfirmation.reset();
      ++state.deleteGeneration;
      state.requestRefresh();
      return;
    }

    // Only Escape answers a question that is still being prepared, and only a
    // question already drawn in full can be confirmed or toggled.
    if (!confirmation.previewReady || !state.confirmationKeysReady)
    {
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
    std::uint64_t const generation,
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

    if (unexpected)
    {
      AO_FATAL_EXCEPTION(std::move(unexpected), "TUI List deletion preview");
    }

    // A retired controller, a cancelled or replaced confirmation, and a
    // cancelled preview all leave nothing waiting for this answer.
    if (statePtr->retired || generation != statePtr->deleteGeneration || !statePtr->optDeleteConfirmation)
    {
      co_return;
    }

    if (cancelled || !previewRes)
    {
      // Nothing can be asked without a preview, so the waiting question
      // closes; a runtime failure reports its own wording.
      statePtr->optDeleteConfirmation.reset();

      if (!cancelled)
      {
        statePtr->post(rt::NotificationSeverity::Error, previewRes.error().message);
      }

      statePtr->requestRefresh();
      co_return;
    }

    statePtr->presentDeletePreview(std::move(previewRes));
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

      entries.append("• ").append(ellipsizeToCellWidth(confirmation.deletedListNames[index], entryColumns));
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

  ftxui::Elements ListAuthoringController::renderDeleteQuestion(ListDeleteConfirmation const& confirmation,
                                                                std::int32_t const columns,
                                                                std::int32_t const rowBudget) const
  {
    using namespace ftxui;

    auto& state = *_statePtr;
    auto rows = Elements{};

    if (!confirmation.previewReady)
    {
      // The runtime is still answering, and the question has nothing
      // truthful to say about the scope yet.
      rows.push_back(text(std::string{kCellEllipsis}) | style::muted());
      return rows;
    }

    // The tag offer and the reference warning are laid out first, because the
    // subtree entries are the part a short terminal can afford to shorten.
    auto tagRows = Elements{};

    if (!confirmation.tagImpactQuestion.empty())
    {
      auto const marker = std::string{confirmation.removeWritableTag ? "[x] " : "[ ] "};
      auto const markerColumns = cellWidth(marker);
      auto const lines = wrapCellText(confirmation.tagImpactQuestion, std::max(1, columns - markerColumns));
      tagRows.push_back(text(""));

      for (std::size_t index = 0; index < lines.size(); ++index)
      {
        tagRows.push_back(hbox({
          text(index == 0 ? marker : std::string(static_cast<std::size_t>(markerColumns), ' ')) | bold,
          text(lines[index]),
        }));
      }
    }

    if (!confirmation.tagReferencesWarning.empty())
    {
      tagRows.push_back(text(""));
      appendWrappedRows(tagRows, confirmation.tagReferencesWarning, columns, style::warning());
    }

    if (confirmation.includeDescendants)
    {
      auto const questionFor = [&](std::string const& entries)
      {
        return i18n::requiredFormat(state.textCatalog,
                                    MessageId::ListDeleteSubtreeQuestion,
                                    {{"count", confirmation.deletedListNames.size()}, {"entries", entries}});
      };

      // The shared pattern's own lines around its entries, measured with one
      // empty entry row standing in for the list.
      auto const surroundingRows = static_cast<std::int32_t>(wrapCellText(questionFor({}), columns).size()) - 1;
      auto const entryBudget = rowBudget - surroundingRows - static_cast<std::int32_t>(tagRows.size());
      appendWrappedRows(rows, questionFor(buildSubtreeEntries(confirmation, entryBudget, columns - 2)), columns);
    }
    else
    {
      appendWrappedRows(rows, confirmation.question, columns);
    }

    for (auto& rowPtr : tagRows)
    {
      rows.push_back(std::move(rowPtr));
    }

    return rows;
  }

  ftxui::Element ListAuthoringController::renderDeleteConfirmation(ListDeleteConfirmation const& confirmation,
                                                                   std::int32_t const terminalColumns,
                                                                   std::int32_t const terminalRows) const
  {
    using namespace ftxui;

    auto& state = *_statePtr;
    state.confirmationMouseBindings.clear();
    state.confirmationMouseReady = true;
    // Drawing the filled question is what makes it answerable by key.
    state.confirmationKeysReady = confirmation.previewReady;

    // The popover keeps one cleared cell around itself, the same halo the
    // command palette uses, so the workspace's borders never join its own.
    auto const panelColumns = std::max(1, std::min(terminalColumns - 2, std::clamp(terminalColumns - 4, 60, 80)));
    auto const bodyColumns = std::max(1, style::popupPanelBodyColumns(panelColumns));
    auto const availableRows = std::max(1, terminalRows - 2);
    // Border, the spacer above the footer, and the footer itself.
    constexpr std::int32_t kChromeRows = 4;

    auto questionRows = renderDeleteQuestion(confirmation, bodyColumns, availableRows - kChromeRows);
    auto footer = Elements{};

    if (confirmation.deleting)
    {
      footer.push_back(text(std::string{i18n::requiredText(state.textCatalog, MessageId::TuiListStatusDeleting)}) |
                       bold);
    }
    else
    {
      if (confirmation.previewReady)
      {
        // The destructive answer carries the danger color, and Escape is named
        // for what it does: nothing is deleted.
        footer.push_back(state.confirmationMouseBindings.bind(
          style::shortcutChip(
            "Enter",
            std::string{i18n::requiredText(
              state.textCatalog,
              confirmation.includeDescendants ? MessageId::ListDeleteAllAction : MessageId::ListDeleteAction)}) |
            style::danger(),
          Event::Return));
        footer.push_back(style::mutedSeparator());

        if (!confirmation.tagImpactQuestion.empty())
        {
          footer.push_back(state.confirmationMouseBindings.bind(
            style::shortcutChip("Space", i18n::requiredText(state.textCatalog, MessageId::TuiEditorHintToggle)),
            Event::Character(" ")));
          footer.push_back(style::mutedSeparator());
        }
      }

      footer.push_back(state.confirmationMouseBindings.bind(
        style::shortcutChip("Esc", i18n::requiredText(state.textCatalog, MessageId::TuiEditorHintCancel)),
        Event::Escape));
    }

    // The question shrinks before the footer does, so the answer keys stay
    // on screen even when a tiny terminal cannot show the whole question.
    auto panelPtr = style::popupPanel(confirmation.title,
                                      vbox({
                                        vbox(std::move(questionRows)) | yframe | yflex_shrink,
                                        text(""),
                                        hbox(std::move(footer)),
                                      })) |
                    size(WIDTH, EQUAL, panelColumns) | size(HEIGHT, LESS_THAN, availableRows);

    return centerPopover(std::move(panelPtr));
  }
} // namespace ao::tui
