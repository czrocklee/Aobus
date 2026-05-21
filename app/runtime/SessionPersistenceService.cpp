// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SessionPersistenceService.h"

#include "ConfigStore.h"
#include "CorePrimitives.h"
#include "CoreRuntime.h"
#include "PlaybackService.h"
#include "StateTypes.h"
#include "ViewService.h"
#include "WorkspaceService.h"
#include "ao/utility/Log.h"

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
    CoreRuntime& runtime;
    ConfigStore& globalConfig;
    ConfigStore& workspaceConfig;

    Signal<std::string const&> sessionRestoredSignal;

    Impl(WorkspaceService& ws, ViewService& vs, PlaybackService& pb, CoreRuntime& rt, ConfigStore& gc, ConfigStore& wc)
      : workspace{ws}, views{vs}, playback{pb}, runtime{rt}, globalConfig{gc}, workspaceConfig{wc}
    {
    }
  };

  SessionPersistenceService::SessionPersistenceService(WorkspaceService& workspace,
                                                       ViewService& views,
                                                       PlaybackService& playback,
                                                       CoreRuntime& runtime,
                                                       ConfigStore& globalConfig,
                                                       ConfigStore& workspaceConfig)
    : _impl{std::make_unique<Impl>(workspace, views, playback, runtime, globalConfig, workspaceConfig)}
  {
  }

  SessionPersistenceService::~SessionPersistenceService() = default;

  void SessionPersistenceService::restore()
  {
    auto globalSnapshot = GlobalSessionSnapshot{};
    auto workspaceSnapshot = WorkspaceSnapshot{};

    if (auto const res = _impl->globalConfig.load("runtime", globalSnapshot); !res)
    {
      if (res.error().code != Error::Code::NotFound)
      {
        APP_LOG_WARN("SessionPersistenceService: Failed to restore global runtime - {}", res.error().message);
      }
    }

    if (auto const res = _impl->workspaceConfig.load("workspace", workspaceSnapshot); !res)
    {
      if (res.error().code != Error::Code::NotFound)
      {
        APP_LOG_WARN("SessionPersistenceService: Failed to restore workspace - {}", res.error().message);
      }
    }

    for (auto const& viewConfig : workspaceSnapshot.openViews)
    {
      auto const res = _impl->views.createView(viewConfig, true);
      _impl->workspace.addView(res.viewId);
    }

    if (auto const layout = _impl->workspace.layoutState();
        workspaceSnapshot.optActiveViewIndex && *workspaceSnapshot.optActiveViewIndex < layout.openViews.size())
    {
      _impl->workspace.setFocusedView(layout.openViews[*workspaceSnapshot.optActiveViewIndex]);
    }
    else if (!layout.openViews.empty())
    {
      _impl->workspace.setFocusedView(layout.openViews.front());
    }

    if (!globalSnapshot.lastBackend.empty())
    {
      _impl->playback.setOutput(audio::BackendId{globalSnapshot.lastBackend},
                                audio::DeviceId{globalSnapshot.lastOutputDeviceId},
                                audio::ProfileId{globalSnapshot.lastProfile});
    }

    _impl->sessionRestoredSignal.emit(globalSnapshot.lastLibraryPath);
  }

  void SessionPersistenceService::save()
  {
    auto const layout = _impl->workspace.layoutState();
    auto globalSnapshot = GlobalSessionSnapshot{};
    auto workspaceSnapshot = WorkspaceSnapshot{};

    workspaceSnapshot.optActiveViewIndex = std::nullopt;
    globalSnapshot.lastLibraryPath = _impl->runtime.musicRoot().string();

    for (std::size_t i = 0; i < layout.openViews.size(); ++i)
    {
      auto const viewId = ViewId{layout.openViews[i]};

      if (viewId == layout.activeViewId)
      {
        workspaceSnapshot.optActiveViewIndex = static_cast<std::uint32_t>(i);
      }

      auto const& state = _impl->views.trackListState(viewId);
      workspaceSnapshot.openViews.push_back(TrackListViewConfig{
        .listId = state.listId,
        .filterExpression = state.filterExpression,
        .groupBy = state.groupBy,
        .sortBy = state.sortBy,
      });
    }

    // Capture playback output state for restoration
    auto const& pb = _impl->playback.state();
    globalSnapshot.lastBackend = pb.selectedOutput.backendId.raw();
    globalSnapshot.lastOutputDeviceId = pb.selectedOutput.deviceId.raw();
    globalSnapshot.lastProfile = pb.selectedOutput.profileId.raw();

    _impl->globalConfig.save("runtime", globalSnapshot);
    _impl->workspaceConfig.save("workspace", workspaceSnapshot);

    if (auto const res = _impl->globalConfig.flush(); !res)
    {
      APP_LOG_ERROR("SessionPersistenceService: Failed to flush global runtime - {}", res.error().message);
    }

    if (auto const res = _impl->workspaceConfig.flush(); !res)
    {
      APP_LOG_ERROR("SessionPersistenceService: Failed to flush workspace - {}", res.error().message);
    }
  }

  Subscription SessionPersistenceService::onSessionRestored(std::move_only_function<void(std::string const&)> handler)
  {
    return _impl->sessionRestoredSignal.connect(std::move(handler));
  }
} // namespace ao::rt
