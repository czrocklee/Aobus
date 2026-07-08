// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/library/presentation/TrackPresentationPickerViewModel.h>

#include <functional>
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
    _focusSub = _workspace.onFocusedViewChanged(
      [this](rt::ViewId)
      {
        _optimisticViewId = rt::kInvalidViewId;
        _optimisticPresentationId.clear();
        refresh();
      });

    _presentationSub = _views.onPresentationChanged(
      [this](rt::ViewService::PresentationChanged const& ev)
      {
        if (ev.viewId != _workspace.layoutState().activeViewId)
        {
          return;
        }

        _optimisticViewId = rt::kInvalidViewId;
        _optimisticPresentationId.clear();
        refresh();
      });

    _catalogSub = _catalog.signalChanged().connect([this] { refresh(); });
  }

  TrackPresentationPickerState TrackPresentationPickerViewModel::state() const
  {
    auto result = TrackPresentationPickerState{
      .enabled = false,
      .activeViewId = rt::kInvalidViewId,
      .activeListId = kInvalidListId,
      .activePresentationId = {},
      .label = "Presentation",
      .menuItems = _catalog.menuItems(),
    };
    auto const activeViewId = _workspace.layoutState().activeViewId;

    if (activeViewId == rt::kInvalidViewId)
    {
      return result;
    }

    auto const viewState = _views.trackListState(activeViewId);
    auto presentationId = viewState.presentation.id;

    if (_optimisticViewId == activeViewId && !_optimisticPresentationId.empty())
    {
      presentationId = _optimisticPresentationId;
    }

    result.enabled = true;
    result.activeViewId = activeViewId;
    result.activeListId = viewState.listId;
    result.activePresentationId = presentationId;
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

  TrackPresentationApplyCommand TrackPresentationPickerViewModel::selectPresentation(std::string_view presentationId)
  {
    auto const activeViewId = _workspace.layoutState().activeViewId;

    if (activeViewId == rt::kInvalidViewId)
    {
      return {};
    }

    auto const optSpec = _catalog.specForId(presentationId);

    if (!optSpec)
    {
      return {};
    }

    if (auto const viewState = _views.trackListState(activeViewId); viewState.listId != kInvalidListId)
    {
      _preferences.setPresentationIdForList(viewState.listId, optSpec->id);
    }

    _optimisticViewId = activeViewId;
    _optimisticPresentationId = optSpec->id;
    refresh();

    return TrackPresentationApplyCommand{.shouldApply = true, .spec = *optSpec};
  }
} // namespace ao::uimodel
