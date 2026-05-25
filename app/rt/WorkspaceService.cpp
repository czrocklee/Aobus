// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/utility/Log.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt
{
  struct WorkspaceService::Impl final
  {
    ViewService& views;
    PlaybackService& playback;
    LibraryMutationService& mutation;
    library::MusicLibrary& library;
    LayoutState layoutState;
    Subscription listsMutatedSub;

    Signal<ViewId> focusedViewChangedSignal;
    Signal<> customPresetsChangedSignal;
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
  };

  WorkspaceService::WorkspaceService(ViewService& views,
                                     PlaybackService& playback,
                                     LibraryMutationService& mutation,
                                     library::MusicLibrary& library)
    : _impl{std::make_unique<Impl>(this, views, playback, mutation, library)}
  {
  }

  WorkspaceService::~WorkspaceService() = default;

  Subscription WorkspaceService::onFocusedViewChanged(std::move_only_function<void(ViewId)> handler)
  {
    return _impl->focusedViewChangedSignal.connect(std::move(handler));
  }

  LayoutState WorkspaceService::layoutState() const
  {
    return _impl->layoutState;
  }

  void WorkspaceService::setFocusedView(ViewId const viewId)
  {
    _impl->layoutState.activeViewId = viewId;
    _impl->layoutState.revision++;
    _impl->focusedViewChangedSignal.emit(viewId);
  }

  void WorkspaceService::addView(ViewId const viewId)
  {
    if (!std::ranges::contains(_impl->layoutState.openViews, viewId))
    {
      _impl->layoutState.openViews.push_back(viewId);
      _impl->layoutState.revision++;
    }
  }

  void WorkspaceService::navigateTo(std::variant<ListId, std::string, GlobalViewKind> const& target)
  {
    auto targetViewId = rt::kInvalidViewId;

    if (std::holds_alternative<ListId>(target))
    {
      auto const listId = ListId{std::get<ListId>(target)};

      for (auto const& record : _impl->views.listViews())
      {
        if (record.kind == ViewKind::TrackList)
        {
          if (auto const& state = _impl->views.trackListState(record.id);
              state.listId == listId && state.filterExpression.empty())
          {
            targetViewId = record.id;
            break;
          }
        }
      }

      if (targetViewId == rt::kInvalidViewId)
      {
        auto const res = _impl->views.createView(TrackListViewConfig{.listId = listId}, true);
        targetViewId = res.viewId;
      }
    }
    else if (std::holds_alternative<std::string>(target))
    {
      auto const query = std::get<std::string>(target);
      auto const res = _impl->views.createView(TrackListViewConfig{.filterExpression = query}, true);
      targetViewId = res.viewId;
    }
    else if (std::holds_alternative<GlobalViewKind>(target))
    {
      if (auto const kind = std::get<GlobalViewKind>(target); kind == GlobalViewKind::AllTracks)
      {
        navigateTo(rt::kAllTracksListId);
        return;
      }
    }

    if (targetViewId != rt::kInvalidViewId)
    {
      APP_LOG_DEBUG("WorkspaceService: Navigating to existing viewId: {}", targetViewId.raw());

      if (!std::ranges::contains(_impl->layoutState.openViews, targetViewId))
      {
        _impl->layoutState.openViews.push_back(targetViewId);
      }

      _impl->layoutState.activeViewId = targetViewId;
      _impl->layoutState.revision++;
      _impl->focusedViewChangedSignal.emit(targetViewId);
    }
    else
    {
      APP_LOG_DEBUG("WorkspaceService: Navigation failed to find or create target view");
    }
  }

  void WorkspaceService::closeView(ViewId const viewId)
  {
    if (auto const it = std::ranges::find(_impl->layoutState.openViews, viewId);
        it != _impl->layoutState.openViews.end())
    {
      _impl->layoutState.openViews.erase(it);
    }

    if (_impl->layoutState.activeViewId == viewId)
    {
      _impl->layoutState.activeViewId =
        _impl->layoutState.openViews.empty() ? rt::kInvalidViewId : _impl->layoutState.openViews.back();
    }

    _impl->layoutState.revision++;
    _impl->focusedViewChangedSignal.emit(_impl->layoutState.activeViewId);

    _impl->views.destroyView(viewId);
  }

  std::span<CustomTrackPresentationPreset const> WorkspaceService::customPresets() const
  {
    return _impl->customPresets;
  }

  void WorkspaceService::addCustomPreset(CustomTrackPresentationPreset const& preset)
  {
    if (auto it = std::ranges::find_if(
          _impl->customPresets, [&](auto const& existingPreset) { return existingPreset.label == preset.label; });
        it != _impl->customPresets.end())
    {
      *it = preset;
    }
    else
    {
      _impl->customPresets.push_back(preset);
    }

    _impl->customPresetsChangedSignal.emit();
  }

  void WorkspaceService::removeCustomPreset(std::string_view presetId)
  {
    std::erase_if(
      _impl->customPresets, [presetId](auto const& existingPreset) { return existingPreset.label == presetId; });
    _impl->customPresetsChangedSignal.emit();
  }

  void WorkspaceService::setCustomPresets(std::vector<CustomTrackPresentationPreset> presets)
  {
    _impl->customPresets = std::move(presets);
    _impl->customPresetsChangedSignal.emit();
  }

  Subscription WorkspaceService::onCustomPresetsChanged(std::move_only_function<void()> handler)
  {
    return _impl->customPresetsChangedSignal.connect(std::move(handler));
  }

  void WorkspaceService::saveSession(ConfigStore& store) const
  {
    auto const layout = _impl->layoutState;
    auto state = SessionState{};

    auto const presets = _impl->customPresets;
    state.customPresets = std::vector<CustomTrackPresentationPreset>(presets.begin(), presets.end());

    for (auto const viewId : layout.openViews)
    {
      auto const& viewState = _impl->views.trackListState(viewId);

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
      auto const res = _impl->views.createView(viewConfig, true);
      addView(res.viewId);
    }

    setCustomPresets(state.customPresets);

    auto focused = kInvalidViewId;

    for (auto const viewId : _impl->layoutState.openViews)
    {
      if (auto const& vs = _impl->views.trackListState(viewId); vs.listId == state.activeListId)
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
    else if (!_impl->layoutState.openViews.empty())
    {
      setFocusedView(_impl->layoutState.openViews.front());
    }
  }
} // namespace ao::rt
