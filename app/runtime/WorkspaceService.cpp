// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "WorkspaceSessionCodec.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/rt/NavigationHistory.h>
#include <ao/rt/Signal.h>
#include <ao/rt/Subscription.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSessionState.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/LibraryChanges.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt
{
  namespace
  {
    struct ResolvedNavigationTarget final
    {
      ListId listId = kInvalidListId;
      std::string filterExpression{};
      bool reusePlainView = false;
    };

    [[noreturn]] void failExecutorAffinity(std::source_location const& location)
    {
      APP_LOG_CRITICAL("WorkspaceService thread-affinity violation: '{}' invoked off the executor thread ({}:{})",
                       location.function_name(),
                       location.file_name(),
                       location.line());

      if (auto const& loggerPtr = Log::appLogger(); loggerPtr)
      {
        loggerPtr->flush();
      }

      std::abort();
    }

    Result<ResolvedNavigationTarget> resolveNavigationTarget(NavigationTarget const& target)
    {
      if (auto const* listId = std::get_if<ListId>(&target); listId != nullptr)
      {
        return ResolvedNavigationTarget{.listId = *listId, .reusePlainView = true};
      }

      if (auto const* filtered = std::get_if<FilteredListTarget>(&target); filtered != nullptr)
      {
        return ResolvedNavigationTarget{.listId = filtered->listId, .filterExpression = filtered->filterExpression};
      }

      if (std::get<GlobalViewKind>(target) == GlobalViewKind::AllTracks)
      {
        return ResolvedNavigationTarget{.listId = ListId{kAllTracksListId}, .reusePlainView = true};
      }

      return makeError(Error::Code::InvalidInput, "Unsupported workspace navigation target");
    }
  } // namespace

  struct WorkspaceService::Impl final
  {
    struct PendingCommit final
    {
      WorkspaceCommitReceipt receipt{};
      WorkspaceChanged changed{};
    };

    async::Executor& executor;
    ViewService& views;
    WorkspaceSnapshot currentSnapshot;
    NavigationHistory navigationHistory;
    std::shared_ptr<Signal<WorkspaceChanged const&>> changedSignalPtr =
      std::make_shared<Signal<WorkspaceChanged const&>>();
    Subscription listsMutatedSub;

    Impl(async::Executor& callbackExecutor, ViewService& viewService, LibraryChanges const& changes)
      : executor{callbackExecutor}, views{viewService}
    {
      listsMutatedSub =
        changes.onChanged([this](LibraryChangeSet const& changeSet) { handleLibraryChange(changeSet); });
    }

    ~Impl() { changedSignalPtr->disconnectAll(); }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void ensureOnExecutor(std::source_location location = std::source_location::current()) const
    {
      if (!executor.isCurrent()) [[unlikely]]
      {
        failExecutorAffinity(location);
      }
    }

    WorkspaceCommitReceipt noChangeReceipt() const
    {
      return WorkspaceCommitReceipt{
        .disposition = WorkspaceCommitDisposition::NoChange,
        .beforeRevision = currentSnapshot.revision,
        .afterRevision = currentSnapshot.revision,
        .activeViewId = currentSnapshot.activeViewId,
      };
    }

    PendingCommit installCandidate(WorkspaceSnapshot nextSnapshot,
                                   NavigationHistory nextHistory,
                                   WorkspaceChangeCause const cause)
    {
      auto const beforeRevision = currentSnapshot.revision;
      nextSnapshot.navigation = NavigationAvailability{
        .canGoBack = nextHistory.canGoBack(),
        .canGoForward = nextHistory.canGoForward(),
      };
      nextSnapshot.revision = beforeRevision + 1;

      auto pending = PendingCommit{
        .receipt =
          WorkspaceCommitReceipt{
            .disposition = WorkspaceCommitDisposition::Applied,
            .beforeRevision = beforeRevision,
            .afterRevision = nextSnapshot.revision,
            .activeViewId = nextSnapshot.activeViewId,
          },
        .changed = WorkspaceChanged{.snapshot = nextSnapshot, .cause = cause},
      };

      currentSnapshot = std::move(nextSnapshot);
      navigationHistory = std::move(nextHistory);
      return pending;
    }

    static void logFailure(std::string_view const message) noexcept
    {
      try
      {
        APP_LOG_ERROR("{}", message);
      }
      catch (...) // NOLINT(bugprone-empty-catch) -- observation delivery must remain noexcept
      {
      }
    }

    static void logFailure(std::string_view const message, std::string_view const detail) noexcept
    {
      try
      {
        APP_LOG_ERROR("{}: {}", message, detail);
      }
      catch (...) // NOLINT(bugprone-empty-catch) -- observation delivery must remain noexcept
      {
      }
    }

    static void emitChange(std::weak_ptr<Signal<WorkspaceChanged const&>> const& weakSignalPtr,
                           WorkspaceChanged const& changed) noexcept
    {
      auto signalPtr = weakSignalPtr.lock();

      if (!signalPtr)
      {
        return;
      }

      try
      {
        signalPtr->emit(changed);
      }
      catch (std::exception const& error)
      {
        logFailure("Workspace observer threw", error.what());
      }
      catch (...)
      {
        logFailure("Workspace observer threw an unknown exception");
      }
    }

    void publish(PendingCommit pending) noexcept
    {
      auto weakSignalPtr = std::weak_ptr<Signal<WorkspaceChanged const&>>{changedSignalPtr};

      try
      {
        executor.defer([weakSignalPtr, changed = std::move(pending.changed)] noexcept
                       { emitChange(weakSignalPtr, changed); });
      }
      catch (std::exception const& error)
      {
        logFailure("Failed to queue workspace observation", error.what());
      }
      catch (...)
      {
        logFailure("Failed to queue workspace observation");
      }
    }

    WorkspaceCommitReceipt commitCandidate(WorkspaceSnapshot nextSnapshot,
                                           NavigationHistory nextHistory,
                                           WorkspaceChangeCause const cause)
    {
      auto pending = installCandidate(std::move(nextSnapshot), std::move(nextHistory), cause);
      auto const receipt = pending.receipt;
      publish(std::move(pending));
      return receipt;
    }

    Result<TrackListViewState> liveViewState(ViewId const viewId) const
    {
      if (viewId == kInvalidViewId)
      {
        return makeError(Error::Code::InvalidInput, "The invalid view id cannot identify a live workspace view");
      }

      try
      {
        auto state = views.trackListState(viewId);

        if (state.lifecycle == ViewLifecycleState::Destroyed)
        {
          return makeError(Error::Code::InvalidState, std::format("View {} is destroyed", viewId));
        }

        return state;
      }
      catch (std::out_of_range const&)
      {
        return makeError(Error::Code::NotFound, std::format("View {} does not exist", viewId));
      }
    }

    std::optional<ViewId> reusableView(ResolvedNavigationTarget const& target) const
    {
      if (!target.reusePlainView)
      {
        return std::nullopt;
      }

      for (auto const& record : views.listViews())
      {
        if (record.kind != ViewKind::TrackList)
        {
          continue;
        }

        if (auto const state = views.trackListState(record.id);
            state.listId == target.listId && state.filterExpression.empty())
        {
          return record.id;
        }
      }

      return std::nullopt;
    }

    static NavigationPoint navigationPoint(TrackListViewState const& state, TrackPresentationSpec const& presentation)
    {
      return NavigationPoint{
        .listId = state.listId,
        .filterExpression = state.filterExpression,
        .presentation = presentation,
      };
    }

    std::optional<TrackPresentationSpec> presentationForId(std::string_view const id) const
    {
      if (auto const* preset = builtinTrackPresentationPreset(id); preset != nullptr)
      {
        return preset->spec;
      }

      for (auto const& custom : currentSnapshot.customPresets)
      {
        if (custom.spec.id == id)
        {
          return custom.spec;
        }
      }

      return std::nullopt;
    }

    Result<WorkspaceCommitReceipt> navigateToReusableView(ViewId const viewId, NavigationOptions const& options)
    {
      auto stateResult = liveViewState(viewId);

      if (!stateResult)
      {
        return std::unexpected{stateResult.error()};
      }

      auto const& state = *stateResult;
      auto const presentation =
        options.optPresentation ? normalizeTrackPresentationSpec(*options.optPresentation) : state.presentation;
      auto nextSnapshot = currentSnapshot;
      auto nextHistory = navigationHistory;
      bool changed = false;

      if (!std::ranges::contains(nextSnapshot.openViews, viewId))
      {
        nextSnapshot.openViews.push_back(viewId);
        changed = true;
      }

      if (nextSnapshot.activeViewId != viewId)
      {
        nextSnapshot.activeViewId = viewId;
        changed = true;
      }

      auto const presentationChanged = state.presentation != presentation;
      changed = changed || presentationChanged;

      if (options.recordHistory)
      {
        changed = nextHistory.commit(navigationPoint(state, presentation)) || changed;
      }

      if (!changed)
      {
        return noChangeReceipt();
      }

      if (presentationChanged)
      {
        if (auto result = views.setPresentation(viewId, presentation); !result)
        {
          return std::unexpected{result.error()};
        }
      }

      APP_LOG_DEBUG("WorkspaceService: Navigating to viewId: {}", viewId.raw());
      return commitCandidate(std::move(nextSnapshot), std::move(nextHistory), WorkspaceChangeCause::Navigation);
    }

    Result<WorkspaceCommitReceipt> navigate(NavigationTarget const& target, NavigationOptions const& options)
    {
      auto targetResult = resolveNavigationTarget(target);

      if (!targetResult)
      {
        return std::unexpected{targetResult.error()};
      }

      if (auto const optViewId = reusableView(*targetResult); optViewId)
      {
        return navigateToReusableView(*optViewId, options);
      }

      auto nextSnapshot = currentSnapshot;
      nextSnapshot.openViews.reserve(nextSnapshot.openViews.size() + 1);
      auto nextHistory = navigationHistory;
      auto config = TrackListViewConfig{
        .listId = targetResult->listId,
        .filterExpression = targetResult->filterExpression,
      };
      config.optPresentation = options.optPresentation;
      auto viewResult = views.createView(config, true);

      if (!viewResult)
      {
        return std::unexpected{viewResult.error()};
      }

      auto const viewId = viewResult->viewId;
      auto const state = views.trackListState(viewId);
      nextSnapshot.openViews.push_back(viewId);
      nextSnapshot.activeViewId = viewId;

      if (options.recordHistory)
      {
        std::ignore = nextHistory.commit(navigationPoint(state, state.presentation));
      }

      APP_LOG_DEBUG("WorkspaceService: Navigating to viewId: {}", viewId.raw());
      return commitCandidate(std::move(nextSnapshot), std::move(nextHistory), WorkspaceChangeCause::Navigation);
    }

    Result<WorkspaceCommitReceipt> focus(ViewId const viewId)
    {
      if (viewId == kInvalidViewId || !std::ranges::contains(currentSnapshot.openViews, viewId))
      {
        return makeError(Error::Code::InvalidInput, std::format("View {} is not open in this workspace", viewId));
      }

      if (auto state = liveViewState(viewId); !state)
      {
        return std::unexpected{state.error()};
      }

      if (currentSnapshot.activeViewId == viewId)
      {
        return noChangeReceipt();
      }

      auto nextSnapshot = currentSnapshot;
      nextSnapshot.activeViewId = viewId;
      return commitCandidate(std::move(nextSnapshot), navigationHistory, WorkspaceChangeCause::Focus);
    }

    Result<WorkspaceCommitReceipt> closeViews(std::span<ViewId const> viewIds, WorkspaceChangeCause const cause)
    {
      if (viewIds.empty())
      {
        return noChangeReceipt();
      }

      auto nextSnapshot = currentSnapshot;
      std::erase_if(
        nextSnapshot.openViews, [&](ViewId const viewId) { return std::ranges::contains(viewIds, viewId); });

      if (std::ranges::contains(viewIds, nextSnapshot.activeViewId))
      {
        nextSnapshot.activeViewId = nextSnapshot.openViews.empty() ? kInvalidViewId : nextSnapshot.openViews.back();
      }

      auto const receipt = commitCandidate(std::move(nextSnapshot), navigationHistory, cause);

      for (auto const viewId : viewIds)
      {
        if (auto result = views.destroyView(viewId); !result)
        {
          APP_LOG_ERROR("Failed to destroy view {} after workspace close: {}", viewId.raw(), result.error().message);
        }
      }

      return receipt;
    }

    Result<WorkspaceCommitReceipt> close(ViewId const viewId)
    {
      if (!std::ranges::contains(currentSnapshot.openViews, viewId))
      {
        return noChangeReceipt();
      }

      if (auto state = liveViewState(viewId); !state)
      {
        return std::unexpected{state.error()};
      }

      auto const ids = std::array{viewId};
      return closeViews(ids, WorkspaceChangeCause::Close);
    }

    Result<WorkspaceCommitReceipt> applyPresentation(TrackPresentationSpec const& requested,
                                                     NavigationOptions const& options)
    {
      auto const viewId = currentSnapshot.activeViewId;

      if (viewId == kInvalidViewId)
      {
        return makeError(Error::Code::InvalidState, "No workspace view is focused");
      }

      auto stateResult = liveViewState(viewId);

      if (!stateResult)
      {
        return std::unexpected{stateResult.error()};
      }

      auto const presentation = normalizeTrackPresentationSpec(requested);
      auto nextHistory = navigationHistory;
      auto changed = stateResult->presentation != presentation;

      if (options.recordHistory)
      {
        changed = nextHistory.commit(navigationPoint(*stateResult, presentation)) || changed;
      }

      if (!changed)
      {
        return noChangeReceipt();
      }

      if (stateResult->presentation != presentation)
      {
        if (auto result = views.setPresentation(viewId, presentation); !result)
        {
          return std::unexpected{result.error()};
        }
      }

      return commitCandidate(currentSnapshot, std::move(nextHistory), WorkspaceChangeCause::Presentation);
    }

    Result<WorkspaceCommitReceipt> restoreNavigationPoint(NavigationPoint const& point, NavigationHistory nextHistory)
    {
      auto matchingViewId = kInvalidViewId;

      for (auto const& record : views.listViews())
      {
        if (record.kind != ViewKind::TrackList)
        {
          continue;
        }

        if (auto const state = views.trackListState(record.id);
            state.listId == point.listId && state.filterExpression == point.filterExpression)
        {
          matchingViewId = record.id;
          break;
        }
      }

      auto nextSnapshot = currentSnapshot;
      nextSnapshot.openViews.reserve(nextSnapshot.openViews.size() + 1);

      if (matchingViewId == kInvalidViewId)
      {
        auto result = views.createView(
          TrackListViewConfig{
            .listId = point.listId,
            .filterExpression = point.filterExpression,
            .optPresentation = point.presentation,
          },
          true);

        if (!result)
        {
          return std::unexpected{result.error()};
        }

        matchingViewId = result->viewId;
      }
      else
      {
        if (auto const state = views.trackListState(matchingViewId); state.presentation != point.presentation)
        {
          if (auto result = views.setPresentation(matchingViewId, point.presentation); !result)
          {
            return std::unexpected{result.error()};
          }
        }
      }

      if (!std::ranges::contains(nextSnapshot.openViews, matchingViewId))
      {
        nextSnapshot.openViews.push_back(matchingViewId);
      }

      nextSnapshot.activeViewId = matchingViewId;
      return commitCandidate(std::move(nextSnapshot), std::move(nextHistory), WorkspaceChangeCause::Navigation);
    }

    Result<WorkspaceCommitReceipt> goBack()
    {
      auto nextHistory = navigationHistory;
      auto optPoint = nextHistory.back();

      if (!optPoint)
      {
        return makeError(Error::Code::NotFound, "Workspace navigation history has no previous entry");
      }

      return restoreNavigationPoint(*optPoint, std::move(nextHistory));
    }

    Result<WorkspaceCommitReceipt> goForward()
    {
      auto nextHistory = navigationHistory;
      auto optPoint = nextHistory.forward();

      if (!optPoint)
      {
        return makeError(Error::Code::NotFound, "Workspace navigation history has no forward entry");
      }

      return restoreNavigationPoint(*optPoint, std::move(nextHistory));
    }

    Result<WorkspaceCommitReceipt> addPreset(CustomTrackPresentationPreset const& preset)
    {
      auto nextSnapshot = currentSnapshot;
      auto const it = std::ranges::find_if(
        nextSnapshot.customPresets, [&](auto const& existing) { return existing.label == preset.label; });

      if (it != nextSnapshot.customPresets.end())
      {
        if (*it == preset)
        {
          return noChangeReceipt();
        }

        *it = preset;
      }
      else
      {
        nextSnapshot.customPresets.push_back(preset);
      }

      return commitCandidate(std::move(nextSnapshot), navigationHistory, WorkspaceChangeCause::Presets);
    }

    Result<WorkspaceCommitReceipt> removePreset(std::string_view const presetId)
    {
      auto nextSnapshot = currentSnapshot;
      auto const removed = std::erase_if(
        nextSnapshot.customPresets, [presetId](auto const& preset) { return preset.spec.id == presetId; });

      if (removed == 0)
      {
        return noChangeReceipt();
      }

      return commitCandidate(std::move(nextSnapshot), navigationHistory, WorkspaceChangeCause::Presets);
    }

    void handleLibraryChange(LibraryChangeSet const& changeSet)
    {
      ensureOnExecutor();
      auto toClose = std::vector<ViewId>{};

      for (auto const listId : changeSet.listsDeleted)
      {
        for (auto const viewId : currentSnapshot.openViews)
        {
          if (auto const state = views.trackListState(viewId);
              state.listId == listId && !std::ranges::contains(toClose, viewId))
          {
            toClose.push_back(viewId);
          }
        }
      }

      if (!toClose.empty())
      {
        std::ignore = closeViews(toClose, WorkspaceChangeCause::ListDeletion);
      }
    }
  };

  WorkspaceService::WorkspaceService(async::Executor& executor, ViewService& views, LibraryChanges const& changes)
    : _implPtr{std::make_unique<Impl>(executor, views, changes)}
  {
  }

  WorkspaceService::~WorkspaceService() = default;

  WorkspaceSnapshot WorkspaceService::snapshot() const
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->currentSnapshot;
  }

  Result<WorkspaceCommitReceipt> WorkspaceService::focusView(ViewId const viewId)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->focus(viewId);
  }

  Result<WorkspaceCommitReceipt> WorkspaceService::navigateTo(NavigationTarget const& target,
                                                              NavigationOptions const options)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->navigate(target, options);
  }

  Result<WorkspaceCommitReceipt> WorkspaceService::closeView(ViewId const viewId)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->close(viewId);
  }

  Result<WorkspaceCommitReceipt> WorkspaceService::setActivePresentation(TrackPresentationSpec const& presentation,
                                                                         NavigationOptions const options)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->applyPresentation(presentation, options);
  }

  Result<WorkspacePresentationReceipt> WorkspaceService::setActivePresentation(std::string_view const presentationId,
                                                                               NavigationOptions const options)
  {
    _implPtr->ensureOnExecutor();
    auto const optPresentation = _implPtr->presentationForId(presentationId);

    if (!optPresentation)
    {
      return makeError(Error::Code::NotFound, std::format("Unknown track presentation '{}'", presentationId));
    }

    auto result = _implPtr->applyPresentation(*optPresentation, options);

    if (!result)
    {
      return std::unexpected{result.error()};
    }

    return WorkspacePresentationReceipt{
      .presentation = normalizeTrackPresentationSpec(*optPresentation),
      .commit = *result,
    };
  }

  Result<WorkspaceCommitReceipt> WorkspaceService::goBack()
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->goBack();
  }

  Result<WorkspaceCommitReceipt> WorkspaceService::goForward()
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->goForward();
  }

  bool WorkspaceService::canGoBack() const
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->navigationHistory.canGoBack();
  }

  bool WorkspaceService::canGoForward() const
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->navigationHistory.canGoForward();
  }

  Subscription WorkspaceService::onChanged(std::move_only_function<void(WorkspaceChanged const&)> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->changedSignalPtr->connect(std::move(handler));
  }

  std::span<CustomTrackPresentationPreset const> WorkspaceService::customPresets() const
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->currentSnapshot.customPresets;
  }

  Result<WorkspaceCommitReceipt> WorkspaceService::addCustomPreset(CustomTrackPresentationPreset const& preset)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->addPreset(preset);
  }

  Result<WorkspaceCommitReceipt> WorkspaceService::removeCustomPreset(std::string_view const presetId)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->removePreset(presetId);
  }

  void WorkspaceService::saveSession(ConfigStore& store) const
  {
    _implPtr->ensureOnExecutor();
    auto const snapshotValue = _implPtr->currentSnapshot;
    auto state = WorkspaceSessionState{};
    state.customPresets = snapshotValue.customPresets;

    for (auto const viewId : snapshotValue.openViews)
    {
      auto const viewState = _implPtr->views.trackListState(viewId);

      if (viewId == snapshotValue.activeViewId)
      {
        state.activeListId = viewState.listId;
      }

      state.openViews.push_back(TrackListViewConfig{
        .listId = viewState.listId,
        .filterExpression = viewState.filterExpression,
        .groupBy = viewState.groupBy,
        .sortBy = viewState.sortBy,
        .optPresentation = viewState.presentation,
      });
    }

    auto document = detail::encodeWorkspaceSession(state);

    if (!document)
    {
      APP_LOG_ERROR("WorkspaceService: Failed to encode session - {}", document.error().message);
      return;
    }

    if (auto const result = store.save("workspace", *document); !result)
    {
      APP_LOG_ERROR("WorkspaceService: Failed to save session - {}", result.error().message);
    }
  }

  Result<WorkspaceCommitReceipt> WorkspaceService::restoreSession(ConfigStore& store)
  {
    _implPtr->ensureOnExecutor();
    auto const containsWorkspace = store.contains("workspace");

    if (!containsWorkspace)
    {
      if (containsWorkspace.error().code == Error::Code::NotFound)
      {
        return _implPtr->noChangeReceipt();
      }

      return std::unexpected{containsWorkspace.error()};
    }

    if (!*containsWorkspace)
    {
      return _implPtr->noChangeReceipt();
    }

    auto document = detail::WorkspaceSessionDocument{};

    if (auto const result = store.loadExact("workspace", document); !result)
    {
      return std::unexpected{result.error()};
    }

    auto stateResult = detail::decodeWorkspaceSession(document);

    if (!stateResult)
    {
      return std::unexpected{stateResult.error()};
    }

    auto state = std::move(*stateResult);

    auto createdViewIds = std::vector<ViewId>{};
    createdViewIds.reserve(state.openViews.size());

    for (auto const& viewConfig : state.openViews)
    {
      auto result = _implPtr->views.createView(viewConfig, true);

      if (!result)
      {
        for (auto const viewId : createdViewIds)
        {
          std::ignore = _implPtr->views.destroyView(viewId);
        }

        return std::unexpected{result.error()};
      }

      createdViewIds.push_back(result->viewId);
    }

    auto nextSnapshot = _implPtr->currentSnapshot;
    nextSnapshot.openViews.reserve(nextSnapshot.openViews.size() + createdViewIds.size());
    nextSnapshot.openViews.insert(nextSnapshot.openViews.end(), createdViewIds.begin(), createdViewIds.end());
    nextSnapshot.customPresets = std::move(state.customPresets);
    auto focused = kInvalidViewId;

    for (auto const viewId : createdViewIds)
    {
      if (auto const viewState = _implPtr->views.trackListState(viewId); viewState.listId == state.activeListId)
      {
        focused = viewId;

        if (!viewState.filterExpression.empty())
        {
          break;
        }
      }
    }

    if (focused == kInvalidViewId && !nextSnapshot.openViews.empty())
    {
      focused = nextSnapshot.openViews.front();
    }

    nextSnapshot.activeViewId = focused;
    auto nextHistory = _implPtr->navigationHistory;
    bool historyChanged = false;

    if (focused != kInvalidViewId)
    {
      auto const viewState = _implPtr->views.trackListState(focused);
      historyChanged = nextHistory.commit(Impl::navigationPoint(viewState, viewState.presentation));
    }

    auto const aggregateChanged = !createdViewIds.empty() ||
                                  nextSnapshot.customPresets != _implPtr->currentSnapshot.customPresets ||
                                  nextSnapshot.activeViewId != _implPtr->currentSnapshot.activeViewId;

    if (!aggregateChanged && !historyChanged)
    {
      return _implPtr->noChangeReceipt();
    }

    return _implPtr->commitCandidate(std::move(nextSnapshot), std::move(nextHistory), WorkspaceChangeCause::Restore);
  }
} // namespace ao::rt
