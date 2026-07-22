// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/uimodel/library/track/TrackFilterResolver.h>
#include <ao/uimodel/library/track/TrackFilterViewModel.h>

#include <format>
#include <functional>
#include <string>
#include <utility>

namespace ao::uimodel
{
  TrackFilterViewModel::TrackFilterViewModel(rt::ViewService& viewService,
                                             rt::WorkspaceService& workspaceService,
                                             std::function<void(TrackFilterViewState const&)> onRender)
    : _viewService{viewService}, _workspaceService{workspaceService}, _onRender{std::move(onRender)}
  {
    _focusSub = _workspaceService.onChanged(
      [this](rt::WorkspaceChanged const& changed)
      {
        if (changed.snapshot.activeViewId != _viewId)
        {
          handleFocusedViewChanged(changed.snapshot.activeViewId);
        }
      });

    handleFocusedViewChanged(_workspaceService.snapshot().activeViewId);
  }

  void TrackFilterViewModel::updateFilter(std::string const& rawText)
  {
    _entryText = rawText;

    if (_viewId == rt::kInvalidViewId)
    {
      refresh();
      return;
    }

    auto const resolved = resolveTrackFilterExpression(rawText);
    _resolvedExpression = resolved.expression;

    auto const result = _viewService.setFilter(_viewId, _resolvedExpression);

    if (!result)
    {
      _optFilterError = result.error();
    }
    else
    {
      _optFilterError = _viewService.trackListState(_viewId).optFilterError;
    }

    refresh();
  }

  void TrackFilterViewModel::handleFocusedViewChanged(rt::ViewId viewId)
  {
    _viewId = viewId;

    if (_viewId == rt::kInvalidViewId)
    {
      _entryText.clear();
      _resolvedExpression.clear();
      _optFilterError.reset();
      refresh();
      return;
    }

    auto const state = _viewService.trackListState(_viewId);
    _entryText = state.filterExpression;

    auto const resolved = resolveTrackFilterExpression(_entryText);
    _resolvedExpression = resolved.expression;
    _optFilterError = state.optFilterError;

    refresh();
  }

  void TrackFilterViewModel::refresh()
  {
    auto view = TrackFilterViewState{};

    if (_viewId == rt::kInvalidViewId)
    {
      view.enabled = false;
    }
    else
    {
      view.enabled = true;
      view.entryText = _entryText;
      view.resolvedExpression = _resolvedExpression;

      if (_optFilterError)
      {
        view.hasError = true;
        view.tooltip = std::format("Filter error: {}", _optFilterError->message);
      }

      view.canCreateSmartList = !view.resolvedExpression.empty() && !view.hasError;
    }

    if (_onRender)
    {
      _onRender(view);
    }
  }
} // namespace ao::uimodel
