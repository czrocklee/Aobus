// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SessionPersistenceService.h"
#include "ConfigStore.h"
#include "CorePrimitives.h"
#include "PlaybackService.h"
#include "StateTypes.h"
#include "ViewService.h"
#include "WorkspaceService.h"
#include <ao/library/MusicLibrary.h>
#include <ao/utility/Log.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace ao::rt
{
  struct SessionPersistenceService::Impl final
  {
    WorkspaceService& workspace;
    ViewService& views;
    PlaybackService& playback;
    library::MusicLibrary& library;
    ConfigStore& configStore;

    Signal<std::string const&> sessionRestoredSignal;

    Impl(WorkspaceService& ws, ViewService& vs, PlaybackService& pb, library::MusicLibrary& lib, ConfigStore& cs)
      : workspace{ws}, views{vs}, playback{pb}, library{lib}, configStore{cs}
    {
    }
  };

  SessionPersistenceService::SessionPersistenceService(WorkspaceService& workspace,
                                                       ViewService& views,
                                                       PlaybackService& playback,
                                                       library::MusicLibrary& library,
                                                       ConfigStore& configStore)
    : _impl{std::make_unique<Impl>(workspace, views, playback, library, configStore)}
  {
  }

  SessionPersistenceService::~SessionPersistenceService() = default;

  void SessionPersistenceService::restore()
  {
    auto snapshot = SessionSnapshot{};

    if (auto const res = _impl->configStore.load("runtime", snapshot); !res)
    {
      if (res.error().code != Error::Code::NotFound)
      {
        APP_LOG_WARN("SessionPersistenceService: Failed to restore runtime - {}", res.error().message);
      }

      return;
    }

    for (auto const& viewConfig : snapshot.openViews)
    {
      auto const res = _impl->views.createView(viewConfig, true);
      _impl->workspace.addView(res.viewId);
    }

    if (auto const layout = _impl->workspace.layoutState();
        snapshot.optActiveViewIndex && *snapshot.optActiveViewIndex < layout.openViews.size())
    {
      _impl->workspace.setFocusedView(layout.openViews[*snapshot.optActiveViewIndex]);
    }
    else if (!layout.openViews.empty())
    {
      _impl->workspace.setFocusedView(layout.openViews.front());
    }

    if (!snapshot.lastBackend.empty())
    {
      _impl->playback.setOutput(audio::BackendId{snapshot.lastBackend},
                                audio::DeviceId{snapshot.lastOutputDeviceId},
                                audio::ProfileId{snapshot.lastProfile});
    }

    _impl->sessionRestoredSignal.emit(snapshot.lastLibraryPath);
  }

  void SessionPersistenceService::save()
  {
    auto const layout = _impl->workspace.layoutState();
    auto snapshot = SessionSnapshot{};
    snapshot.optActiveViewIndex = std::nullopt;
    snapshot.lastLibraryPath = _impl->library.rootPath().string();

    for (std::size_t i = 0; i < layout.openViews.size(); ++i)
    {
      auto const viewId = ViewId{layout.openViews[i]};

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

    _impl->configStore.save("runtime", snapshot);

    if (auto const res = _impl->configStore.flush(); !res)
    {
      APP_LOG_ERROR("SessionPersistenceService: Failed to flush runtime - {}", res.error().message);
    }
  }

  Subscription SessionPersistenceService::onSessionRestored(std::move_only_function<void(std::string const&)> handler)
  {
    return _impl->sessionRestoredSignal.connect(std::move(handler));
  }
} // namespace ao::rt
