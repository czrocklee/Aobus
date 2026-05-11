// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "WorkspaceService.h"
#include "ConfigStore.h"
#include "LibraryMutationService.h"
#include "PlaybackService.h"
#include "ViewService.h"

#include <algorithm>
#include <ao/library/MusicLibrary.h>
#include <ranges>

namespace ao::rt
{
  struct WorkspaceService::Impl final
  {
    ViewService& views;
    PlaybackService& playback;
    LibraryMutationService& mutation;
    ao::library::MusicLibrary& library;
    LayoutState layoutState;
    std::shared_ptr<ConfigStore> configStore;
    Subscription listsMutatedSub;

    Signal<ViewId> focusedViewChangedSignal;
    Signal<std::string const&> sessionRestoredSignal;

    Impl(WorkspaceService* self,
         ViewService& views,
         PlaybackService& playback,
         LibraryMutationService& mutation,
         ao::library::MusicLibrary& library,
         std::shared_ptr<ConfigStore> configStore)
      : views{views}, playback{playback}, mutation{mutation}, library{library}, configStore{std::move(configStore)}
    {
      listsMutatedSub = mutation.onListsMutated(
        [this, self](LibraryMutationService::ListsMutated const& ev)
        {
          auto toClose = std::vector<ViewId>{};
          for (auto id : ev.deleted)
          {
            for (auto const viewId : this->layoutState.openViews)
            {
              auto const& state = this->views.trackListState(viewId);
              if (state.listId == id)
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
                                     ao::library::MusicLibrary& library,
                                     std::shared_ptr<ConfigStore> configStore)
    : _impl{std::make_unique<Impl>(this, views, playback, mutation, library, std::move(configStore))}
  {
  }

  WorkspaceService::~WorkspaceService() = default;

  Subscription WorkspaceService::onFocusedViewChanged(std::move_only_function<void(ViewId)> handler)
  {
    return _impl->focusedViewChangedSignal.connect(std::move(handler));
  }

  Subscription WorkspaceService::onSessionRestored(std::move_only_function<void(std::string const&)> handler)
  {
    return _impl->sessionRestoredSignal.connect(std::move(handler));
  }

  LayoutState WorkspaceService::layoutState() const
  {
    return _impl->layoutState;
  }

  void WorkspaceService::setFocusedView(ViewId viewId)
  {
    _impl->layoutState.activeViewId = viewId;
    _impl->layoutState.revision++;
    _impl->focusedViewChangedSignal.emit(viewId);
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
      if (auto kind = std::get<GlobalViewKind>(target); kind == GlobalViewKind::AllTracks)
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
      _impl->focusedViewChangedSignal.emit(targetViewId);
    }
  }

  void WorkspaceService::closeView(ViewId viewId)
  {
    if (auto it = std::ranges::find(_impl->layoutState.openViews, viewId); it != _impl->layoutState.openViews.end())
    {
      _impl->layoutState.openViews.erase(it);
    }

    if (_impl->layoutState.activeViewId == viewId)
    {
      _impl->layoutState.activeViewId =
        _impl->layoutState.openViews.empty() ? ViewId{} : _impl->layoutState.openViews.back();
    }

    _impl->layoutState.revision++;
    _impl->focusedViewChangedSignal.emit(_impl->layoutState.activeViewId);

    _impl->views.destroyView(viewId);
  }

  void WorkspaceService::restoreSession()
  {
    if (!_impl->configStore)
    {
      return;
    }

    auto snapshot = SessionSnapshot{};
    _impl->configStore->load("session", snapshot);

    for (auto const& viewConfig : snapshot.openViews)
    {
      auto res = _impl->views.createView(viewConfig, true);
      _impl->layoutState.openViews.push_back(res.viewId);
    }

    if (snapshot.optActiveViewIndex && *snapshot.optActiveViewIndex < _impl->layoutState.openViews.size())
    {
      _impl->layoutState.activeViewId = _impl->layoutState.openViews[*snapshot.optActiveViewIndex];
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

    _impl->sessionRestoredSignal.emit(snapshot.lastLibraryPath);
    if (_impl->layoutState.activeViewId != ViewId{})
    {
      _impl->focusedViewChangedSignal.emit(_impl->layoutState.activeViewId);
    }
  }

  void WorkspaceService::saveSession()
  {
    if (!_impl->configStore)
    {
      return;
    }

    auto const& layout = _impl->layoutState;
    auto snapshot = SessionSnapshot{};
    snapshot.optActiveViewIndex = std::nullopt;
    snapshot.lastLibraryPath = _impl->library.rootPath().string();

    for (auto i = std::size_t{0}; i < layout.openViews.size(); ++i)
    {
      auto const viewId = layout.openViews[i];
      if (viewId == layout.activeViewId)
      {
        snapshot.optActiveViewIndex = static_cast<std::uint32_t>(i);
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
    auto const& pb = _impl->playback.state();
    snapshot.lastBackend = pb.selectedOutput.backendId.value();
    snapshot.lastOutputDeviceId = pb.selectedOutput.deviceId.value();
    snapshot.lastProfile = pb.selectedOutput.profileId.value();

    _impl->configStore->save("session", snapshot);
    _impl->configStore->flush();
  }
}
