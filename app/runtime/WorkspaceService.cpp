// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "WorkspaceService.h"
#include "EventBus.h"
#include "EventTypes.h"
#include "ISessionPersistence.h"
#include "PlaybackService.h"
#include "ViewService.h"

#include <algorithm>
#include <ao/library/MusicLibrary.h>
#include <ranges>

namespace ao::app
{
  struct WorkspaceService::Impl final
  {
    EventBus& events;
    ViewService& views;
    PlaybackService& playback;
    ao::library::MusicLibrary& library;
    LayoutState layoutState;
    std::shared_ptr<ISessionPersistence> persistence;

    Impl(EventBus& events,
         ViewService& views,
         PlaybackService& playback,
         ao::library::MusicLibrary& library,
         std::shared_ptr<ISessionPersistence> persistence)
      : events{events}, views{views}, playback{playback}, library{library}, persistence{std::move(persistence)}
    {
    }
  };

  WorkspaceService::WorkspaceService(EventBus& events,
                                     ViewService& views,
                                     PlaybackService& playback,
                                     ao::library::MusicLibrary& library,
                                     std::shared_ptr<ISessionPersistence> persistence)
    : _impl{std::make_unique<Impl>(events, views, playback, library, std::move(persistence))}
  {
  }

  WorkspaceService::~WorkspaceService() = default;

  LayoutState WorkspaceService::layoutState() const
  {
    return _impl->layoutState;
  }

  void WorkspaceService::setFocusedView(ViewId viewId)
  {
    _impl->layoutState.activeViewId = viewId;
    _impl->layoutState.revision++;
    _impl->events.publish(FocusedViewChanged{.viewId = viewId});
  }

  void WorkspaceService::navigateTo(std::variant<ao::ListId, std::string, GlobalViewKind> target)
  {
    ViewId targetViewId{};

    if (std::holds_alternative<ao::ListId>(target))
    {
      auto listId = std::get<ao::ListId>(target);
      for (auto const& record : _impl->views.listViews())
      {
        if (record.kind == ViewKind::TrackList)
        {
          auto const& state = _impl->views.trackListState(record.id);
          if (state.listId == listId && state.filterExpression.empty())
          {
            targetViewId = record.id;
            break;
          }
        }
      }

      if (targetViewId == ViewId{})
      {
        auto res = _impl->views.createView(TrackListViewConfig{.listId = listId}, true);
        targetViewId = res.viewId;
      }
    }
    else if (std::holds_alternative<std::string>(target))
    {
      auto query = std::get<std::string>(target);
      auto res = _impl->views.createView(TrackListViewConfig{.filterExpression = query}, true);
      targetViewId = res.viewId;
    }
    else if (std::holds_alternative<GlobalViewKind>(target))
    {
      auto kind = std::get<GlobalViewKind>(target);
      if (kind == GlobalViewKind::AllTracks)
      {
        auto res = _impl->views.createView(TrackListViewConfig{}, true);
        targetViewId = res.viewId;
      }
    }

    if (targetViewId != ViewId{})
    {
      if (std::ranges::find(_impl->layoutState.openViews, targetViewId) == _impl->layoutState.openViews.end())
      {
        _impl->layoutState.openViews.push_back(targetViewId);
      }
      _impl->layoutState.activeViewId = targetViewId;
      _impl->layoutState.revision++;
      _impl->events.publish(FocusedViewChanged{.viewId = targetViewId});
    }
  }

  void WorkspaceService::closeView(ViewId viewId)
  {
    auto it = std::ranges::find(_impl->layoutState.openViews, viewId);
    if (it != _impl->layoutState.openViews.end())
    {
      _impl->layoutState.openViews.erase(it);
    }

    if (_impl->layoutState.activeViewId == viewId)
    {
      _impl->layoutState.activeViewId =
        _impl->layoutState.openViews.empty() ? ViewId{} : _impl->layoutState.openViews.back();
    }

    _impl->layoutState.revision++;
    _impl->events.publish(FocusedViewChanged{.viewId = _impl->layoutState.activeViewId});

    _impl->views.destroyView(viewId);
  }

  void WorkspaceService::restoreSession()
  {
    if (!_impl->persistence)
    {
      return;
    }

    auto const optSnapshot = _impl->persistence->loadSnapshot();
    if (!optSnapshot)
    {
      return;
    }

    auto const& snapshot = *optSnapshot;

    for (auto const& viewConfig : snapshot.openViews)
    {
      auto res = _impl->views.createView(viewConfig, true);
      _impl->layoutState.openViews.push_back(res.viewId);
    }

    if (snapshot.activeViewIndex && *snapshot.activeViewIndex < _impl->layoutState.openViews.size())
    {
      _impl->layoutState.activeViewId = _impl->layoutState.openViews[*snapshot.activeViewIndex];
    }
    else if (!_impl->layoutState.openViews.empty())
    {
      _impl->layoutState.activeViewId = _impl->layoutState.openViews.front();
    }

    _impl->layoutState.revision++;

    if (!snapshot.lastBackend.empty())
    {
      _impl->playback.setOutput(ao::audio::BackendId{snapshot.lastBackend},
                                ao::audio::DeviceId{snapshot.lastOutputDeviceId},
                                ao::audio::ProfileId(snapshot.lastProfile));
    }

    _impl->events.publish(SessionRestored{.libraryPath = snapshot.lastLibraryPath});
    if (_impl->layoutState.activeViewId != ViewId{})
    {
      _impl->events.publish(FocusedViewChanged{.viewId = _impl->layoutState.activeViewId});
    }
  }

  void WorkspaceService::saveSession()
  {
    if (!_impl->persistence)
    {
      return;
    }

    auto const& layout = _impl->layoutState;
    auto snapshot = SessionSnapshot{};
    snapshot.activeViewIndex = std::nullopt;
    snapshot.lastLibraryPath = _impl->library.rootPath().string();

    for (auto i = std::size_t{0}; i < layout.openViews.size(); ++i)
    {
      auto const viewId = layout.openViews[i];
      if (viewId == layout.activeViewId)
      {
        snapshot.activeViewIndex = static_cast<std::uint32_t>(i);
      }

      auto const& state = _impl->views.trackListState(viewId);
      snapshot.openViews.push_back(TrackListViewConfig{
        .listId = state.listId,
        .filterExpression = state.filterExpression,
        .groupBy = state.groupBy,
        .sortBy = state.sortBy,
      });
    }

    // Capture playback output state for restoration
    auto const pb = _impl->playback.state();
    snapshot.lastBackend = pb.selectedOutput.backendId.value();
    snapshot.lastOutputDeviceId = pb.selectedOutput.deviceId.value();
    snapshot.lastProfile = pb.selectedOutput.profileId.value();

    _impl->persistence->saveSnapshot(snapshot);
  }
}
