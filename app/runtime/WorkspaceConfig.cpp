// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "WorkspaceConfig.h"

#include "ConfigStore.h"
#include "CorePrimitives.h"
#include "CoreRuntime.h"
#include "PlaybackService.h"
#include "StateTypes.h"
#include "TrackPresentation.h"
#include "ViewService.h"
#include "WorkspaceService.h"
#include "ao/utility/Log.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ao::rt
{
  struct WorkspaceConfig::Impl final
  {
    WorkspaceService& workspace;
    ViewService& views;
    PlaybackService& playback;
    CoreRuntime& runtime;
    ConfigStore& store;

    Impl(WorkspaceService& ws, ViewService& vs, PlaybackService& pb, CoreRuntime& rt, ConfigStore& st)
      : workspace{ws}, views{vs}, playback{pb}, runtime{rt}, store{st}
    {
    }
  };

  WorkspaceConfig::WorkspaceConfig(WorkspaceService& workspace,
                                   ViewService& views,
                                   PlaybackService& playback,
                                   CoreRuntime& runtime,
                                   ConfigStore& store)
    : _impl{std::make_unique<Impl>(workspace, views, playback, runtime, store)}
  {
  }

  WorkspaceConfig::~WorkspaceConfig() = default;

  void WorkspaceConfig::restore()
  {
    auto state = SessionState{};

    if (auto const res = _impl->store.load("workspace", state); !res)
    {
      if (res.error().code != Error::Code::NotFound)
      {
        APP_LOG_WARN("WorkspaceConfig: Failed to restore workspace - {}", res.error().message);
      }

      return;
    }

    for (auto const& viewConfig : state.openViews)
    {
      auto const res = _impl->views.createView(viewConfig, true);
      _impl->workspace.addView(res.viewId);
    }

    _impl->workspace.setCustomPresets(state.customPresets);

    if (auto const layout = _impl->workspace.layoutState();
        state.optActiveViewIndex && *state.optActiveViewIndex < layout.openViews.size())
    {
      _impl->workspace.setFocusedView(layout.openViews[*state.optActiveViewIndex]);
    }
    else if (!layout.openViews.empty())
    {
      _impl->workspace.setFocusedView(layout.openViews.front());
    }
  }

  void WorkspaceConfig::save()
  {
    auto const layout = _impl->workspace.layoutState();
    auto state = SessionState{};

    state.optActiveViewIndex = std::nullopt;

    auto const presets = _impl->workspace.customPresets();
    state.customPresets = std::vector<CustomTrackPresentationPreset>(presets.begin(), presets.end());

    for (std::size_t i = 0; i < layout.openViews.size(); ++i)
    {
      auto const viewId = ViewId{layout.openViews[i]};

      if (viewId == layout.activeViewId)
      {
        state.optActiveViewIndex = static_cast<std::uint32_t>(i);
      }

      auto const& viewState = _impl->views.trackListState(viewId);
      state.openViews.push_back(TrackListViewConfig{
        .listId = viewState.listId,
        .filterExpression = viewState.filterExpression,
        .groupBy = viewState.groupBy,
        .sortBy = viewState.sortBy,
      });
    }

    _impl->store.save("workspace", state);

    if (auto const res = _impl->store.flush(); !res)
    {
      APP_LOG_ERROR("WorkspaceConfig: Failed to flush workspace - {}", res.error().message);
    }
  }
} // namespace ao::rt
