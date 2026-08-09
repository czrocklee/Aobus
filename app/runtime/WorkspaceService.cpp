// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/WorkspaceService.h>

#include "WorkspaceSessionYamlSchema.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/rt/NavigationHistory.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceSessionState.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/utility/StrongTypeFormatter.h>

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
      WorkspaceSnapshot snapshot{};
      NavigationHistory history{};
      WorkspaceChanged changed{};
    };

    async::Executor& executor;
    ViewService& views;
    LibraryChanges const& changes;
    WorkspaceSnapshot currentSnapshot;
    NavigationHistory navigationHistory;
    async::Signal<WorkspaceChanged const&> changedSignal;
    async::Subscription listsMutatedSub;

    Impl(async::Executor& callbackExecutor, ViewService& viewService, LibraryChanges const& changes)
      : executor{callbackExecutor}, views{viewService}, changes{changes}
    {
      listsMutatedSub =
        changes.onChanged([this](LibraryChangeSet const& changeSet) { handleLibraryChange(changeSet); });
    }

    ~Impl() { changedSignal.disconnectAll(); }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void ensureOnExecutor(std::source_location location = std::source_location::current()) const
    {
      AO_EXPECTS_AT(location, executor.isCurrent(), "WorkspaceService invoked off the executor thread");
    }

    PendingCommit prepareCommit(WorkspaceSnapshot nextSnapshot,
                                NavigationHistory nextHistory,
                                WorkspaceChangeCause const cause) const
    {
      nextSnapshot.revision = currentSnapshot.revision + 1;
      auto changed = WorkspaceChanged{.snapshot = nextSnapshot, .cause = cause};

      return PendingCommit{
        .snapshot = std::move(nextSnapshot),
        .history = std::move(nextHistory),
        .changed = std::move(changed),
      };
    }

    void publish(WorkspaceChanged changed) noexcept
    {
      try
      {
        // Signal::post owns the weak-state dance and the non-throwing delivery
        // boundary after admission.
        changedSignal.post(executor, std::move(changed));
      }
      catch (...)
      {
        AO_FATAL_EXCEPTION(std::current_exception(), "workspace observation admission");
      }
    }

    void installCommit(PendingCommit pending) noexcept
    {
      currentSnapshot = std::move(pending.snapshot);
      navigationHistory = std::move(pending.history);
      publish(std::move(pending.changed));
    }

    void commitCandidate(WorkspaceSnapshot nextSnapshot,
                         NavigationHistory nextHistory,
                         WorkspaceChangeCause const cause)
    {
      installCommit(prepareCommit(std::move(nextSnapshot), std::move(nextHistory), cause));
    }

    Result<TrackListViewState> liveViewState(ViewId const viewId) const
    {
      if (viewId == kInvalidViewId)
      {
        return makeError(Error::Code::InvalidInput, "The invalid view id cannot identify a live workspace view");
      }

      return views.findTrackListState(viewId);
    }

    std::optional<ViewId> reusableView(ResolvedNavigationTarget const& target) const
    {
      if (!target.reusePlainView)
      {
        return std::nullopt;
      }

      for (auto const viewId : views.listViews())
      {
        if (auto const state = views.trackListState(viewId);
            state.listId == target.listId && state.filterExpression.empty())
        {
          return viewId;
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

    Result<ViewId> navigateToReusableView(ViewId const viewId, NavigationRequest const& request)
    {
      auto stateRes = liveViewState(viewId);

      if (!stateRes)
      {
        return std::unexpected{stateRes.error()};
      }

      auto const& state = *stateRes;
      auto presentation = state.presentation;

      if (request.optPresentation && request.optPresentation->mode == NavigationPresentationMode::Override)
      {
        presentation = normalizeTrackPresentationSpec(request.optPresentation->spec);
      }

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

      if (request.recordHistory)
      {
        changed = nextHistory.commit(navigationPoint(state, presentation)) || changed;
      }

      if (!changed)
      {
        return viewId;
      }

      auto pending = prepareCommit(std::move(nextSnapshot), std::move(nextHistory), WorkspaceChangeCause::Navigation);

      if (presentationChanged)
      {
        if (auto result = views.setPresentation(viewId, presentation); !result)
        {
          return std::unexpected{result.error()};
        }
      }

      installCommit(std::move(pending));
      return viewId;
    }

    Result<ViewId> navigate(NavigationRequest const& request)
    {
      auto targetRes = resolveNavigationTarget(request.target);

      if (!targetRes)
      {
        return std::unexpected{targetRes.error()};
      }

      if (auto const optViewId = reusableView(*targetRes); optViewId)
      {
        return navigateToReusableView(*optViewId, request);
      }

      auto nextSnapshot = currentSnapshot;
      nextSnapshot.openViews.reserve(nextSnapshot.openViews.size() + 1);
      auto nextHistory = navigationHistory;
      auto config = TrackListViewConfig{
        .listId = targetRes->listId,
        .filterExpression = targetRes->filterExpression,
      };

      if (request.optPresentation)
      {
        config.optPresentation = request.optPresentation->spec;
      }

      auto viewRes = views.createView(config);

      if (!viewRes)
      {
        return std::unexpected{viewRes.error()};
      }

      auto const viewId = *viewRes;

      try
      {
        auto const state = views.trackListState(viewId);
        nextSnapshot.openViews.push_back(viewId);
        nextSnapshot.activeViewId = viewId;

        if (request.recordHistory)
        {
          std::ignore = nextHistory.commit(navigationPoint(state, state.presentation));
        }

        installCommit(prepareCommit(std::move(nextSnapshot), std::move(nextHistory), WorkspaceChangeCause::Navigation));
      }
      catch (...)
      {
        views.destroyView(viewId);
        throw;
      }

      return viewId;
    }

    Result<> focus(ViewId const viewId)
    {
      if (viewId == kInvalidViewId || !std::ranges::contains(currentSnapshot.openViews, viewId))
      {
        return makeError(Error::Code::InvalidInput, std::format("View {} is not open in this workspace", viewId));
      }

      if (auto stateRes = liveViewState(viewId); !stateRes)
      {
        return std::unexpected{stateRes.error()};
      }

      if (currentSnapshot.activeViewId == viewId)
      {
        return {};
      }

      auto nextSnapshot = currentSnapshot;
      nextSnapshot.activeViewId = viewId;
      commitCandidate(std::move(nextSnapshot), navigationHistory, WorkspaceChangeCause::Focus);
      return {};
    }

    Result<> closeViews(std::span<ViewId const> viewIds, WorkspaceChangeCause const cause)
    {
      if (viewIds.empty())
      {
        return {};
      }

      auto nextSnapshot = currentSnapshot;
      std::erase_if(
        nextSnapshot.openViews, [&](ViewId const viewId) { return std::ranges::contains(viewIds, viewId); });

      if (std::ranges::contains(viewIds, nextSnapshot.activeViewId))
      {
        nextSnapshot.activeViewId = nextSnapshot.openViews.empty() ? kInvalidViewId : nextSnapshot.openViews.back();
      }

      commitCandidate(std::move(nextSnapshot), navigationHistory, cause);

      for (auto const viewId : viewIds)
      {
        views.destroyView(viewId);
      }

      return {};
    }

    Result<> close(ViewId const viewId)
    {
      if (!std::ranges::contains(currentSnapshot.openViews, viewId))
      {
        return {};
      }

      if (auto stateRes = liveViewState(viewId); !stateRes)
      {
        return std::unexpected{stateRes.error()};
      }

      auto const ids = std::array{viewId};
      return closeViews(ids, WorkspaceChangeCause::Close);
    }

    Result<> applyPresentation(TrackPresentationSpec const& requested, PresentationChangeOptions const& options)
    {
      auto const viewId = currentSnapshot.activeViewId;

      if (viewId == kInvalidViewId)
      {
        return makeError(Error::Code::InvalidState, "No workspace view is focused");
      }

      auto stateRes = liveViewState(viewId);

      if (!stateRes)
      {
        return std::unexpected{stateRes.error()};
      }

      auto const presentation = normalizeTrackPresentationSpec(requested);
      auto nextHistory = navigationHistory;
      auto changed = stateRes->presentation != presentation;

      if (options.recordHistory)
      {
        changed = nextHistory.commit(navigationPoint(*stateRes, presentation)) || changed;
      }

      if (!changed)
      {
        return {};
      }

      auto pending = prepareCommit(currentSnapshot, std::move(nextHistory), WorkspaceChangeCause::Presentation);

      if (stateRes->presentation != presentation)
      {
        if (auto result = views.setPresentation(viewId, presentation); !result)
        {
          return std::unexpected{result.error()};
        }
      }

      installCommit(std::move(pending));
      return {};
    }

    Result<> restoreNavigationPoint(NavigationPoint const& point, NavigationHistory nextHistory)
    {
      auto matchingViewId = kInvalidViewId;

      for (auto const viewId : views.listViews())
      {
        if (auto const state = views.trackListState(viewId);
            state.listId == point.listId && state.filterExpression == point.filterExpression)
        {
          matchingViewId = viewId;
          break;
        }
      }

      auto nextSnapshot = currentSnapshot;
      nextSnapshot.openViews.reserve(nextSnapshot.openViews.size() + 1);
      bool createdView = false;

      if (matchingViewId == kInvalidViewId)
      {
        auto result = views.createView(TrackListViewConfig{
          .listId = point.listId,
          .filterExpression = point.filterExpression,
          .optPresentation = point.presentation,
        });

        if (!result)
        {
          return std::unexpected{result.error()};
        }

        matchingViewId = *result;
        createdView = true;
      }

      try
      {
        auto const presentationChanged =
          !createdView && views.trackListState(matchingViewId).presentation != point.presentation;

        if (!std::ranges::contains(nextSnapshot.openViews, matchingViewId))
        {
          nextSnapshot.openViews.push_back(matchingViewId);
        }

        nextSnapshot.activeViewId = matchingViewId;
        auto pending = prepareCommit(std::move(nextSnapshot), std::move(nextHistory), WorkspaceChangeCause::Navigation);

        if (presentationChanged)
        {
          if (auto result = views.setPresentation(matchingViewId, point.presentation); !result)
          {
            return std::unexpected{result.error()};
          }
        }

        installCommit(std::move(pending));
      }
      catch (...)
      {
        if (createdView)
        {
          views.destroyView(matchingViewId);
        }

        throw;
      }

      return {};
    }

    Result<> goBack()
    {
      auto nextHistory = navigationHistory;
      auto optPoint = nextHistory.back();

      if (!optPoint)
      {
        return makeError(Error::Code::NotFound, "Workspace navigation history has no previous entry");
      }

      return restoreNavigationPoint(*optPoint, std::move(nextHistory));
    }

    Result<> goForward()
    {
      auto nextHistory = navigationHistory;
      auto optPoint = nextHistory.forward();

      if (!optPoint)
      {
        return makeError(Error::Code::NotFound, "Workspace navigation history has no forward entry");
      }

      return restoreNavigationPoint(*optPoint, std::move(nextHistory));
    }

    Result<> addPreset(CustomTrackPresentationPreset const& preset)
    {
      auto nextSnapshot = currentSnapshot;
      auto const it = std::ranges::find_if(
        nextSnapshot.customPresets, [&](auto const& existing) { return existing.label == preset.label; });

      if (it != nextSnapshot.customPresets.end())
      {
        if (*it == preset)
        {
          return {};
        }

        *it = preset;
      }
      else
      {
        nextSnapshot.customPresets.push_back(preset);
      }

      commitCandidate(std::move(nextSnapshot), navigationHistory, WorkspaceChangeCause::Presets);
      return {};
    }

    Result<> removePreset(std::string_view const presetId)
    {
      auto nextSnapshot = currentSnapshot;
      auto const removed = std::erase_if(
        nextSnapshot.customPresets, [presetId](auto const& preset) { return preset.spec.id == presetId; });

      if (removed == 0)
      {
        return {};
      }

      commitCandidate(std::move(nextSnapshot), navigationHistory, WorkspaceChangeCause::Presets);
      return {};
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

  Result<> WorkspaceService::focusView(ViewId const viewId)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->focus(viewId);
  }

  Result<ViewId> WorkspaceService::navigate(NavigationRequest const& request)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->navigate(request);
  }

  Result<> WorkspaceService::closeView(ViewId const viewId)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->close(viewId);
  }

  Result<> WorkspaceService::setActivePresentation(TrackPresentationSpec const& presentation,
                                                   PresentationChangeOptions const options)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->applyPresentation(presentation, options);
  }

  Result<TrackPresentationSpec> WorkspaceService::setActivePresentation(std::string_view const presentationId,
                                                                        PresentationChangeOptions const options)
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

    return normalizeTrackPresentationSpec(*optPresentation);
  }

  Result<> WorkspaceService::goBack()
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->goBack();
  }

  Result<> WorkspaceService::goForward()
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->goForward();
  }

  std::unique_ptr<TrackDetailProjection> WorkspaceService::detailProjection(DetailTarget const& target)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->views.detailProjection(target, *this, _implPtr->changes);
  }

  async::Subscription WorkspaceService::onChanged(std::move_only_function<void(WorkspaceChanged const&)> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->changedSignal.connect(std::move(handler));
  }

  std::span<CustomTrackPresentationPreset const> WorkspaceService::customPresets() const
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->currentSnapshot.customPresets;
  }

  Result<> WorkspaceService::addCustomPreset(CustomTrackPresentationPreset const& preset)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->addPreset(preset);
  }

  Result<> WorkspaceService::removeCustomPreset(std::string_view const presetId)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->removePreset(presetId);
  }

  void WorkspaceService::saveSession(ConfigStore& store) const
  {
    _implPtr->ensureOnExecutor();
    auto const snapshotValue = _implPtr->currentSnapshot;
    auto state = WorkspaceSessionState{};
    // A nonempty snapshot starts one past the valid range so the schema rejects a missing active id.
    state.activeViewIndex = snapshotValue.openViews.empty() ? 0 : snapshotValue.openViews.size();
    state.customPresets = snapshotValue.customPresets;

    for (std::size_t index = 0; index < snapshotValue.openViews.size(); ++index)
    {
      auto const viewId = snapshotValue.openViews[index];
      auto const viewState = _implPtr->views.trackListState(viewId);

      if (viewId == snapshotValue.activeViewId)
      {
        state.activeViewIndex = index;
      }

      state.openViews.push_back(TrackListViewConfig{
        .listId = viewState.listId,
        .filterExpression = viewState.filterExpression,
        .groupBy = viewState.groupBy,
        .sortBy = viewState.sortBy,
        .optPresentation = viewState.presentation,
      });
    }

    if (auto const result = store.save("workspace", state, detail::WorkspaceSessionYamlSchema{}); !result)
    {
      APP_LOG_ERROR("WorkspaceService: Failed to save session - {}", result.error().message);
    }
  }

  Result<> WorkspaceService::restoreSession(ConfigStore& store)
  {
    _implPtr->ensureOnExecutor();
    auto state = WorkspaceSessionState{};
    auto const loadedRes = store.load("workspace", state, detail::WorkspaceSessionYamlSchema{});

    if (!loadedRes)
    {
      if (loadedRes.error().code == Error::Code::NotFound)
      {
        return {};
      }

      return std::unexpected{loadedRes.error()};
    }

    if (!*loadedRes)
    {
      return {};
    }

    auto createdViewIds = std::vector<ViewId>{};
    createdViewIds.reserve(state.openViews.size());

    for (auto const& viewConfig : state.openViews)
    {
      auto result = _implPtr->views.createView(viewConfig);

      if (!result)
      {
        for (auto const viewId : createdViewIds)
        {
          _implPtr->views.destroyView(viewId);
        }

        return std::unexpected{result.error()};
      }

      createdViewIds.push_back(*result);
    }

    try
    {
      auto nextSnapshot = _implPtr->currentSnapshot;
      nextSnapshot.openViews.reserve(nextSnapshot.openViews.size() + createdViewIds.size());
      nextSnapshot.openViews.insert(nextSnapshot.openViews.end(), createdViewIds.begin(), createdViewIds.end());
      nextSnapshot.customPresets = std::move(state.customPresets);
      auto focused = kInvalidViewId;

      if (!createdViewIds.empty())
      {
        focused = createdViewIds[state.activeViewIndex];
      }
      else if (!nextSnapshot.openViews.empty())
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
        return {};
      }

      _implPtr->commitCandidate(std::move(nextSnapshot), std::move(nextHistory), WorkspaceChangeCause::Restore);
    }
    catch (...)
    {
      for (auto const viewId : createdViewIds)
      {
        _implPtr->views.destroyView(viewId);
      }

      throw;
    }

    return {};
  }
} // namespace ao::rt
