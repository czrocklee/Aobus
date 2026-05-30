// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/NavigationHistory.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/utility/Log.h>

#include <algorithm>
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
    class ReplayScope final
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
  }

  struct WorkspaceService::Impl final
  {
    ViewService& views;
    PlaybackService& playback;
    LibraryMutationService& mutation;
    library::MusicLibrary& library;
    LayoutState layoutState;
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
         LibraryMutationService& mutation,
         library::MusicLibrary& library)
      : views{views}, playback{playback}, mutation{mutation}, library{library}
    {
      listsMutatedSub = mutation.onListsMutated(
        [this, self](LibraryMutationService::ListsMutated const& ev)
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

    ViewId resolveOrCreateTargetView(NavigationTarget const& target,
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

        auto const res = viewsSvc.createView(config, true);
        return res.viewId;
      }

      if (std::holds_alternative<FilteredListTarget>(target))
      {
        auto const& filtered = std::get<FilteredListTarget>(target);
        auto config = TrackListViewConfig{.listId = filtered.listId, .filterExpression = filtered.filterExpression};

        if (optPresentation)
        {
          config.optPresentation = *optPresentation;
        }

        auto const res = viewsSvc.createView(config, true);
        return res.viewId;
      }

      if (std::holds_alternative<GlobalViewKind>(target))
      {
        if (std::get<GlobalViewKind>(target) == GlobalViewKind::AllTracks)
        {
          return resolveOrCreateTargetView(ListId{kAllTracksListId}, viewsSvc, optPresentation);
        }
      }

      return kInvalidViewId;
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

    void restoreNavigationPoint(NavigationPoint const& point)
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
        auto config = TrackListViewConfig{.listId = point.listId, .filterExpression = point.filterExpression};
        auto const res = views.createView(config, true);
        matchingViewId = res.viewId;
      }

      if (!std::ranges::contains(layoutState.openViews, matchingViewId))
      {
        layoutState.openViews.push_back(matchingViewId);
      }

      layoutState.activeViewId = matchingViewId;
      layoutState.revision++;
      focusedViewChangedSignal.emit(matchingViewId);

      views.setPresentation(matchingViewId, point.presentation);
    }
  };

  WorkspaceService::WorkspaceService(ViewService& views,
                                     PlaybackService& playback,
                                     LibraryMutationService& mutation,
                                     library::MusicLibrary& library)
    : _implPtr{std::make_unique<Impl>(this, views, playback, mutation, library)}
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

  LayoutState WorkspaceService::layoutState() const
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

  void WorkspaceService::navigateTo(NavigationTarget const& target, NavigationOptions const options)
  {
    auto const targetViewId = _implPtr->resolveOrCreateTargetView(target, _implPtr->views, options.optPresentation);

    if (targetViewId == kInvalidViewId)
    {
      APP_LOG_DEBUG("WorkspaceService: Navigation failed to find or create target view");
      return;
    }

    APP_LOG_DEBUG("WorkspaceService: Navigating to viewId: {}", targetViewId.raw());
    _implPtr->focusView(targetViewId);
    _implPtr->commitActiveViewIfRequested(options);
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
    auto const targetViewId = _implPtr->resolveOrCreateTargetView(allTracksTarget, _implPtr->views, std::nullopt);

    if (targetViewId == kInvalidViewId)
    {
      return;
    }

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

  bool WorkspaceService::goBack()
  {
    auto optPoint = _implPtr->navigationHistory.back();

    if (!optPoint)
    {
      return false;
    }

    auto replay = ReplayScope{_implPtr->replayingNavigation};
    _implPtr->restoreNavigationPoint(*optPoint);
    _implPtr->emitNavigationHistoryChanged();
    return true;
  }

  bool WorkspaceService::goForward()
  {
    auto optPoint = _implPtr->navigationHistory.forward();

    if (!optPoint)
    {
      return false;
    }

    auto replay = ReplayScope{_implPtr->replayingNavigation};
    _implPtr->restoreNavigationPoint(*optPoint);
    _implPtr->emitNavigationHistoryChanged();
    return true;
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
    if (auto const it = std::ranges::find(_implPtr->layoutState.openViews, viewId);
        it != _implPtr->layoutState.openViews.end())
    {
      _implPtr->layoutState.openViews.erase(it);
    }

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
      _implPtr->customPresets, [presetId](auto const& existingPreset) { return existingPreset.label == presetId; });
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
    auto state = SessionState{};

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
                                                    .sortBy = viewState.sortBy});
    }

    store.save("workspace", state);

    if (auto const res = store.flush(); !res)
    {
      APP_LOG_ERROR("WorkspaceService: Failed to flush session - {}", res.error().message);
    }
  }

  void WorkspaceService::restoreSession(ConfigStore& store)
  {
    auto state = SessionState{};

    if (auto const res = store.load("workspace", state); !res)
    {
      if (res.error().code != Error::Code::NotFound)
      {
        APP_LOG_WARN("WorkspaceService: Failed to restore session - {}", res.error().message);
      }

      return;
    }

    for (auto const& viewConfig : state.openViews)
    {
      auto const res = _implPtr->views.createView(viewConfig, true);
      addView(res.viewId);
    }

    setCustomPresets(state.customPresets);

    auto focused = kInvalidViewId;

    for (auto const viewId : _implPtr->layoutState.openViews)
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
  }
} // namespace ao::rt
