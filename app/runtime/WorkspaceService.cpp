// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/rt/NavigationHistory.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/Signal.h>
#include <ao/rt/Subscription.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSessionState.h>
#include <ao/rt/WorkspaceViewState.h>
#include <ao/rt/library/LibraryChanges.h>

#include <algorithm>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt
{
  namespace
  {
    class [[nodiscard]] ReplayScope final
    {
    public:
      explicit ReplayScope(bool& replaying)
        : _replaying{replaying}, _previous{replaying}
      {
        _replaying = true;
      }

      ReplayScope(ReplayScope const&) = delete;
      ReplayScope& operator=(ReplayScope const&) = delete;
      ReplayScope(ReplayScope&&) = delete;
      ReplayScope& operator=(ReplayScope&&) = delete;

      ~ReplayScope() { _replaying = _previous; }

    private:
      bool& _replaying;
      bool _previous;
    };
  } // namespace

  struct WorkspaceService::Impl final
  {
    ViewService& views;
    PlaybackService& playback;
    library::MusicLibrary& library;
    WorkspaceViewState layoutState;
    NavigationHistory navigationHistory;
    bool replayingNavigation = false;
    Subscription listsMutatedSub;

    Signal<ViewId> focusedViewChangedSignal;
    Signal<> customPresetsChangedSignal;
    Signal<WorkspaceService::NavigationHistoryChanged const&> navigationHistoryChangedSignal;
    std::vector<CustomTrackPresentationPreset> customPresets;

    Impl(WorkspaceService* self,
         ViewService& views,
         PlaybackService& playback,
         LibraryChanges const& changes,
         library::MusicLibrary& library)
      : views{views}, playback{playback}, library{library}
    {
      listsMutatedSub = changes.onListsMutated(
        [this, self](LibraryChanges::ListsMutated const& ev)
        {
          auto toClose = std::vector<ViewId>{};

          for (auto const id : ev.deleted)
          {
            for (auto const viewId : this->layoutState.openViews)
            {
              if (auto const& state = this->views.trackListState(viewId); state.listId == id)
              {
                toClose.push_back(viewId);
              }
            }
          }

          for (auto const viewId : toClose)
          {
            self->closeView(viewId);
          }
        });
    }

    std::optional<NavigationPoint> snapshotActiveView() const
    {
      auto const viewId = layoutState.activeViewId;

      if (viewId == kInvalidViewId)
      {
        return std::nullopt;
      }

      auto const state = views.trackListState(viewId);

      if (state.lifecycle == ViewLifecycleState::Destroyed)
      {
        return std::nullopt;
      }

      return NavigationPoint{
        .listId = state.listId, .filterExpression = state.filterExpression, .presentation = state.presentation};
    }

    void commitActiveViewIfRequested(NavigationOptions const& options)
    {
      if (!options.recordHistory || replayingNavigation)
      {
        return;
      }

      if (auto optPoint = snapshotActiveView(); optPoint)
      {
        auto const beforeBack = navigationHistory.canGoBack();
        auto const beforeForward = navigationHistory.canGoForward();

        navigationHistory.commit(std::move(*optPoint));

        if (beforeBack != navigationHistory.canGoBack() || beforeForward != navigationHistory.canGoForward())
        {
          emitNavigationHistoryChanged();
        }
      }
    }

    void emitNavigationHistoryChanged()
    {
      navigationHistoryChangedSignal.emit(NavigationHistoryChanged{
        .canGoBack = navigationHistory.canGoBack(), .canGoForward = navigationHistory.canGoForward()});
    }

    Result<ViewId> resolveOrCreateTargetView(NavigationTarget const& target,
                                             ViewService& viewsSvc,
                                             std::optional<TrackPresentationSpec> const& optPresentation)
    {
      if (std::holds_alternative<ListId>(target))
      {
        auto const listId = std::get<ListId>(target);

        for (auto const& record : viewsSvc.listViews())
        {
          if (record.kind == ViewKind::TrackList)
          {
            if (auto const& state = viewsSvc.trackListState(record.id);
                state.listId == listId && state.filterExpression.empty())
            {
              if (optPresentation)
              {
                viewsSvc.setPresentation(record.id, *optPresentation);
              }

              return record.id;
            }
          }
        }

        auto config = TrackListViewConfig{.listId = listId};

        if (optPresentation)
        {
          config.optPresentation = *optPresentation;
        }

        auto result = viewsSvc.createView(config, true);

        if (!result)
        {
          return std::unexpected{result.error()};
        }

        return result->viewId;
      }

      if (std::holds_alternative<FilteredListTarget>(target))
      {
        auto const& filtered = std::get<FilteredListTarget>(target);
        auto config = TrackListViewConfig{.listId = filtered.listId, .filterExpression = filtered.filterExpression};

        if (optPresentation)
        {
          config.optPresentation = *optPresentation;
        }

        auto result = viewsSvc.createView(config, true);

        if (!result)
        {
          return std::unexpected{result.error()};
        }

        return result->viewId;
      }

      if (std::holds_alternative<GlobalViewKind>(target))
      {
        if (std::get<GlobalViewKind>(target) == GlobalViewKind::AllTracks)
        {
          return resolveOrCreateTargetView(ListId{kAllTracksListId}, viewsSvc, optPresentation);
        }
      }

      return makeError(Error::Code::InvalidInput, "Unsupported workspace navigation target");
    }

    void focusView(ViewId viewId)
    {
      if (!std::ranges::contains(layoutState.openViews, viewId))
      {
        layoutState.openViews.push_back(viewId);
      }

      layoutState.activeViewId = viewId;
      layoutState.revision++;
      focusedViewChangedSignal.emit(viewId);
    }

    std::optional<TrackPresentationSpec> presentationForId(std::string_view id) const
    {
      if (auto const* preset = builtinTrackPresentationPreset(id); preset != nullptr)
      {
        return preset->spec;
      }

      for (auto const& custom : customPresets)
      {
        if (custom.spec.id == id)
        {
          return custom.spec;
        }
      }

      return std::nullopt;
    }

    Result<ViewId> restoreNavigationPoint(NavigationPoint const& point)
    {
      // Find an existing non-destroyed view matching listId and filterExpression.
      auto matchingViewId = kInvalidViewId;

      for (auto const& record : views.listViews())
      {
        if (record.kind != ViewKind::TrackList)
        {
          continue;
        }

        if (record.lifecycle == ViewLifecycleState::Destroyed)
        {
          continue;
        }

        if (auto const& state = views.trackListState(record.id);
            state.listId == point.listId && state.filterExpression == point.filterExpression)
        {
          matchingViewId = record.id;
          break;
        }
      }

      if (matchingViewId == kInvalidViewId)
      {
        auto config = TrackListViewConfig{
          .listId = point.listId,
          .filterExpression = point.filterExpression,
          .optPresentation = point.presentation,
        };
        auto result = views.createView(config, true);

        if (!result)
        {
          return std::unexpected{result.error()};
        }

        matchingViewId = result->viewId;
      }
      else
      {
        views.setPresentation(matchingViewId, point.presentation);
      }

      if (!std::ranges::contains(layoutState.openViews, matchingViewId))
      {
        layoutState.openViews.push_back(matchingViewId);
      }

      layoutState.activeViewId = matchingViewId;
      layoutState.revision++;
      focusedViewChangedSignal.emit(matchingViewId);
      return matchingViewId;
    }
  };

  WorkspaceService::WorkspaceService(ViewService& views,
                                     PlaybackService& playback,
                                     LibraryChanges const& changes,
                                     library::MusicLibrary& library)
    : _implPtr{std::make_unique<Impl>(this, views, playback, changes, library)}
  {
  }

  WorkspaceService::~WorkspaceService() = default;

  Subscription WorkspaceService::onFocusedViewChanged(std::move_only_function<void(ViewId)> handler)
  {
    return _implPtr->focusedViewChangedSignal.connect(std::move(handler));
  }

  Subscription WorkspaceService::onNavigationHistoryChanged(
    std::move_only_function<void(NavigationHistoryChanged const&)> handler)
  {
    return _implPtr->navigationHistoryChangedSignal.connect(std::move(handler));
  }

  WorkspaceViewState WorkspaceService::layoutState() const
  {
    return _implPtr->layoutState;
  }

  void WorkspaceService::setFocusedView(ViewId const viewId)
  {
    _implPtr->layoutState.activeViewId = viewId;
    _implPtr->layoutState.revision++;
    _implPtr->focusedViewChangedSignal.emit(viewId);
  }

  void WorkspaceService::addView(ViewId const viewId)
  {
    if (!std::ranges::contains(_implPtr->layoutState.openViews, viewId))
    {
      _implPtr->layoutState.openViews.push_back(viewId);
      _implPtr->layoutState.revision++;
    }
  }

  Result<ViewId> WorkspaceService::navigateTo(NavigationTarget const& target, NavigationOptions const options)
  {
    auto targetResult = _implPtr->resolveOrCreateTargetView(target, _implPtr->views, options.optPresentation);

    if (!targetResult)
    {
      return std::unexpected{targetResult.error()};
    }

    auto const targetViewId = *targetResult;
    APP_LOG_DEBUG("WorkspaceService: Navigating to viewId: {}", targetViewId.raw());
    _implPtr->focusView(targetViewId);
    _implPtr->commitActiveViewIfRequested(options);
    return targetViewId;
  }

  void WorkspaceService::setActivePresentation(TrackPresentationSpec const& presentation,
                                               NavigationOptions const options)
  {
    auto const viewId = _implPtr->layoutState.activeViewId;

    if (viewId == kInvalidViewId)
    {
      return;
    }

    _implPtr->views.setPresentation(viewId, presentation);
    _implPtr->commitActiveViewIfRequested(options);
  }

  TrackPresentationSpec WorkspaceService::setActivePresentation(std::string_view const presentationId,
                                                                NavigationOptions const options)
  {
    auto const viewId = _implPtr->layoutState.activeViewId;

    if (viewId == kInvalidViewId)
    {
      return {};
    }

    auto const optSpec = _implPtr->presentationForId(presentationId);

    if (!optSpec)
    {
      return {};
    }

    auto const spec = normalizeTrackPresentationSpec(*optSpec);
    _implPtr->views.setPresentation(viewId, spec);
    _implPtr->commitActiveViewIfRequested(options);
    return spec;
  }

  void WorkspaceService::jumpToAlbum(TrackId const trackId)
  {
    if (trackId == kInvalidTrackId)
    {
      return;
    }

    // Navigate to AllTracks without recording.
    auto const allTracksTarget = NavigationTarget{ListId{kAllTracksListId}};
    auto targetResult = _implPtr->resolveOrCreateTargetView(allTracksTarget, _implPtr->views, std::nullopt);

    if (!targetResult)
    {
      return;
    }

    auto const targetViewId = *targetResult;
    _implPtr->focusView(targetViewId);

    // Apply album presentation without recording.
    if (auto const* preset = builtinTrackPresentationPreset("albums"); preset != nullptr)
    {
      _implPtr->views.setPresentation(targetViewId, preset->spec);
    }

    // Reveal the track.
    _implPtr->playback.revealTrack(trackId, targetViewId);

    // Commit the final state once.
    _implPtr->commitActiveViewIfRequested({.recordHistory = true});
  }

  Result<ViewId> WorkspaceService::goBack()
  {
    auto previousHistory = _implPtr->navigationHistory;
    auto optPoint = _implPtr->navigationHistory.back();

    if (!optPoint)
    {
      return makeError(Error::Code::NotFound, "Workspace navigation history has no previous entry");
    }

    auto replay = ReplayScope{_implPtr->replayingNavigation};
    auto result = _implPtr->restoreNavigationPoint(*optPoint);

    if (!result)
    {
      _implPtr->navigationHistory = std::move(previousHistory);
      return std::unexpected{result.error()};
    }

    _implPtr->emitNavigationHistoryChanged();
    return *result;
  }

  Result<ViewId> WorkspaceService::goForward()
  {
    auto previousHistory = _implPtr->navigationHistory;
    auto optPoint = _implPtr->navigationHistory.forward();

    if (!optPoint)
    {
      return makeError(Error::Code::NotFound, "Workspace navigation history has no forward entry");
    }

    auto replay = ReplayScope{_implPtr->replayingNavigation};
    auto result = _implPtr->restoreNavigationPoint(*optPoint);

    if (!result)
    {
      _implPtr->navigationHistory = std::move(previousHistory);
      return std::unexpected{result.error()};
    }

    _implPtr->emitNavigationHistoryChanged();
    return *result;
  }

  bool WorkspaceService::canGoBack() const noexcept
  {
    return _implPtr->navigationHistory.canGoBack();
  }

  bool WorkspaceService::canGoForward() const noexcept
  {
    return _implPtr->navigationHistory.canGoForward();
  }

  void WorkspaceService::closeView(ViewId const viewId)
  {
    std::erase(_implPtr->layoutState.openViews, viewId);

    if (_implPtr->layoutState.activeViewId == viewId)
    {
      _implPtr->layoutState.activeViewId =
        _implPtr->layoutState.openViews.empty() ? rt::kInvalidViewId : _implPtr->layoutState.openViews.back();
    }

    _implPtr->layoutState.revision++;
    _implPtr->focusedViewChangedSignal.emit(_implPtr->layoutState.activeViewId);

    _implPtr->views.destroyView(viewId);
  }

  std::span<CustomTrackPresentationPreset const> WorkspaceService::customPresets() const
  {
    return _implPtr->customPresets;
  }

  void WorkspaceService::addCustomPreset(CustomTrackPresentationPreset const& preset)
  {
    if (auto it = std::ranges::find_if(
          _implPtr->customPresets, [&](auto const& existingPreset) { return existingPreset.label == preset.label; });
        it != _implPtr->customPresets.end())
    {
      *it = preset;
    }
    else
    {
      _implPtr->customPresets.push_back(preset);
    }

    _implPtr->customPresetsChangedSignal.emit();
  }

  void WorkspaceService::removeCustomPreset(std::string_view presetId)
  {
    std::erase_if(
      _implPtr->customPresets, [presetId](auto const& existingPreset) { return existingPreset.spec.id == presetId; });
    _implPtr->customPresetsChangedSignal.emit();
  }

  void WorkspaceService::setCustomPresets(std::vector<CustomTrackPresentationPreset> presets)
  {
    _implPtr->customPresets = std::move(presets);
    _implPtr->customPresetsChangedSignal.emit();
  }

  Subscription WorkspaceService::onCustomPresetsChanged(std::move_only_function<void()> handler)
  {
    return _implPtr->customPresetsChangedSignal.connect(std::move(handler));
  }

  void WorkspaceService::saveSession(ConfigStore& store) const
  {
    auto const layout = _implPtr->layoutState;
    auto state = WorkspaceSessionState{};

    auto const presets = _implPtr->customPresets;
    state.customPresets = std::vector(presets.begin(), presets.end());

    for (auto const viewId : layout.openViews)
    {
      auto const& viewState = _implPtr->views.trackListState(viewId);

      if (viewId == layout.activeViewId)
      {
        state.activeListId = viewState.listId;
      }

      state.openViews.push_back(TrackListViewConfig{.listId = viewState.listId,
                                                    .filterExpression = viewState.filterExpression,
                                                    .groupBy = viewState.groupBy,
                                                    .sortBy = viewState.sortBy,
                                                    .optPresentation = viewState.presentation});
    }

    store.save("workspace", state);

    if (auto const res = store.flush(); !res)
    {
      APP_LOG_ERROR("WorkspaceService: Failed to flush session - {}", res.error().message);
    }
  }

  Result<> WorkspaceService::restoreSession(ConfigStore& store)
  {
    auto state = WorkspaceSessionState{};

    if (auto const result = store.load("workspace", state); !result)
    {
      if (result.error().code == Error::Code::NotFound)
      {
        return {};
      }

      return std::unexpected{result.error()};
    }

    auto createdViewIds = std::vector<ViewId>{};
    createdViewIds.reserve(state.openViews.size());

    for (auto const& viewConfig : state.openViews)
    {
      auto result = _implPtr->views.createView(viewConfig, true);

      if (!result)
      {
        for (auto const viewId : createdViewIds)
        {
          _implPtr->views.destroyView(viewId);
        }

        return std::unexpected{result.error()};
      }

      createdViewIds.push_back(result->viewId);
    }

    for (auto const viewId : createdViewIds)
    {
      addView(viewId);
    }

    setCustomPresets(state.customPresets);

    auto focused = kInvalidViewId;

    for (auto const viewId : createdViewIds)
    {
      if (auto const& vs = _implPtr->views.trackListState(viewId); vs.listId == state.activeListId)
      {
        focused = viewId;

        if (!vs.filterExpression.empty())
        {
          break;
        }
      }
    }

    if (focused != kInvalidViewId)
    {
      setFocusedView(focused);
    }
    else if (!_implPtr->layoutState.openViews.empty())
    {
      setFocusedView(_implPtr->layoutState.openViews.front());
    }

    // Commit the restored view as the initial navigation point so the user
    // can navigate back to it after moving elsewhere.  canGoBack remains
    // false until a subsequent navigateTo commits a second point.
    _implPtr->commitActiveViewIfRequested({.recordHistory = true});
    return {};
  }
} // namespace ao::rt
