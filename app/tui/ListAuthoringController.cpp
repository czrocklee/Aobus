// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "ListAuthoringController.h"

#include "MouseBindings.h"
#include "Render.h"
#include "SmartListEditor.h"
#include "Style.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
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
#include <format>
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

    /// Everything one preview recomputation produced for the open editor.
    struct ListPreview final
    {
      uimodel::SmartListEditorViewState viewState{};
      std::vector<std::string> tracks{};
    };

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
    bool submissionPending = false;
    std::uint64_t previewGeneration = 0;
    async::TaskHandle previewTask{};
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
    _statePtr->previewTask.reset();
    ++_statePtr->previewGeneration;
    _statePtr->outputs = Outputs{};
  }

  bool ListAuthoringController::tryOpenNew(ListId const targetListId)
  {
    auto& state = *_statePtr;
    state.expectCallbackExecutor();

    if (state.retired || state.optEditor || state.submissionPending)
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

    if (state.retired || state.optEditor || state.submissionPending)
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

  SmartListEditor const* ListAuthoringController::activeEditor() const noexcept
  {
    return _statePtr->optEditor ? &*_statePtr->optEditor : nullptr;
  }

  bool ListAuthoringController::isActive() const noexcept
  {
    return _statePtr->optEditor.has_value();
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

    return nullptr;
  }
} // namespace ao::tui
