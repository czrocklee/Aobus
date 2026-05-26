// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ProjectionTypes.h>

#include <functional>
#include <optional>
#include <string>

namespace ao::rt
{
  class ViewService;
  class WorkspaceService;
}

namespace ao::uimodel::track
{
  struct TrackFilterViewState final
  {
    std::string entryText;
    std::string resolvedExpression;
    std::string tooltip;
    bool enabled = false;
    bool pending = false;
    bool hasError = false;
    bool canCreateSmartList = false;
  };

  class TrackFilterViewModel final
  {
  public:
    TrackFilterViewModel(rt::ViewService& viewService,
                         rt::WorkspaceService& workspaceService,
                         std::function<void(TrackFilterViewState const&)> onRender);

    TrackFilterViewModel(TrackFilterViewModel const&) = delete;
    TrackFilterViewModel& operator=(TrackFilterViewModel const&) = delete;
    TrackFilterViewModel(TrackFilterViewModel&&) = delete;
    TrackFilterViewModel& operator=(TrackFilterViewModel&&) = delete;

    ~TrackFilterViewModel() = default;

    void updateFilter(std::string const& rawText);

  private:
    void onFocusedViewChanged(rt::ViewId viewId);
    void onFilterStatusChanged(rt::FilterStatusChanged const& status);
    void refresh();

    rt::ViewService& _viewService;
    rt::WorkspaceService& _workspaceService;
    std::function<void(TrackFilterViewState const&)> _onRender;

    rt::ViewId _viewId{rt::kInvalidViewId};
    std::string _entryText;
    std::string _resolvedExpression;
    bool _filterPending = false;
    std::optional<Error> _optFilterError;

    rt::Subscription _focusSub;
    rt::Subscription _filterStatusSub;
  };
} // namespace ao::uimodel::track
