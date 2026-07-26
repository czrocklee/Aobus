// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/library/presentation/TrackPresentationPickerViewModel.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel
{
  TrackPresentationPickerViewModel::TrackPresentationPickerViewModel(
    rt::ViewService& views,
    rt::WorkspaceService& workspace,
    TrackPresentationCatalog& catalog,
    ListPresentationPreferenceStore& preferences,
    std::function<void(TrackPresentationPickerState const&)> onRender)
    : _views{views}, _workspace{workspace}, _catalog{catalog}, _preferences{preferences}, _onRender{std::move(onRender)}
  {
    _observedViewId = _workspace.snapshot().activeViewId;
    _focusSub = _workspace.onChanged(
      [this](rt::WorkspaceChanged const& changed) noexcept
      {
        if (changed.snapshot.activeViewId == _observedViewId)
        {
          return;
        }

        _observedViewId = changed.snapshot.activeViewId;
        _optimisticViewId = rt::kInvalidViewId;
        _optimisticPresentationId.clear();
        refresh();
      });

    _presentationSub = _views.onPresentationChanged(
      [this](rt::ViewService::PresentationChanged const& ev) noexcept
      {
        if (ev.viewId != _workspace.snapshot().activeViewId)
        {
          return;
        }

        _optimisticViewId = rt::kInvalidViewId;
        _optimisticPresentationId.clear();
        refresh();
      });

    _catalogSub = _catalog.signalChanged().connect([this] noexcept { refresh(); });
  }

  TrackPresentationPickerState TrackPresentationPickerViewModel::state() const
  {
    auto result = TrackPresentationPickerState{
      .enabled = false,
      .activeViewId = rt::kInvalidViewId,
      .label = "Presentation",
      .menuItems = _catalog.menuItems(),
    };
    auto const activeViewId = _workspace.snapshot().activeViewId;

    if (activeViewId == rt::kInvalidViewId)
    {
      return result;
    }

    // Reached from three observers via refresh(), so the active view may already
    // be gone by the time the notification is delivered.
    auto const foundState = _views.findTrackListState(activeViewId);

    if (!foundState)
    {
      return result;
    }

    auto presentationId = foundState->presentation.id;

    if (_optimisticViewId == activeViewId && !_optimisticPresentationId.empty())
    {
      presentationId = _optimisticPresentationId;
    }

    result.enabled = true;
    result.activeViewId = activeViewId;
    result.label = _catalog.labelForId(presentationId);
    return result;
  }

  void TrackPresentationPickerViewModel::refresh()
  {
    if (_onRender)
    {
      _onRender(state());
    }
  }

  std::optional<TrackPresentationSelection> TrackPresentationPickerViewModel::selectPresentation(
    std::string_view presentationId)
  {
    auto const activeViewId = _workspace.snapshot().activeViewId;

    if (activeViewId == rt::kInvalidViewId)
    {
      return {};
    }

    auto const foundState = _views.findTrackListState(activeViewId);

    if (!foundState)
    {
      return {};
    }

    auto optSpec = _catalog.specForId(presentationId);

    if (!optSpec)
    {
      return {};
    }

    if (foundState->listId != kInvalidListId)
    {
      _preferences.setPresentationIdForList(foundState->listId, optSpec->id);
    }

    _optimisticViewId = activeViewId;
    _optimisticPresentationId = optSpec->id;
    refresh();

    return TrackPresentationSelection{.targetViewId = activeViewId, .spec = std::move(*optSpec)};
  }
} // namespace ao::uimodel
