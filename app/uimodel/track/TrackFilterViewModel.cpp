// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/rt/CorePrimitives.h"
#include <ao/rt/ProjectionTypes.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/track/TrackFilterViewModel.h>

#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <utility>

namespace ao::uimodel::track
{
  namespace
  {
    enum class TrackFilterMode : std::uint8_t
    {
      None,
      PlainText,
      Expression
    };

    struct ResolvedFilter final
    {
      TrackFilterMode mode = TrackFilterMode::None;
      std::string expression;
    };

    ResolvedFilter resolveTrackFilterExpression(std::string const& raw)
    {
      if (raw.empty())
      {
        return {.mode = TrackFilterMode::None, .expression = ""};
      }

      if (raw.starts_with('$'))
      {
        return {.mode = TrackFilterMode::Expression, .expression = raw};
      }

      return {.mode = TrackFilterMode::PlainText, .expression = std::format("$text ~ \"{}\"", raw)};
    }
  } // namespace

  TrackFilterViewModel::TrackFilterViewModel(rt::ViewService& viewService,
                                             rt::WorkspaceService& workspaceService,
                                             std::function<void(TrackFilterViewState const&)> onRender)
    : _viewService{viewService}, _workspaceService{workspaceService}, _onRender{std::move(onRender)}
  {
    _focusSub = _workspaceService.onFocusedViewChanged([this](rt::ViewId viewId) { onFocusedViewChanged(viewId); });
    _filterStatusSub =
      _viewService.onFilterStatusChanged([this](auto const& status) { onFilterStatusChanged(status); });

    onFocusedViewChanged(_workspaceService.layoutState().activeViewId);
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
    _filterPending = true;

    if (resolved.mode == TrackFilterMode::None)
    {
      _viewService.setFilter(_viewId, "");
    }
    else
    {
      _viewService.setFilter(_viewId, resolved.expression);
    }

    refresh();
  }

  void TrackFilterViewModel::onFocusedViewChanged(rt::ViewId viewId)
  {
    _viewId = viewId;

    if (_viewId == rt::kInvalidViewId)
    {
      _entryText.clear();
      _resolvedExpression.clear();
      _filterPending = false;
      _optFilterError.reset();
      refresh();
      return;
    }

    auto const state = _viewService.trackListState(_viewId);
    _entryText = state.filterExpression;

    auto const resolved = resolveTrackFilterExpression(_entryText);
    _resolvedExpression = resolved.expression;
    _filterPending = false;
    _optFilterError.reset();

    refresh();
  }

  void TrackFilterViewModel::onFilterStatusChanged(rt::FilterStatusChanged const& status)
  {
    if (status.viewId != _viewId)
    {
      return;
    }

    _filterPending = status.pending;
    _optFilterError = status.optError;

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
      view.pending = _filterPending;

      if (_optFilterError)
      {
        view.hasError = true;
        view.tooltip = std::format("Expression error: {}", _optFilterError->message);
      }

      view.canCreateSmartList = !view.resolvedExpression.empty() && !view.pending && !view.hasError;
    }

    if (_onRender)
    {
      _onRender(view);
    }
  }
} // namespace ao::uimodel::track
