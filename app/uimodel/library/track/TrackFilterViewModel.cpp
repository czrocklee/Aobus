// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/uimodel/library/track/TrackFilterResolver.h>
#include <ao/uimodel/library/track/TrackFilterViewModel.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

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
      [this](rt::WorkspaceChanged const& changed) noexcept
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

    // Reached from the workspace observer, whose snapshot can name a view that
    // has already been destroyed.
    auto const found = _viewService.findTrackListState(_viewId);

    if (!found)
    {
      _viewId = rt::kInvalidViewId;
      _entryText.clear();
      _resolvedExpression.clear();
      _optFilterError.reset();
      refresh();
      return;
    }

    _entryText = found->filterExpression;

    auto const resolved = resolveTrackFilterExpression(_entryText);
    _resolvedExpression = resolved.expression;
    _optFilterError = found->optFilterError;

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
        view.tooltip = PresentationTextCatalog{}.trackFilterError(_optFilterError->message);
      }

      view.canCreateSmartList = !view.resolvedExpression.empty() && !view.hasError;
    }

    if (_onRender)
    {
      _onRender(view);
    }
  }
} // namespace ao::uimodel
